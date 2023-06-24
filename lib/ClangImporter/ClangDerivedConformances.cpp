//===--- ClangDerivedConformances.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ClangDerivedConformances.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/Overload.h"

using namespace swift;
using namespace swift::importer;

/// Alternative to `NominalTypeDecl::lookupDirect`.
/// This function does not attempt to load extensions of the nominal decl.
static TinyPtrVector<ValueDecl *>
lookupDirectWithoutExtensions(NominalTypeDecl *decl, Identifier id) {
  ASTContext &ctx = decl->getASTContext();
  auto *importer = static_cast<ClangImporter *>(ctx.getClangModuleLoader());

  TinyPtrVector<ValueDecl *> result;

  if (id.isOperator()) {
    auto underlyingId =
        ctx.getIdentifier(getPrivateOperatorName(std::string(id)));
    TinyPtrVector<ValueDecl *> underlyingFuncs = evaluateOrDefault(
        ctx.evaluator, ClangRecordMemberLookup({decl, underlyingId}), {});
    for (auto it : underlyingFuncs) {
      if (auto synthesizedFunc =
              importer->getCXXSynthesizedOperatorFunc(cast<FuncDecl>(it)))
        result.push_back(synthesizedFunc);
    }
  } else {
    // See if there is a Clang decl with the given name.
    result = evaluateOrDefault(ctx.evaluator,
                               ClangRecordMemberLookup({decl, id}), {});
  }

  // Check if there are any synthesized Swift members that match the name.
  for (auto member : decl->getCurrentMembersWithoutLoading()) {
    if (auto namedMember = dyn_cast<ValueDecl>(member)) {
      if (namedMember->hasName() && !namedMember->getName().isSpecial() &&
          namedMember->getName().getBaseIdentifier().is(id.str()) &&
          // Make sure we don't add duplicate entries, as that would wrongly
          // imply that lookup is ambiguous.
          !llvm::is_contained(result, namedMember)) {
        result.push_back(namedMember);
      }
    }
  }
  return result;
}

template <typename Decl>
static Decl *lookupDirectSingleWithoutExtensions(NominalTypeDecl *decl,
                                                 Identifier id) {
  auto results = lookupDirectWithoutExtensions(decl, id);
  if (results.size() != 1)
    return nullptr;
  return dyn_cast<Decl>(results.front());
}

/// Similar to ModuleDecl::conformsToProtocol, but doesn't introduce a
/// dependency on Sema.
static bool isConcreteAndValid(ProtocolConformanceRef conformanceRef,
                               ModuleDecl *module) {
  if (conformanceRef.isInvalid())
    return false;
  if (!conformanceRef.isConcrete())
    return false;
  auto conformance = conformanceRef.getConcrete();
  auto subMap = conformance->getSubstitutionMap();
  return llvm::all_of(subMap.getConformances(),
                      [&](ProtocolConformanceRef each) -> bool {
                        return isConcreteAndValid(each, module);
                      });
}

static bool isStdDecl(const clang::CXXRecordDecl *clangDecl,
                      llvm::ArrayRef<StringRef> names) {
  if (!clangDecl->isInStdNamespace())
    return false;
  if (!clangDecl->getIdentifier())
    return false;
  StringRef name = clangDecl->getName();
  return llvm::is_contained(names, name);
}

static clang::TypeDecl *
getIteratorCategoryDecl(const clang::CXXRecordDecl *clangDecl) {
  clang::IdentifierInfo *iteratorCategoryDeclName =
      &clangDecl->getASTContext().Idents.get("iterator_category");
  auto iteratorCategories = clangDecl->lookup(iteratorCategoryDeclName);
  if (!iteratorCategories.isSingleResult())
    return nullptr;
  auto iteratorCategory = iteratorCategories.front();

  return dyn_cast_or_null<clang::TypeDecl>(iteratorCategory);
}

static ValueDecl *lookupOperator(NominalTypeDecl *decl, Identifier id,
                                 function_ref<bool(ValueDecl *)> isValid) {
  // First look for operator declared as a member.
  auto memberResults = lookupDirectWithoutExtensions(decl, id);
  for (const auto &member : memberResults) {
    if (isValid(member))
      return member;
  }

  // If no member operator was found, look for out-of-class definitions in the
  // same module.
  auto module = decl->getModuleContext();
  SmallVector<ValueDecl *> nonMemberResults;
  module->lookupValue(id, NLKind::UnqualifiedLookup, nonMemberResults);
  for (const auto &nonMember : nonMemberResults) {
    if (isValid(nonMember))
      return nonMember;
  }

  return nullptr;
}

static ValueDecl *getEqualEqualOperator(NominalTypeDecl *decl) {
  auto isValid = [&](ValueDecl *equalEqualOp) -> bool {
    auto equalEqual = dyn_cast<FuncDecl>(equalEqualOp);
    if (!equalEqual || !equalEqual->hasParameterList())
      return false;
    auto params = equalEqual->getParameters();
    if (params->size() != 2)
      return false;
    auto lhs = params->get(0);
    auto rhs = params->get(1);
    if (lhs->isInOut() || rhs->isInOut())
      return false;
    auto lhsTy = lhs->getType();
    auto rhsTy = rhs->getType();
    if (!lhsTy || !rhsTy)
      return false;
    auto lhsNominal = lhsTy->getAnyNominal();
    auto rhsNominal = rhsTy->getAnyNominal();
    if (lhsNominal != rhsNominal || lhsNominal != decl)
      return false;
    return true;
  };

  return lookupOperator(decl, decl->getASTContext().Id_EqualsOperator, isValid);
}

static ValueDecl *getMinusOperator(NominalTypeDecl *decl) {
  auto binaryIntegerProto =
      decl->getASTContext().getProtocol(KnownProtocolKind::BinaryInteger);
  auto module = decl->getModuleContext();

  auto isValid = [&](ValueDecl *minusOp) -> bool {
    auto minus = dyn_cast<FuncDecl>(minusOp);
    if (!minus || !minus->hasParameterList())
      return false;
    auto params = minus->getParameters();
    if (params->size() != 2)
      return false;
    auto lhs = params->get(0);
    auto rhs = params->get(1);
    if (lhs->isInOut() || rhs->isInOut())
      return false;
    auto lhsTy = lhs->getType();
    auto rhsTy = rhs->getType();
    if (!lhsTy || !rhsTy)
      return false;
    auto lhsNominal = lhsTy->getAnyNominal();
    auto rhsNominal = rhsTy->getAnyNominal();
    if (lhsNominal != rhsNominal || lhsNominal != decl)
      return false;
    auto returnTy = minus->getResultInterfaceType();
    auto conformanceRef =
        module->lookupConformance(returnTy, binaryIntegerProto);
    if (!isConcreteAndValid(conformanceRef, module))
      return false;
    return true;
  };

  return lookupOperator(decl, decl->getASTContext().getIdentifier("-"),
                        isValid);
}

static ValueDecl *getPlusEqualOperator(NominalTypeDecl *decl, Type distanceTy) {
  auto isValid = [&](ValueDecl *plusEqualOp) -> bool {
    auto plusEqual = dyn_cast<FuncDecl>(plusEqualOp);
    if (!plusEqual || !plusEqual->hasParameterList())
      return false;
    auto params = plusEqual->getParameters();
    if (params->size() != 2)
      return false;
    auto lhs = params->get(0);
    auto rhs = params->get(1);
    if (rhs->isInOut())
      return false;
    auto lhsTy = lhs->getType();
    auto rhsTy = rhs->getType();
    if (!lhsTy || !rhsTy)
      return false;
    if (rhsTy->getCanonicalType() != distanceTy->getCanonicalType())
      return false;
    auto lhsNominal = lhsTy->getAnyNominal();
    if (lhsNominal != decl)
      return false;
    auto returnTy = plusEqual->getResultInterfaceType();
    if (!returnTy->isVoid())
      return false;
    return true;
  };

  return lookupOperator(decl, decl->getASTContext().getIdentifier("+="),
                        isValid);
}

static void instantiateTemplatedOperator(
    ClangImporter::Implementation &impl,
    const clang::ClassTemplateSpecializationDecl *classDecl,
    clang::BinaryOperatorKind operatorKind) {

  clang::ASTContext &clangCtx = impl.getClangASTContext();
  clang::Sema &clangSema = impl.getClangSema();

  clang::UnresolvedSet<1> ops;
  auto qualType = clang::QualType(classDecl->getTypeForDecl(), 0);
  auto arg = new (clangCtx)
      clang::CXXThisExpr(clang::SourceLocation(), qualType, false);
  arg->setType(clang::QualType(classDecl->getTypeForDecl(), 0));

  clang::OverloadedOperatorKind opKind =
      clang::BinaryOperator::getOverloadedOperator(operatorKind);
  clang::OverloadCandidateSet candidateSet(
      classDecl->getLocation(), clang::OverloadCandidateSet::CSK_Operator,
      clang::OverloadCandidateSet::OperatorRewriteInfo(opKind,
                                              clang::SourceLocation(), false));
  clangSema.LookupOverloadedBinOp(candidateSet, opKind, ops, {arg, arg}, true);

  clang::OverloadCandidateSet::iterator best;
  switch (candidateSet.BestViableFunction(clangSema, clang::SourceLocation(),
                                          best)) {
  case clang::OR_Success: {
    if (auto clangCallee = best->Function) {
      auto lookupTable = impl.findLookupTable(classDecl);
      addEntryToLookupTable(*lookupTable, clangCallee, impl.getNameImporter());
    }
    break;
  }
  case clang::OR_No_Viable_Function:
  case clang::OR_Ambiguous:
  case clang::OR_Deleted:
    break;
  }
}

bool swift::isIterator(const clang::CXXRecordDecl *clangDecl) {
  return getIteratorCategoryDecl(clangDecl);
}

void swift::conformToCxxIteratorIfNeeded(
    ClangImporter::Implementation &impl, NominalTypeDecl *decl,
    const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to UnsafeCxxInputIterator", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  if (!ctx.getProtocol(KnownProtocolKind::UnsafeCxxInputIterator))
    return;

  // We consider a type to be an input iterator if it defines an
  // `iterator_category` that inherits from `std::input_iterator_tag`, e.g.
  // `using iterator_category = std::input_iterator_tag`.
  auto iteratorCategory = getIteratorCategoryDecl(clangDecl);
  if (!iteratorCategory)
    return;

  // If `iterator_category` is a typedef or a using-decl, retrieve the
  // underlying struct decl.
  clang::CXXRecordDecl *underlyingCategoryDecl = nullptr;
  if (auto typedefDecl = dyn_cast<clang::TypedefNameDecl>(iteratorCategory)) {
    auto type = typedefDecl->getUnderlyingType();
    underlyingCategoryDecl = type->getAsCXXRecordDecl();
  } else {
    underlyingCategoryDecl = dyn_cast<clang::CXXRecordDecl>(iteratorCategory);
  }
  if (underlyingCategoryDecl) {
    underlyingCategoryDecl = underlyingCategoryDecl->getDefinition();
  }

  if (!underlyingCategoryDecl)
    return;

  auto isIteratorCategoryDecl = [&](const clang::CXXRecordDecl *base,
                                    StringRef tag) {
    return base->isInStdNamespace() && base->getIdentifier() &&
           base->getName() == tag;
  };
  auto isInputIteratorDecl = [&](const clang::CXXRecordDecl *base) {
    return isIteratorCategoryDecl(base, "input_iterator_tag");
  };
  auto isRandomAccessIteratorDecl = [&](const clang::CXXRecordDecl *base) {
    return isIteratorCategoryDecl(base, "random_access_iterator_tag");
  };

  // Traverse all transitive bases of `underlyingDecl` to check if
  // it inherits from `std::input_iterator_tag`.
  bool isInputIterator = isInputIteratorDecl(underlyingCategoryDecl);
  bool isRandomAccessIterator =
      isRandomAccessIteratorDecl(underlyingCategoryDecl);
  underlyingCategoryDecl->forallBases([&](const clang::CXXRecordDecl *base) {
    if (isInputIteratorDecl(base)) {
      isInputIterator = true;
    }
    if (isRandomAccessIteratorDecl(base)) {
      isRandomAccessIterator = true;
      isInputIterator = true;
      return false;
    }
    return true;
  });

  if (!isInputIterator)
    return;

  // Check if present: `var pointee: Pointee { get }`
  auto pointeeId = ctx.getIdentifier("pointee");
  auto pointee = lookupDirectSingleWithoutExtensions<VarDecl>(decl, pointeeId);
  if (!pointee || pointee->isGetterMutating() || pointee->getType()->hasError())
    return;

  // Check if present: `func successor() -> Self`
  auto successorId = ctx.getIdentifier("successor");
  auto successor =
      lookupDirectSingleWithoutExtensions<FuncDecl>(decl, successorId);
  if (!successor || successor->isMutating())
    return;
  auto successorTy = successor->getResultInterfaceType();
  if (!successorTy || successorTy->getAnyNominal() != decl)
    return;

  // If this is a templated class, `operator==` might be templated as well.
  // Try to instantiate it.
  if (auto templateSpec =
          dyn_cast<clang::ClassTemplateSpecializationDecl>(clangDecl)) {
    instantiateTemplatedOperator(impl, templateSpec,
                                 clang::BinaryOperatorKind::BO_EQ);
  }
  // Check if present: `func ==`
  auto equalEqual = getEqualEqualOperator(decl);
  if (!equalEqual)
    return;

  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Pointee"),
                               pointee->getType());
  impl.addSynthesizedProtocolAttrs(decl,
                                   {KnownProtocolKind::UnsafeCxxInputIterator});
  if (!isRandomAccessIterator ||
      !ctx.getProtocol(KnownProtocolKind::UnsafeCxxRandomAccessIterator))
    return;

  // Try to conform to UnsafeCxxRandomAccessIterator if possible.

  if (auto templateSpec =
          dyn_cast<clang::ClassTemplateSpecializationDecl>(clangDecl)) {
    instantiateTemplatedOperator(impl, templateSpec,
                                 clang::BinaryOperatorKind::BO_Sub);
  }
  auto minus = dyn_cast_or_null<FuncDecl>(getMinusOperator(decl));
  if (!minus)
    return;
  auto distanceTy = minus->getResultInterfaceType();
  // distanceTy conforms to BinaryInteger, this is ensured by getMinusOperator.

  auto plusEqual = dyn_cast_or_null<FuncDecl>(getPlusEqualOperator(decl, distanceTy));
  if (!plusEqual)
    return;

  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Distance"), distanceTy);
  impl.addSynthesizedProtocolAttrs(
      decl, {KnownProtocolKind::UnsafeCxxRandomAccessIterator});
}

void swift::conformToCxxOptionalIfNeeded(
    ClangImporter::Implementation &impl, NominalTypeDecl *decl,
    const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to CxxOptional", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  if (!isStdDecl(clangDecl, {"optional"}))
    return;

  ProtocolDecl *cxxOptionalProto =
      ctx.getProtocol(KnownProtocolKind::CxxOptional);
  // If the Cxx module is missing, or does not include one of the necessary
  // protocol, bail.
  if (!cxxOptionalProto)
    return;

  auto pointeeId = ctx.getIdentifier("pointee");
  auto pointees = lookupDirectWithoutExtensions(decl, pointeeId);
  if (pointees.size() != 1)
    return;
  auto pointee = dyn_cast<VarDecl>(pointees.front());
  if (!pointee)
    return;
  auto pointeeTy = pointee->getInterfaceType();

  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Wrapped"), pointeeTy);
  impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxOptional});
}

void swift::conformToCxxSequenceIfNeeded(
    ClangImporter::Implementation &impl, NominalTypeDecl *decl,
    const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to CxxSequence", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  ProtocolDecl *cxxIteratorProto =
      ctx.getProtocol(KnownProtocolKind::UnsafeCxxInputIterator);
  ProtocolDecl *cxxSequenceProto =
      ctx.getProtocol(KnownProtocolKind::CxxSequence);
  ProtocolDecl *cxxConvertibleProto =
      ctx.getProtocol(KnownProtocolKind::CxxConvertibleToCollection);
  // If the Cxx module is missing, or does not include one of the necessary
  // protocols, bail.
  if (!cxxIteratorProto || !cxxSequenceProto)
    return;

  // Check if present: `func __beginUnsafe() -> RawIterator`
  auto beginId = ctx.getIdentifier("__beginUnsafe");
  auto begin = lookupDirectSingleWithoutExtensions<FuncDecl>(decl, beginId);
  if (!begin)
    return;
  auto rawIteratorTy = begin->getResultInterfaceType();

  // Check if present: `func __endUnsafe() -> RawIterator`
  auto endId = ctx.getIdentifier("__endUnsafe");
  auto end = lookupDirectSingleWithoutExtensions<FuncDecl>(decl, endId);
  if (!end)
    return;

  // Check if `begin()` and `end()` are non-mutating.
  if (begin->isMutating() || end->isMutating())
    return;

  // Check if `__beginUnsafe` and `__endUnsafe` have the same return type.
  auto endTy = end->getResultInterfaceType();
  if (!endTy || endTy->getCanonicalType() != rawIteratorTy->getCanonicalType())
    return;

  // Check if RawIterator conforms to UnsafeCxxInputIterator.
  ModuleDecl *module = decl->getModuleContext();
  auto rawIteratorConformanceRef =
      module->lookupConformance(rawIteratorTy, cxxIteratorProto);
  if (!isConcreteAndValid(rawIteratorConformanceRef, module))
    return;
  auto rawIteratorConformance = rawIteratorConformanceRef.getConcrete();
  auto pointeeDecl =
      cxxIteratorProto->getAssociatedType(ctx.getIdentifier("Pointee"));
  assert(pointeeDecl &&
         "UnsafeCxxInputIterator must have a Pointee associated type");
  auto pointeeTy = rawIteratorConformance->getTypeWitness(pointeeDecl);
  assert(pointeeTy && "valid conformance must have a Pointee witness");

  // Take the default definition of `Iterator` from CxxSequence protocol. This
  // type is currently `CxxIterator<Self>`.
  auto iteratorDecl = cxxSequenceProto->getAssociatedType(ctx.Id_Iterator);
  auto iteratorTy = iteratorDecl->getDefaultDefinitionType();
  // Substitute generic `Self` parameter.
  auto cxxSequenceSelfTy = cxxSequenceProto->getSelfInterfaceType();
  auto declSelfTy = decl->getDeclaredInterfaceType();
  iteratorTy = iteratorTy.subst(
      [&](SubstitutableType *dependentType) {
        if (dependentType->isEqual(cxxSequenceSelfTy))
          return declSelfTy;
        return Type(dependentType);
      },
      LookUpConformanceInModule(module));

  impl.addSynthesizedTypealias(decl, ctx.Id_Element, pointeeTy);
  impl.addSynthesizedTypealias(decl, ctx.Id_Iterator, iteratorTy);
  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("RawIterator"),
                               rawIteratorTy);
  // Not conforming the type to CxxSequence protocol here:
  // The current implementation of CxxSequence triggers extra copies of the C++
  // collection when creating a CxxIterator instance. It needs a more efficient
  // implementation, which is not possible with the existing Swift features.
  // impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxSequence});

  // Try to conform to CxxRandomAccessCollection if possible.

  auto tryToConformToRandomAccessCollection = [&]() -> bool {
    auto cxxRAIteratorProto =
        ctx.getProtocol(KnownProtocolKind::UnsafeCxxRandomAccessIterator);
    if (!cxxRAIteratorProto ||
        !ctx.getProtocol(KnownProtocolKind::CxxRandomAccessCollection))
      return false;

    // Check if RawIterator conforms to UnsafeCxxRandomAccessIterator.
    auto rawIteratorRAConformanceRef =
        decl->getModuleContext()->lookupConformance(rawIteratorTy,
                                                    cxxRAIteratorProto);
    if (!isConcreteAndValid(rawIteratorRAConformanceRef, module))
      return false;

    // CxxRandomAccessCollection always uses Int as an Index.
    auto indexTy = ctx.getIntType();

    auto sliceTy = ctx.getSliceType();
    sliceTy = sliceTy.subst(
        [&](SubstitutableType *dependentType) {
          if (dependentType->isEqual(cxxSequenceSelfTy))
            return declSelfTy;
          return Type(dependentType);
        },
        LookUpConformanceInModule(module));

    auto indicesTy = ctx.getRangeType();
    indicesTy = indicesTy.subst(
        [&](SubstitutableType *dependentType) {
          if (dependentType->isEqual(cxxSequenceSelfTy))
            return indexTy;
          return Type(dependentType);
        },
        LookUpConformanceInModule(module));

    impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Element"), pointeeTy);
    impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Index"), indexTy);
    impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Indices"), indicesTy);
    impl.addSynthesizedTypealias(decl, ctx.getIdentifier("SubSequence"),
                                 sliceTy);
    impl.addSynthesizedProtocolAttrs(
        decl, {KnownProtocolKind::CxxRandomAccessCollection});
    return true;
  };

  bool conformedToRAC = tryToConformToRandomAccessCollection();

  // If the collection does not support random access, let's still allow the
  // developer to explicitly convert a C++ sequence to a Swift Array (making a
  // copy of the sequence's elements) by conforming the type to
  // CxxCollectionConvertible. This enables an overload of Array.init declared
  // in the Cxx module.
  if (!conformedToRAC && cxxConvertibleProto) {
    impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Element"), pointeeTy);
    impl.addSynthesizedProtocolAttrs(
        decl, {KnownProtocolKind::CxxConvertibleToCollection});
  }
}

void swift::conformToCxxSetIfNeeded(ClangImporter::Implementation &impl,
                                    NominalTypeDecl *decl,
                                    const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to CxxSet", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  // Only auto-conform types from the C++ standard library. Custom user types
  // might have a similar interface but different semantics.
  if (!isStdDecl(clangDecl, {"set", "unordered_set", "multiset"}))
    return;

  auto valueType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("value_type"));
  auto sizeType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("size_type"));
  if (!valueType || !sizeType)
    return;

  auto insertId = ctx.getIdentifier("__insertUnsafe");
  auto inserts = lookupDirectWithoutExtensions(decl, insertId);
  FuncDecl *insert = nullptr;
  for (auto candidate : inserts) {
    if (auto candidateMethod = dyn_cast<FuncDecl>(candidate)) {
      if (!candidateMethod->hasParameterList())
        continue;
      auto params = candidateMethod->getParameters();
      if (params->size() != 1)
        continue;
      auto param = params->front();
      if (param->getType()->getCanonicalType() !=
          valueType->getUnderlyingType()->getCanonicalType())
        continue;
      insert = candidateMethod;
      break;
    }
  }
  if (!insert)
    return;

  impl.addSynthesizedTypealias(decl, ctx.Id_Element,
                               valueType->getUnderlyingType());
  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Size"),
                               sizeType->getUnderlyingType());
  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("InsertionResult"),
                               insert->getResultInterfaceType());
  impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxSet});

  // If this isn't a std::multiset, try to also synthesize the conformance to
  // CxxUniqueSet.
  if (!isStdDecl(clangDecl, {"set", "unordered_set"}))
    return;

  ProtocolDecl *cxxIteratorProto =
      ctx.getProtocol(KnownProtocolKind::UnsafeCxxInputIterator);
  if (!cxxIteratorProto)
    return;

  auto rawMutableIteratorType =
      lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
          decl, ctx.getIdentifier("iterator"));
  if (!rawMutableIteratorType)
    return;

  auto rawMutableIteratorTy = rawMutableIteratorType->getUnderlyingType();
  // Check if RawMutableIterator conforms to UnsafeCxxInputIterator.
  ModuleDecl *module = decl->getModuleContext();
  auto rawIteratorConformanceRef =
      module->lookupConformance(rawMutableIteratorTy, cxxIteratorProto);
  if (!isConcreteAndValid(rawIteratorConformanceRef, module))
    return;

  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("RawMutableIterator"),
                               rawMutableIteratorTy);
  impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxUniqueSet});
}

void swift::conformToCxxPairIfNeeded(ClangImporter::Implementation &impl,
                                     NominalTypeDecl *decl,
                                     const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to CxxPair", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  // Only auto-conform types from the C++ standard library. Custom user types
  // might have a similar interface but different semantics.
  if (!isStdDecl(clangDecl, {"pair"}))
    return;

  auto firstType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("first_type"));
  auto secondType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("second_type"));
  if (!firstType || !secondType)
    return;

  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("First"),
                               firstType->getUnderlyingType());
  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("Second"),
                               secondType->getUnderlyingType());
  impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxPair});
}

void swift::conformToCxxDictionaryIfNeeded(
    ClangImporter::Implementation &impl, NominalTypeDecl *decl,
    const clang::CXXRecordDecl *clangDecl) {
  PrettyStackTraceDecl trace("conforming to CxxDictionary", decl);

  assert(decl);
  assert(clangDecl);
  ASTContext &ctx = decl->getASTContext();

  // Only auto-conform types from the C++ standard library. Custom user types
  // might have a similar interface but different semantics.
  if (!isStdDecl(clangDecl, {"map", "unordered_map"}))
    return;

  auto keyType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("key_type"));
  auto valueType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("mapped_type"));
  auto iterType = lookupDirectSingleWithoutExtensions<TypeAliasDecl>(
      decl, ctx.getIdentifier("const_iterator"));
  if (!keyType || !valueType || !iterType)
    return;

  // Make the original subscript that returns a non-optional value unavailable.
  // CxxDictionary adds another subscript that returns an optional value,
  // similarly to Swift.Dictionary.
  for (auto member : decl->getCurrentMembersWithoutLoading()) {
    if (auto subscript = dyn_cast<SubscriptDecl>(member)) {
      impl.markUnavailable(subscript,
                           "use subscript with optional return value");
    }
  }

  impl.addSynthesizedTypealias(decl, ctx.Id_Key, keyType->getUnderlyingType());
  impl.addSynthesizedTypealias(decl, ctx.Id_Value,
                               valueType->getUnderlyingType());
  impl.addSynthesizedTypealias(decl, ctx.getIdentifier("RawIterator"),
                               iterType->getUnderlyingType());
  impl.addSynthesizedProtocolAttrs(decl, {KnownProtocolKind::CxxDictionary});
}

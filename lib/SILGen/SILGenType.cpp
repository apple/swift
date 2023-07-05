//===--- SILGenType.cpp - SILGen for types and their members --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains code for emitting code associated with types:
//   - methods
//   - vtables and vtable thunks
//   - witness tables and witness thunks
//
//===----------------------------------------------------------------------===//

#include "ManagedValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILVTableVisitor.h"
#include "swift/SIL/SILWitnessVisitor.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

llvm::Optional<SILVTable::Entry>
SILGenModule::emitVTableMethod(ClassDecl *theClass, SILDeclRef derived,
                               SILDeclRef base) {
  assert(base.kind == derived.kind);

  auto *baseDecl = cast<AbstractFunctionDecl>(base.getDecl());
  auto *derivedDecl = cast<AbstractFunctionDecl>(derived.getDecl());

  // Note: We intentionally don't support extension members here.
  //
  // Once extensions can override or introduce new vtable entries, this will
  // all likely change anyway.
  auto *baseClass = cast<ClassDecl>(baseDecl->getDeclContext());
  auto *derivedClass = cast<ClassDecl>(derivedDecl->getDeclContext());

  // Figure out if the vtable entry comes from the superclass, in which
  // case we won't emit it if building a resilient module.
  SILVTable::Entry::Kind implKind;
  if (baseClass == theClass) {
    // This is a vtable entry for a method of the immediate class.
    implKind = SILVTable::Entry::Kind::Normal;
  } else if (derivedClass == theClass) {
    // This is a vtable entry for a method of a base class, but it is being
    // overridden in the immediate class.
    implKind = SILVTable::Entry::Kind::Override;
  } else {
    // This vtable entry is copied from the superclass.
    implKind = SILVTable::Entry::Kind::Inherited;

    // If the override is defined in a class from a different resilience
    // domain, don't emit the vtable entry.
    if (derivedClass->isResilient(M.getSwiftModule(),
                                  ResilienceExpansion::Maximal)) {
      return llvm::None;
    }
  }

  SILFunction *implFn;

  // If the member is dynamic, reference its dynamic dispatch thunk so that
  // it will be redispatched, funneling the method call through the runtime
  // hook point.
  bool usesObjCDynamicDispatch =
      (derivedDecl->shouldUseObjCDispatch() &&
       derived.kind != SILDeclRef::Kind::Allocator);

  if (usesObjCDynamicDispatch) {
    implFn = getDynamicThunk(
        derived, Types.getConstantInfo(TypeExpansionContext::minimal(), derived)
                     .SILFnType);
  } else if (auto *derivativeId = derived.getDerivativeFunctionIdentifier()) {
    // For JVP/VJP methods, create a vtable entry thunk. The thunk contains an
    // `differentiable_function` instruction, which is later filled during the
    // differentiation transform.
    auto derivedFnType =
        Types.getConstantInfo(TypeExpansionContext::minimal(), derived)
            .SILFnType;
    implFn = getOrCreateDerivativeVTableThunk(derived, derivedFnType);
  } else {
    implFn = getFunction(derived, NotForDefinition);
  }

  // As a fast path, if there is no override, definitely no thunk is necessary.
  if (derived == base)
    return SILVTable::Entry(base, implFn, implKind, false);

  // If the base method is less visible than the derived method, we need
  // a thunk.
  bool baseLessVisibleThanDerived =
    (!usesObjCDynamicDispatch &&
     !derivedDecl->isFinal() &&
     derivedDecl->isMoreVisibleThan(baseDecl));

  // Determine the derived thunk type by lowering the derived type against the
  // abstraction pattern of the base.
  auto baseInfo = Types.getConstantInfo(TypeExpansionContext::minimal(), base);
  auto derivedInfo =
      Types.getConstantInfo(TypeExpansionContext::minimal(), derived);
  auto basePattern = AbstractionPattern(baseInfo.LoweredType);

  auto overrideInfo = M.Types.getConstantOverrideInfo(
      TypeExpansionContext::minimal(), derived, base);

  // If base method's generic requirements are not satisfied by the derived
  // method then we need a thunk.
  using Direction = ASTContext::OverrideGenericSignatureReqCheck;
  auto doesNotHaveGenericRequirementDifference =
      getASTContext().overrideGenericSignatureReqsSatisfied(
          baseDecl, derivedDecl, Direction::BaseReqSatisfiedByDerived);

  // The override member type is semantically a subtype of the base
  // member type. If the override is ABI compatible, we do not need
  // a thunk.
  bool compatibleCallingConvention;
  switch (M.Types.checkFunctionForABIDifferences(M,
                                                 derivedInfo.SILFnType,
                                                 overrideInfo.SILFnType)) {
  case TypeConverter::ABIDifference::CompatibleCallingConvention:
  case TypeConverter::ABIDifference::CompatibleRepresentation:
    compatibleCallingConvention = true;
    break;
  case TypeConverter::ABIDifference::NeedsThunk:
    compatibleCallingConvention = false;
    break;
  case TypeConverter::ABIDifference::CompatibleCallingConvention_ThinToThick:
  case TypeConverter::ABIDifference::CompatibleRepresentation_ThinToThick:
    llvm_unreachable("shouldn't be thick methods");
  }
  if (doesNotHaveGenericRequirementDifference
      && !baseLessVisibleThanDerived
      && compatibleCallingConvention)
    return SILVTable::Entry(base, implFn, implKind, false);

  // Generate the thunk name.
  std::string name;
  {
    Mangle::ASTMangler mangler;
    if (isa<FuncDecl>(baseDecl)) {
      name = mangler.mangleVTableThunk(
        cast<FuncDecl>(baseDecl),
        cast<FuncDecl>(derivedDecl));
    } else {
      name = mangler.mangleConstructorVTableThunk(
        cast<ConstructorDecl>(baseDecl),
        cast<ConstructorDecl>(derivedDecl),
        base.kind == SILDeclRef::Kind::Allocator);
    }
    // TODO(TF-685): Use proper autodiff thunk mangling.
    if (auto *derivativeId = derived.getDerivativeFunctionIdentifier()) {
      switch (derivativeId->getKind()) {
      case AutoDiffDerivativeFunctionKind::JVP:
        name += "_jvp";
        break;
      case AutoDiffDerivativeFunctionKind::VJP:
        name += "_vjp";
        break;
      }
    }
  }

  // If we already emitted this thunk, reuse it.
  if (auto existingThunk = M.lookUpFunction(name))
    return SILVTable::Entry(base, existingThunk, implKind, false);

  auto *genericEnv = overrideInfo.FormalType.getOptGenericSignature().getGenericEnvironment();

  // Emit the thunk.
  SILLocation loc(derivedDecl);
  SILGenFunctionBuilder builder(*this);
  auto thunk = builder.createFunction(
      SILLinkage::Private, name, overrideInfo.SILFnType,
      genericEnv, loc,
      IsBare, IsNotTransparent, IsNotSerialized, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible, ProfileCounter(), IsThunk);
  thunk->setDebugScope(new (M) SILDebugScope(loc, thunk));

  PrettyStackTraceSILFunction trace("generating vtable thunk", thunk);

  SILGenFunction(*this, *thunk, theClass)
    .emitVTableThunk(base, derived, implFn, basePattern,
                     overrideInfo.LoweredType,
                     derivedInfo.LoweredType,
                     baseLessVisibleThanDerived);
  emitLazyConformancesForFunction(thunk);

  return SILVTable::Entry(base, thunk, implKind, false);
}

bool SILGenModule::requiresObjCMethodEntryPoint(FuncDecl *method) {
  // Property accessors should be generated alongside the property unless
  // the @NSManaged attribute is present.
  if (auto accessor = dyn_cast<AccessorDecl>(method)) {
    if (accessor->isGetterOrSetter()) {
      auto asd = accessor->getStorage();
      return asd->isObjC() && !asd->getAttrs().hasAttribute<NSManagedAttr>() &&
             !method->isNativeMethodReplacement();
    }
  }

  if (method->getAttrs().hasAttribute<NSManagedAttr>())
    return false;
  if (!method->isObjC())
    return false;

  // Don't emit the objective c entry point of @_dynamicReplacement(for:)
  // methods in generic classes. There is no way to call it.
  return !method->isNativeMethodReplacement();
}

bool SILGenModule::requiresObjCMethodEntryPoint(ConstructorDecl *constructor) {
  if (!constructor->isObjC())
    return false;
  // Don't emit the objective c entry point of @_dynamicReplacement(for:)
  // methods in generic classes. There is no way to call it.
  return !constructor->isNativeMethodReplacement();
}

namespace {

/// An ASTVisitor for populating SILVTable entries from ClassDecl members.
class SILGenVTable : public SILVTableVisitor<SILGenVTable> {
public:
  SILGenModule &SGM;
  ClassDecl *theClass;
  bool isResilient;

  // Map a base SILDeclRef to the corresponding element in vtableMethods.
  llvm::DenseMap<SILDeclRef, unsigned> baseToIndexMap;

  // For each base method, store the corresponding override.
  SmallVector<std::pair<SILDeclRef, SILDeclRef>, 8> vtableMethods;

  SILGenVTable(SILGenModule &SGM, ClassDecl *theClass)
    : SGM(SGM), theClass(theClass) {
    isResilient = theClass->isResilient();
  }

  void emitVTable() {
    // Imported types don't have vtables right now.
    if (theClass->hasClangNode())
      return;

    // Populate our list of base methods and overrides.
    visitAncestor(theClass);

    SmallVector<SILVTable::Entry, 8> vtableEntries;
    vtableEntries.reserve(vtableMethods.size() + 2);

    // For each base method/override pair, emit a vtable thunk or direct
    // reference to the method implementation.
    for (auto method : vtableMethods) {
      SILDeclRef baseRef, derivedRef;
      std::tie(baseRef, derivedRef) = method;

      auto entry = SGM.emitVTableMethod(theClass, derivedRef, baseRef);

      // We might skip emitting entries if the base class is resilient.
      if (entry)
        vtableEntries.push_back(*entry);
    }

    // Add the deallocating destructor to the vtable just for the purpose
    // that it is referenced and cannot be eliminated by dead function removal.
    // In reality, the deallocating destructor is referenced directly from
    // the HeapMetadata for the class.
    {
      auto *dtor = theClass->getDestructor();
      SILDeclRef dtorRef(dtor, SILDeclRef::Kind::Deallocator);
      auto *dtorFn = SGM.getFunction(dtorRef, NotForDefinition);
      vtableEntries.emplace_back(dtorRef, dtorFn,
                                 SILVTable::Entry::Kind::Normal,
                                 false);
    }

    if (SGM.requiresIVarDestroyer(theClass)) {
      SILDeclRef dtorRef(theClass, SILDeclRef::Kind::IVarDestroyer);
      auto *dtorFn = SGM.getFunction(dtorRef, NotForDefinition);
      vtableEntries.emplace_back(dtorRef, dtorFn,
                                 SILVTable::Entry::Kind::Normal,
                                 false);
    }

    IsSerialized_t serialized = IsNotSerialized;
    auto classIsPublic = theClass->getEffectiveAccess() >= AccessLevel::Public;
    // Only public, fixed-layout classes should have serialized vtables.
    if (classIsPublic && !isResilient)
      serialized = IsSerialized;

    // Finally, create the vtable.
    SILVTable::create(SGM.M, theClass, serialized, vtableEntries);
  }

  void visitAncestor(ClassDecl *ancestor) {
    auto *superDecl = ancestor->getSuperclassDecl();
    if (superDecl)
      visitAncestor(superDecl);

    addVTableEntries(ancestor);
  }

  // Try to find an overridden entry.
  void addMethodOverride(SILDeclRef baseRef, SILDeclRef declRef) {
    auto found = baseToIndexMap.find(baseRef);
    assert(found != baseToIndexMap.end());
    auto &method = vtableMethods[found->second];
    assert(method.first == baseRef);
    method.second = declRef;
  }

  // Add an entry to the vtable.
  void addMethod(SILDeclRef member) {
    unsigned index = vtableMethods.size();
    vtableMethods.push_back(std::make_pair(member, member));
    auto result = baseToIndexMap.insert(std::make_pair(member, index));
    assert(result.second);
    (void) result;
  }

  void addPlaceholder(MissingMemberDecl *m) {
#ifndef NDEBUG
    auto *classDecl = cast<ClassDecl>(m->getDeclContext());
    bool isResilient =
        classDecl->isResilient(SGM.M.getSwiftModule(),
                               ResilienceExpansion::Maximal);
    assert(isResilient || m->getNumberOfVTableEntries() == 0 &&
           "Should not be emitting fragile class with missing members");
#endif
  }
};

} // end anonymous namespace

static void emitTypeMemberGlobalVariable(SILGenModule &SGM,
                                         VarDecl *var) {
  if (var->getDeclContext()->isGenericContext()) {
    assert(var->getDeclContext()->getGenericSignatureOfContext()
              ->areAllParamsConcrete()
           && "generic static vars are not implemented yet");
  }

  if (var->getDeclContext()->getSelfClassDecl()) {
    assert(var->isFinal() && "only 'static' ('class final') stored properties are implemented in classes");
  }

  SGM.addGlobalVariable(var);
}

namespace {

// Is this a free function witness satisfying a static method requirement?
static IsFreeFunctionWitness_t isFreeFunctionWitness(ValueDecl *requirement,
                                                     ValueDecl *witness) {
  if (!witness->getDeclContext()->isTypeContext()) {
    assert(!requirement->isInstanceMember()
           && "free function satisfying instance method requirement?!");
    return IsFreeFunctionWitness;
  }

  return IsNotFreeFunctionWitness;
}

/// A CRTP class for emitting witness thunks for the requirements of a
/// protocol.
///
/// There are two subclasses:
///
/// - SILGenConformance: emits witness thunks for a conformance of a
///   a concrete type to a protocol
/// - SILGenDefaultWitnessTable: emits default witness thunks for
///   default implementations of protocol requirements
///
template<typename T> class SILGenWitnessTable : public SILWitnessVisitor<T> {
  T &asDerived() { return *static_cast<T*>(this); }

public:
  void addMethod(SILDeclRef requirementRef) {
    auto reqDecl = requirementRef.getDecl();

    // Static functions can be witnessed by enum cases with payload
    if (!(isa<AccessorDecl>(reqDecl) || isa<ConstructorDecl>(reqDecl))) {
      auto FD = cast<FuncDecl>(reqDecl);
      if (auto witness = asDerived().getWitness(FD)) {
        if (auto EED = dyn_cast<EnumElementDecl>(witness.getDecl())) {
          return addMethodImplementation(
              requirementRef, SILDeclRef(EED, SILDeclRef::Kind::EnumElement),
              witness);
        }
      }
    }

    auto reqAccessor = dyn_cast<AccessorDecl>(reqDecl);

    // If it's not an accessor, just look for the witness.
    if (!reqAccessor) {
      if (auto witness = asDerived().getWitness(reqDecl)) {
        auto newDecl = requirementRef.withDecl(witness.getDecl());
        // Only import C++ methods as foreign. If the following
        // Objective-C function is imported as foreign:
        //   () -> String
        // It will be imported as the following type:
        //   () -> NSString
        // But the first is correct, so make sure we don't mark this witness
        // as foreign.
        if (dyn_cast_or_null<clang::CXXMethodDecl>(
                witness.getDecl()->getClangDecl()))
          newDecl = newDecl.asForeign();
        return addMethodImplementation(
            requirementRef, getWitnessRef(newDecl, witness),
            witness);
      }

      return asDerived().addMissingMethod(requirementRef);
    }

    // Otherwise, we need to map the storage declaration and then get
    // the appropriate accessor for it.
    auto witness = asDerived().getWitness(reqAccessor->getStorage());
    if (!witness)
      return asDerived().addMissingMethod(requirementRef);

    // Static properties can be witnessed by enum cases without payload
    if (auto EED = dyn_cast<EnumElementDecl>(witness.getDecl())) {
      return addMethodImplementation(
          requirementRef, SILDeclRef(EED, SILDeclRef::Kind::EnumElement),
          witness);
    }

    auto witnessStorage = cast<AbstractStorageDecl>(witness.getDecl());
    if (reqAccessor->isSetter() && !witnessStorage->supportsMutation())
      return asDerived().addMissingMethod(requirementRef);

    auto witnessAccessor =
      witnessStorage->getSynthesizedAccessor(reqAccessor->getAccessorKind());

    return addMethodImplementation(
        requirementRef, getWitnessRef(requirementRef, witnessAccessor),
        witness);
  }

private:
  void addMethodImplementation(SILDeclRef requirementRef,
                               SILDeclRef witnessRef,
                               Witness witness) {
    // Free function witnesses have an implicit uncurry layer imposed on them by
    // the inserted metatype argument.
    auto isFree =
      isFreeFunctionWitness(requirementRef.getDecl(), witnessRef.getDecl());
    asDerived().addMethodImplementation(requirementRef, witnessRef,
                                        isFree, witness);
  }

  SILDeclRef getWitnessRef(SILDeclRef requirementRef, Witness witness) {
    auto witnessRef = requirementRef.withDecl(witness.getDecl());
    // If the requirement/witness is a derivative function, we need to
    // substitute the witness's derivative generic signature in its derivative
    // function identifier.
    if (requirementRef.isAutoDiffDerivativeFunction()) {
      auto *reqrDerivativeId = requirementRef.getDerivativeFunctionIdentifier();
      auto *witnessDerivativeId = AutoDiffDerivativeFunctionIdentifier::get(
          reqrDerivativeId->getKind(), reqrDerivativeId->getParameterIndices(),
          witness.getDerivativeGenericSignature(), witnessRef.getASTContext());
      witnessRef = witnessRef.asAutoDiffDerivativeFunction(witnessDerivativeId);
    }

    return witnessRef;
  }
};

static IsSerialized_t isConformanceSerialized(RootProtocolConformance *conf) {
  return SILWitnessTable::conformanceIsSerialized(conf)
           ? IsSerialized : IsNotSerialized;
}

/// Emit a witness table for a protocol conformance.
class SILGenConformance : public SILGenWitnessTable<SILGenConformance> {
  using super = SILGenWitnessTable<SILGenConformance>;

public:
  SILGenModule &SGM;
  NormalProtocolConformance *Conformance;
  std::vector<SILWitnessTable::Entry> Entries;
  std::vector<SILWitnessTable::ConditionalConformance> ConditionalConformances;
  SILLinkage Linkage;
  IsSerialized_t Serialized;

  SILGenConformance(SILGenModule &SGM, NormalProtocolConformance *C)
    : SGM(SGM), Conformance(C),
      Linkage(getLinkageForProtocolConformance(Conformance,
                                               ForDefinition)),
      Serialized(isConformanceSerialized(Conformance))
  {
    auto *proto = Conformance->getProtocol();

    // Not all protocols use witness tables; in this case we just skip
    // all of emit() below completely.
    if (!Lowering::TypeConverter::protocolRequiresWitnessTable(proto))
      Conformance = nullptr;
  }

  SILWitnessTable *emit() {
    // Nothing to do if this wasn't a normal conformance.
    if (!Conformance)
      return nullptr;

    PrettyStackTraceConformance trace("generating SIL witness table",
                                      Conformance);

    auto *proto = Conformance->getProtocol();
    visitProtocolDecl(proto);

    addConditionalRequirements();

    // Check if we already have a declaration or definition for this witness
    // table.
    if (auto *wt = SGM.M.lookUpWitnessTable(Conformance)) {
      // If we have a definition already, just return it.
      //
      // FIXME: I am not sure if this is possible, if it is not change this to an
      // assert.
      if (wt->isDefinition())
        return wt;

      // If we have a declaration, convert the witness table to a definition.
      if (wt->isDeclaration()) {
        wt->convertToDefinition(Entries, ConditionalConformances, Serialized);

        // Since we had a declaration before, its linkage should be external,
        // ensure that we have a compatible linkage for sanity. *NOTE* we are ok
        // with both being shared since we do not have a shared_external
        // linkage.
        assert(stripExternalFromLinkage(wt->getLinkage()) == Linkage &&
               "Witness table declaration has inconsistent linkage with"
               " silgen definition.");

        // And then override the linkage with the new linkage.
        wt->setLinkage(Linkage);
        return wt;
      }
    }

    // Otherwise if we have no witness table yet, create it.
    return SILWitnessTable::create(SGM.M, Linkage, Serialized, Conformance,
                                   Entries, ConditionalConformances);
  }

  void addProtocolConformanceDescriptor() {
  }


  void addOutOfLineBaseProtocol(ProtocolDecl *baseProtocol) {
    assert(Lowering::TypeConverter::protocolRequiresWitnessTable(baseProtocol));

    auto conformance = Conformance->getInheritedConformance(baseProtocol);

    Entries.push_back(SILWitnessTable::BaseProtocolWitness{
      baseProtocol,
      conformance,
    });

    // Emit the witness table for the base conformance if it is shared.
    SGM.useConformance(ProtocolConformanceRef(conformance));
  }

  Witness getWitness(ValueDecl *decl) {
    return Conformance->getWitness(decl);
  }

  void addPlaceholder(MissingMemberDecl *placeholder) {
    llvm_unreachable("generating a witness table with placeholders in it");
  }

  void addMissingMethod(SILDeclRef requirement) {
    llvm_unreachable("generating a witness table with placeholders in it");
  }

  void addMethodImplementation(SILDeclRef requirementRef,
                               SILDeclRef witnessRef,
                               IsFreeFunctionWitness_t isFree,
                               Witness witness) {
    // Emit the witness thunk and add it to the table.
    auto witnessLinkage = witnessRef.getLinkage(ForDefinition);
    auto witnessSerialized = Serialized;
    if (witnessSerialized &&
        fixmeWitnessHasLinkageThatNeedsToBePublic(witnessRef)) {
      witnessLinkage = SILLinkage::Public;
      witnessSerialized = IsNotSerialized;
    } else {
      // This is the "real" rule; the above case should go away once we
      // figure out what's going on.

      // Normally witness thunks can be private.
      witnessLinkage = SILLinkage::Private;

      // Unless the witness table is going to be serialized.
      if (witnessSerialized)
        witnessLinkage = SILLinkage::Shared;

      // Or even if its not serialized, it might be for an imported
      // conformance in which case it can be emitted multiple times.
      if (Linkage == SILLinkage::Shared)
        witnessLinkage = SILLinkage::Shared;
    }

    if (isa<EnumElementDecl>(witnessRef.getDecl())) {
      assert(witnessRef.isEnumElement() && "Witness decl, but different kind?");
    }

    SILFunction *witnessFn = SGM.emitProtocolWitness(
        ProtocolConformanceRef(Conformance), witnessLinkage, witnessSerialized,
        requirementRef, witnessRef, isFree, witness);
    Entries.push_back(
                    SILWitnessTable::MethodWitness{requirementRef, witnessFn});
  }

  void addAssociatedType(AssociatedType requirement) {
    // Find the substitution info for the witness type.
    auto td = requirement.getAssociation();
    Type witness = Conformance->getTypeWitness(td);

    // Emit the record for the type itself.
    Entries.push_back(SILWitnessTable::AssociatedTypeWitness{td,
                                                witness->getCanonicalType()});
  }

  void addAssociatedConformance(AssociatedConformance req) {
    auto assocConformance =
      Conformance->getAssociatedConformance(req.getAssociation(),
                                            req.getAssociatedRequirement());

    SGM.useConformance(assocConformance);

    Entries.push_back(SILWitnessTable::AssociatedTypeProtocolWitness{
        req.getAssociation(), req.getAssociatedRequirement(),
        assocConformance});
  }

  void addConditionalRequirements() {
    SILWitnessTable::enumerateWitnessTableConditionalConformances(
        Conformance, [&](unsigned, CanType type, ProtocolDecl *protocol) {
          auto conformance =
              Conformance->getGenericSignature()->lookupConformance(type,
                                                                    protocol);
          assert(conformance &&
                 "unable to find conformance that should be known");

          ConditionalConformances.push_back(
              SILWitnessTable::ConditionalConformance{type, conformance});

          return /*finished?*/ false;
        });
  }
};

} // end anonymous namespace

SILWitnessTable *
SILGenModule::getWitnessTable(NormalProtocolConformance *conformance) {
  // If we've already emitted this witness table, return it.
  auto found = emittedWitnessTables.find(conformance);
  if (found != emittedWitnessTables.end())
    return found->second;

  SILWitnessTable *table = SILGenConformance(*this, conformance).emit();
  emittedWitnessTables.insert({conformance, table});

  return table;
}

SILFunction *SILGenModule::emitProtocolWitness(
    ProtocolConformanceRef conformance, SILLinkage linkage,
    IsSerialized_t isSerialized, SILDeclRef requirement, SILDeclRef witnessRef,
    IsFreeFunctionWitness_t isFree, Witness witness) {
  auto requirementInfo =
      Types.getConstantInfo(TypeExpansionContext::minimal(), requirement);

  auto shouldUseDistributedThunkWitness =
      // always use a distributed thunk for distributed requirements:
      requirement.isDistributedThunk() ||
      // for non-distributed requirements, which are however async/throws,
      // and have a proper witness (passed typechecking), we can still invoke
      // them on the distributed actor; but must do so through the distributed
      // thunk as the call "through an existential" we never statically know
      // if the actor is local or not.
      (requirement.hasDecl() && requirement.getFuncDecl() && requirement.hasAsync() &&
       !requirement.getFuncDecl()->isDistributed() &&
       witnessRef.hasDecl() && witnessRef.getFuncDecl() &&
       witnessRef.getFuncDecl()->isDistributed());
  if (shouldUseDistributedThunkWitness) {
    auto thunkDeclRef = SILDeclRef(
        witnessRef.getFuncDecl()->getDistributedThunk(),
        SILDeclRef::Kind::Func);
    witnessRef = thunkDeclRef.asDistributed();
  }

  // Work out the lowered function type of the SIL witness thunk.
  auto reqtOrigTy = cast<GenericFunctionType>(requirementInfo.LoweredType);

  // Mapping from the requirement's generic signature to the witness
  // thunk's generic signature.
  auto reqtSubMap = witness.getRequirementToWitnessThunkSubs();

  // The generic environment for the witness thunk.
  auto *genericEnv = witness.getWitnessThunkSignature().getGenericEnvironment();
  auto genericSig = witness.getWitnessThunkSignature().getCanonicalSignature();

  // The type of the witness thunk.
  auto reqtSubstTy = cast<AnyFunctionType>(
    reqtOrigTy->substGenericArgs(reqtSubMap)
      ->getReducedType(genericSig));

  // Generic signatures where all parameters are concrete are lowered away
  // at the SILFunctionType level.
  if (genericSig && genericSig->areAllParamsConcrete()) {
    genericSig = nullptr;
    genericEnv = nullptr;
  }

  // Rewrite the conformance in terms of the requirement environment's Self
  // type, which might have a different generic signature than the type
  // itself.
  //
  // For example, if the conforming type is a class and the witness is defined
  // in a protocol extension, the generic signature will have an additional
  // generic parameter representing Self, so the generic parameters of the
  // class will all be shifted down by one.
  if (reqtSubMap) {
    auto requirement = conformance.getRequirement();
    auto self = requirement->getSelfInterfaceType()->getCanonicalType();

    conformance = reqtSubMap.lookupConformance(self, requirement);
  }

  reqtSubstTy =
    CanAnyFunctionType::get(genericSig,
                            reqtSubstTy->getParams(),
                            reqtSubstTy.getResult(),
                            reqtOrigTy->getExtInfo());

  // Coroutine lowering requires us to provide these substitutions
  // in order to recreate the appropriate yield types for the accessor
  // because they aren't reflected in the accessor's AST type.
  // But this is expensive, so we only do it for coroutine lowering.
  // When they're part of the AST function type, we can remove this
  // parameter completely.
  llvm::Optional<SubstitutionMap> witnessSubsForTypeLowering;
  if (auto accessor = dyn_cast<AccessorDecl>(requirement.getDecl())) {
    if (accessor->isCoroutine()) {
      witnessSubsForTypeLowering =
        witness.getSubstitutions().mapReplacementTypesOutOfContext();
    }
  }

  // Lower the witness thunk type with the requirement's abstraction level.
  auto witnessSILFnType = getNativeSILFunctionType(
      M.Types, TypeExpansionContext::minimal(), AbstractionPattern(reqtOrigTy),
      reqtSubstTy, requirementInfo.SILFnType->getExtInfo(), requirement,
      witnessRef, witnessSubsForTypeLowering, conformance);

  // Mangle the name of the witness thunk.
  Mangle::ASTMangler NewMangler;
  auto manglingConformance =
      conformance.isConcrete() ? conformance.getConcrete() : nullptr;
  std::string nameBuffer =
      NewMangler.mangleWitnessThunk(manglingConformance, requirement.getDecl());
  // TODO(TF-685): Proper mangling for derivative witness thunks.
  if (auto *derivativeId = requirement.getDerivativeFunctionIdentifier()) {
    std::string kindString;
    switch (derivativeId->getKind()) {
    case AutoDiffDerivativeFunctionKind::JVP:
      kindString = "jvp";
      break;
    case AutoDiffDerivativeFunctionKind::VJP:
      kindString = "vjp";
      break;
    }
    nameBuffer = "AD__" + nameBuffer + "_" + kindString + "_" +
                 derivativeId->getParameterIndices()->getString();
  }

  if (requirement.isDistributedThunk()) {
    nameBuffer = nameBuffer + "TE";
  }

  // If the thunked-to function is set to be always inlined, do the
  // same with the witness, on the theory that the user wants all
  // calls removed if possible, e.g. when we're able to devirtualize
  // the witness method call. Otherwise, use the default inlining
  // setting on the theory that forcing inlining off should only
  // effect the user's function, not otherwise invisible thunks.
  Inline_t InlineStrategy = InlineDefault;
  if (witnessRef.isAlwaysInline())
    InlineStrategy = AlwaysInline;

  SILGenFunctionBuilder builder(*this);
  auto *f = builder.createFunction(
      linkage, nameBuffer, witnessSILFnType, genericEnv,
      SILLocation(witnessRef.getDecl()), IsNotBare, IsTransparent, isSerialized,
      IsNotDynamic, IsNotDistributed, IsNotRuntimeAccessible, ProfileCounter(),
      IsThunk, SubclassScope::NotApplicable, InlineStrategy);

  f->setDebugScope(new (M)
                   SILDebugScope(RegularLocation(witnessRef.getDecl()), f));

  PrettyStackTraceSILFunction trace("generating protocol witness thunk", f);

  // Create the witness.
  SILGenFunction SGF(*this, *f, SwiftModule);

  // Substitutions mapping the generic parameters of the witness to
  // archetypes of the witness thunk generic environment.
  auto witnessSubs = witness.getSubstitutions();

  SGF.emitProtocolWitness(AbstractionPattern(reqtOrigTy), reqtSubstTy,
                          requirement, reqtSubMap, witnessRef,
                          witnessSubs, isFree, /*isSelfConformance*/ false,
                          witness.getEnterIsolation());

  emitLazyConformancesForFunction(f);
  return f;
}

namespace {

static SILFunction *emitSelfConformanceWitness(SILGenModule &SGM,
                                           SelfProtocolConformance *conformance,
                                               SILLinkage linkage,
                                               SILDeclRef requirement) {
  auto requirementInfo =
      SGM.Types.getConstantInfo(TypeExpansionContext::minimal(), requirement);

  // Work out the lowered function type of the SIL witness thunk.
  auto reqtOrigTy = cast<GenericFunctionType>(requirementInfo.LoweredType);

  // The transformations we do here don't work for generic requirements.
  GenericEnvironment *genericEnv = nullptr;

  // A mapping from the requirement's generic signature to the type parameters
  // of the witness thunk (which is non-generic).
  auto protocol = conformance->getProtocol();
  auto protocolType = protocol->getDeclaredInterfaceType();
  auto reqtSubs = SubstitutionMap::getProtocolSubstitutions(protocol,
                                          protocolType,
                                          ProtocolConformanceRef(conformance));

  // Open the protocol type.
  auto openedType = OpenedArchetypeType::get(
      protocol->getDeclaredExistentialType()->getCanonicalType(),
      GenericSignature());

  // Form the substitutions for calling the witness.
  auto witnessSubs = SubstitutionMap::getProtocolSubstitutions(protocol,
                                          openedType,
                                          ProtocolConformanceRef(protocol));

  // Substitute to get the formal substituted type of the thunk.
  auto reqtSubstTy =
    cast<AnyFunctionType>(reqtOrigTy.subst(reqtSubs)->getCanonicalType());

  // Substitute into the requirement type to get the type of the thunk.
  auto witnessSILFnType = requirementInfo.SILFnType->substGenericArgs(
      SGM.M, reqtSubs, TypeExpansionContext::minimal());

  // Mangle the name of the witness thunk.
  std::string name = [&] {
    Mangle::ASTMangler mangler;
    return mangler.mangleWitnessThunk(conformance, requirement.getDecl());
  }();

  SILGenFunctionBuilder builder(SGM);
  auto *f = builder.createFunction(
      linkage, name, witnessSILFnType, genericEnv,
      SILLocation(requirement.getDecl()), IsNotBare, IsTransparent,
      IsSerialized, IsNotDynamic, IsNotDistributed, IsNotRuntimeAccessible,
      ProfileCounter(), IsThunk, SubclassScope::NotApplicable, InlineDefault);

  f->setDebugScope(new (SGM.M)
                   SILDebugScope(RegularLocation(requirement.getDecl()), f));

  PrettyStackTraceSILFunction trace("generating protocol witness thunk", f);

  // Create the witness.
  SILGenFunction SGF(SGM, *f, SGM.SwiftModule);

  auto isFree = isFreeFunctionWitness(requirement.getDecl(),
                                      requirement.getDecl());

  SGF.emitProtocolWitness(AbstractionPattern(reqtOrigTy), reqtSubstTy,
                          requirement, reqtSubs, requirement, witnessSubs,
                          isFree, /*isSelfConformance*/ true, llvm::None);

  SGM.emitLazyConformancesForFunction(f);

  return f;
}

/// Emit a witness table for a self-conformance.
class SILGenSelfConformanceWitnessTable
       : public SILWitnessVisitor<SILGenSelfConformanceWitnessTable> {
  using super = SILWitnessVisitor<SILGenSelfConformanceWitnessTable>;

  SILGenModule &SGM;
  SelfProtocolConformance *conformance;
  SILLinkage linkage;
  IsSerialized_t serialized;

  SmallVector<SILWitnessTable::Entry, 8> entries;
public:
  SILGenSelfConformanceWitnessTable(SILGenModule &SGM,
                                    SelfProtocolConformance *conformance)
    : SGM(SGM), conformance(conformance),
      linkage(getLinkageForProtocolConformance(conformance, ForDefinition)),
      serialized(isConformanceSerialized(conformance)) {
  }

  void emit() {
    PrettyStackTraceConformance trace("generating SIL witness table",
                                      conformance);

    // Add entries for all the requirements.
    visitProtocolDecl(conformance->getProtocol());

    // Create the witness table.
    (void) SILWitnessTable::create(SGM.M, linkage, serialized, conformance,
                                   entries, /*conditional*/ {});
  }

  void addProtocolConformanceDescriptor() {}

  void addOutOfLineBaseProtocol(ProtocolDecl *protocol) {
    // This is an unnecessary restriction that's just not necessary for Error.
    llvm_unreachable("base protocols not supported in self-conformance");
  }

  // These are real semantic restrictions.
  void addAssociatedConformance(AssociatedConformance conformance) {
    llvm_unreachable("associated conformances not supported in self-conformance");
  }
  void addAssociatedType(AssociatedType type) {
    llvm_unreachable("associated types not supported in self-conformance");
  }
  void addPlaceholder(MissingMemberDecl *placeholder) {
    llvm_unreachable("placeholders not supported in self-conformance");
  }

  void addMethod(SILDeclRef requirement) {
    auto witness = emitSelfConformanceWitness(SGM, conformance, linkage,
                                              requirement);
    entries.push_back(SILWitnessTable::MethodWitness{requirement, witness});
  }
};
}

void SILGenModule::emitSelfConformanceWitnessTable(ProtocolDecl *protocol) {
  auto conformance = getASTContext().getSelfConformance(protocol);
  SILGenSelfConformanceWitnessTable(*this, conformance).emit();
}

namespace {

/// Emit a default witness table for a resilient protocol definition.
class SILGenDefaultWitnessTable
    : public SILGenWitnessTable<SILGenDefaultWitnessTable> {
  using super = SILGenWitnessTable<SILGenDefaultWitnessTable>;

public:
  SILGenModule &SGM;
  ProtocolDecl *Proto;
  SILLinkage Linkage;

  SmallVector<SILDefaultWitnessTable::Entry, 8> DefaultWitnesses;

  SILGenDefaultWitnessTable(SILGenModule &SGM, ProtocolDecl *proto,
                            SILLinkage linkage)
      : SGM(SGM), Proto(proto), Linkage(linkage) { }

  void addMissingDefault() {
    DefaultWitnesses.push_back(SILDefaultWitnessTable::Entry());
  }

  void addProtocolConformanceDescriptor() { }

  void addOutOfLineBaseProtocol(ProtocolDecl *baseProto) {
    addMissingDefault();
  }

  void addMissingMethod(SILDeclRef ref) {
    addMissingDefault();
  }

  void addPlaceholder(MissingMemberDecl *placeholder) {
    llvm_unreachable("generating a witness table with placeholders in it");
  }

  Witness getWitness(ValueDecl *decl) {
    return Proto->getDefaultWitness(decl);
  }

  void addMethodImplementation(SILDeclRef requirementRef,
                               SILDeclRef witnessRef,
                               IsFreeFunctionWitness_t isFree,
                               Witness witness) {
    SILFunction *witnessFn = SGM.emitProtocolWitness(
        ProtocolConformanceRef(Proto), SILLinkage::Private, IsNotSerialized,
        requirementRef, witnessRef, isFree, witness);
    auto entry = SILWitnessTable::MethodWitness{requirementRef, witnessFn};
    DefaultWitnesses.push_back(entry);
  }

  void addAssociatedType(AssociatedType req) {
    Type witness = Proto->getDefaultTypeWitness(req.getAssociation());
    if (!witness)
      return addMissingDefault();

    Type witnessInContext = Proto->mapTypeIntoContext(witness);
    auto entry = SILWitnessTable::AssociatedTypeWitness{
                                          req.getAssociation(),
                                          witnessInContext->getCanonicalType()};
    DefaultWitnesses.push_back(entry);
  }

  void addAssociatedConformance(const AssociatedConformance &req) {
    auto witness =
        Proto->getDefaultAssociatedConformanceWitness(
          req.getAssociation(),
          req.getAssociatedRequirement());
    if (witness.isInvalid())
      return addMissingDefault();

    auto entry = SILWitnessTable::AssociatedTypeProtocolWitness{
        req.getAssociation(), req.getAssociatedRequirement(), witness};
    DefaultWitnesses.push_back(entry);
  }
};

} // end anonymous namespace

void SILGenModule::emitDefaultWitnessTable(ProtocolDecl *protocol) {
  SILLinkage linkage =
      getSILLinkage(getDeclLinkage(protocol), ForDefinition);

  SILGenDefaultWitnessTable builder(*this, protocol, linkage);
  builder.visitProtocolDecl(protocol);

  SILDefaultWitnessTable *defaultWitnesses =
      M.createDefaultWitnessTableDeclaration(protocol, linkage);
  defaultWitnesses->convertToDefinition(builder.DefaultWitnesses);
}

void SILGenModule::emitNonCopyableTypeDeinitTable(NominalTypeDecl *nom) {
  auto *dd = nom->getValueTypeDestructor();
  if (!dd)
    return;

  SILDeclRef constant(dd, SILDeclRef::Kind::Deallocator);
  SILFunction *f = getFunction(constant, NotForDefinition);
  auto serialized = IsSerialized_t::IsNotSerialized;
  bool nomIsPublic = nom->getEffectiveAccess() >= AccessLevel::Public;
  // We only serialize the deinit if the type is public and not resilient.
  if (nomIsPublic && !nom->isResilient())
    serialized = IsSerialized;
  SILMoveOnlyDeinit::create(f->getModule(), nom, serialized, f);
}

namespace {

/// An ASTVisitor for generating SIL from method declarations
/// inside nominal types.
class SILGenType : public TypeMemberVisitor<SILGenType> {
public:
  SILGenModule &SGM;
  NominalTypeDecl *theType;

  SILGenType(SILGenModule &SGM, NominalTypeDecl *theType)
    : SGM(SGM), theType(theType) {}

  /// Emit SIL functions for all the members of the type.
  void emitType() {
    SGM.emitLazyConformancesForType(theType);

    forEachMemberToLower(theType, [&](Decl *member) {
      visit(member);
    });

    // Build a vtable if this is a class.
    if (auto theClass = dyn_cast<ClassDecl>(theType)) {
      SILGenVTable genVTable(SGM, theClass);
      genVTable.emitVTable();
    }

    // If this is a nominal type that is move only, emit a deinit table for it.
    if (auto *nom = dyn_cast<NominalTypeDecl>(theType)) {
      if (nom->isMoveOnly()) {
        SGM.emitNonCopyableTypeDeinitTable(nom);
      }
    }

    // Build a default witness table if this is a protocol that needs one.
    if (auto protocol = dyn_cast<ProtocolDecl>(theType)) {
      if (!protocol->isObjC() && protocol->isResilient()) {
        auto *SF = protocol->getParentSourceFile();
        if (!SF || SF->Kind != SourceFileKind::Interface)
          SGM.emitDefaultWitnessTable(protocol);
      }
      if (protocol->requiresSelfConformanceWitnessTable()) {
        SGM.emitSelfConformanceWitnessTable(protocol);
      }
      return;
    }

    // Emit witness tables for conformances of concrete types. Protocol types
    // are existential and do not have witness tables.
    for (auto *conformance : theType->getLocalConformances(
                               ConformanceLookupKind::NonInherited)) {
      if (conformance->getSourceKind() == ConformanceEntryKind::PreMacroExpansion)
        continue;

      assert(conformance->isComplete());
      if (auto *normal = dyn_cast<NormalProtocolConformance>(conformance))
        SGM.getWitnessTable(normal);
    }
  }

  //===--------------------------------------------------------------------===//
  // Visitors for subdeclarations
  //===--------------------------------------------------------------------===//
  void visitTypeAliasDecl(TypeAliasDecl *tad) {}
  void visitOpaqueTypeDecl(OpaqueTypeDecl *otd) {}
  void visitGenericTypeParamDecl(GenericTypeParamDecl *d) {}
  void visitAssociatedTypeDecl(AssociatedTypeDecl *d) {}
  void visitModuleDecl(ModuleDecl *md) {}
  void visitMissingMemberDecl(MissingMemberDecl *) {}
  void visitNominalTypeDecl(NominalTypeDecl *ntd) {
    SILGenType(SGM, ntd).emitType();
  }
  void visitFuncDecl(FuncDecl *fd) {
    SGM.emitFunction(fd);
    // FIXME: Default implementations in protocols.
    if (SGM.requiresObjCMethodEntryPoint(fd) &&
        !isa<ProtocolDecl>(fd->getDeclContext()))
      SGM.emitObjCMethodThunk(fd);
  }
  void visitConstructorDecl(ConstructorDecl *cd) {
    SGM.emitConstructor(cd);

    if (SGM.requiresObjCMethodEntryPoint(cd) &&
        !isa<ProtocolDecl>(cd->getDeclContext()))
      SGM.emitObjCConstructorThunk(cd);
  }

  void visitDestructorDecl(DestructorDecl *dd) {
    if (auto *cd = dyn_cast<ClassDecl>(theType))
      return SGM.emitDestructor(cast<ClassDecl>(theType), dd);
    if (auto *nom = dyn_cast<NominalTypeDecl>(theType)) {
      if (nom->isMoveOnly()) {
        return SGM.emitMoveOnlyDestructor(nom, dd);
      }
    }
    assert(isa<ClassDecl>(theType) &&
           "destructor in a non-class, non-moveonly type");
  }

  void visitEnumCaseDecl(EnumCaseDecl *ecd) {}
  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;

    // Emit any default argument generators.
    SGM.emitArgumentGenerators(EED, EED->getParameterList());
  }

  void visitPatternBindingDecl(PatternBindingDecl *pd) {
    // Emit initializers.
    for (auto i : range(pd->getNumPatternEntries())) {
      if (pd->getExecutableInit(i)) {
        if (pd->isStatic())
          SGM.emitGlobalInitialization(pd, i);
        else
          SGM.emitStoredPropertyInitialization(pd, i);
      }
    }
  }

  void visitVarDecl(VarDecl *vd) {
    // Collect global variables for static properties.
    // FIXME: We can't statically emit a global variable for generic properties.
    if (vd->isStatic() && vd->hasStorage()) {
      emitTypeMemberGlobalVariable(SGM, vd);
      visitAccessors(vd);
      return;
    }

    // If this variable has an attached property wrapper with an initialization
    // function, emit the backing initializer function.
    auto initInfo = vd->getPropertyWrapperInitializerInfo();
    if (initInfo.hasInitFromWrappedValue() && !vd->isStatic()) {
      SGM.emitPropertyWrapperBackingInitializer(vd);
    }

    if (auto *thunk = vd->getDistributedThunk()) {
      auto thunkRef = SILDeclRef(thunk).asDistributed();
      SGM.emitFunctionDefinition(thunkRef,
                                 SGM.getFunction(thunkRef, ForDefinition));
    }

    visitAbstractStorageDecl(vd);
  }

  void visitSubscriptDecl(SubscriptDecl *sd) {
    SGM.emitArgumentGenerators(sd, sd->getIndices());
    visitAbstractStorageDecl(sd);
  }

  void visitAbstractStorageDecl(AbstractStorageDecl *asd) {
    // FIXME: Default implementations in protocols.
    if (asd->isObjC() && !isa<ProtocolDecl>(asd->getDeclContext()))
      SGM.emitObjCPropertyMethodThunks(asd);

    SGM.tryEmitPropertyDescriptor(asd);
    visitAccessors(asd);
  }

  void visitAccessors(AbstractStorageDecl *asd) {
    asd->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visitFuncDecl(accessor);
    });
  }

  void visitMissingDecl(MissingDecl *missing) {
    llvm_unreachable("missing decl in SILGen");
  }

  void visitMacroDecl(MacroDecl *md) {
    llvm_unreachable("macros aren't allowed in types");
  }
};

} // end anonymous namespace

void SILGenModule::visitNominalTypeDecl(NominalTypeDecl *ntd) {
  SILGenType(*this, ntd).emitType();
}

/// SILGenExtension - an ASTVisitor for generating SIL from method declarations
/// and protocol conformances inside type extensions.
class SILGenExtension : public TypeMemberVisitor<SILGenExtension> {
public:
  SILGenModule &SGM;

  SILGenExtension(SILGenModule &SGM)
    : SGM(SGM) {}

  /// Emit SIL functions for all the members of the extension.
  void emitExtension(ExtensionDecl *e) {
    // Arguably, we should divert to SILGenType::emitType() here if it's an
    // @_objcImplementation extension, but we don't actually need to do any of
    // the stuff that it currently does.

    forEachMemberToLower(e, [&](Decl *member) {
      visit(member);
    });

    // If this is a main-interface @_objcImplementation extension and the class
    // has a synthesized destructor, emit it now.
    if (auto cd = dyn_cast_or_null<ClassDecl>(e->getImplementedObjCDecl())) {
      auto dd = cd->getDestructor();
      if (dd->getDeclContext() == cd)
        visit(dd);
    }

    if (!isa<ProtocolDecl>(e->getExtendedNominal())) {
      // Emit witness tables for protocol conformances introduced by the
      // extension.
      for (auto *conformance : e->getLocalConformances(
                                 ConformanceLookupKind::All)) {
        assert(conformance->isComplete());
        if (auto *normal =dyn_cast<NormalProtocolConformance>(conformance))
          SGM.getWitnessTable(normal);
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Visitors for subdeclarations
  //===--------------------------------------------------------------------===//
  void visitTypeAliasDecl(TypeAliasDecl *tad) {}
  void visitOpaqueTypeDecl(OpaqueTypeDecl *tad) {}
  void visitGenericTypeParamDecl(GenericTypeParamDecl *d) {}
  void visitAssociatedTypeDecl(AssociatedTypeDecl *d) {}
  void visitModuleDecl(ModuleDecl *md) {}
  void visitMissingMemberDecl(MissingMemberDecl *) {}
  void visitNominalTypeDecl(NominalTypeDecl *ntd) {
    SILGenType(SGM, ntd).emitType();
  }
  void visitFuncDecl(FuncDecl *fd) {
    // Don't emit other accessors for a dynamic replacement of didSet inside of
    // an extension. We only allow such a construct to allow definition of a
    // didSet/willSet dynamic replacement. Emitting other accessors is
    // problematic because there is no storage.
    //
    // extension SomeStruct {
    //   @_dynamicReplacement(for: someProperty)
    //   var replacement : Int {
    //     didSet {
    //     }
    //   }
    // }
    if (auto *accessor = dyn_cast<AccessorDecl>(fd)) {
      auto *storage = accessor->getStorage();
      bool hasDidSetOrWillSetDynamicReplacement =
          storage->hasDidSetOrWillSetDynamicReplacement();

      if (hasDidSetOrWillSetDynamicReplacement &&
          isa<ExtensionDecl>(storage->getDeclContext()) &&
          fd != storage->getParsedAccessor(AccessorKind::WillSet) &&
          fd != storage->getParsedAccessor(AccessorKind::DidSet))
        return;
    }
    SGM.emitFunction(fd);
    if (SGM.requiresObjCMethodEntryPoint(fd))
      SGM.emitObjCMethodThunk(fd);
  }
  void visitConstructorDecl(ConstructorDecl *cd) {
    SGM.emitConstructor(cd);
    if (SGM.requiresObjCMethodEntryPoint(cd))
      SGM.emitObjCConstructorThunk(cd);
  }
  void visitDestructorDecl(DestructorDecl *dd) {
    auto contextInterface = dd->getDeclContext()->getImplementedObjCContext();
    if (auto cd = dyn_cast<ClassDecl>(contextInterface)) {
      SGM.emitDestructor(cd, dd);
      return;
    }
    llvm_unreachable("destructor in extension?!");
  }

  void visitPatternBindingDecl(PatternBindingDecl *pd) {
    // Emit initializers for static variables.
    for (auto i : range(pd->getNumPatternEntries())) {
      if (pd->getExecutableInit(i)) {
        if (pd->isStatic())
          SGM.emitGlobalInitialization(pd, i);
        else if (isa<ExtensionDecl>(pd->getDeclContext()) &&
                 cast<ExtensionDecl>(pd->getDeclContext())
                     ->isObjCImplementation())
          SGM.emitStoredPropertyInitialization(pd, i);
      }
    }
  }

  void visitVarDecl(VarDecl *vd) {
    if (vd->hasStorage()) {
      if (!vd->isStatic()) {
        // Is this a stored property of an @_objcImplementation extension?
        auto ed = cast<ExtensionDecl>(vd->getDeclContext());
        if (auto cd =
                dyn_cast_or_null<ClassDecl>(ed->getImplementedObjCDecl())) {
          // Act as though we declared it on the class.
          SILGenType(SGM, cd).visitVarDecl(vd);
          return;
        }
      }

      bool hasDidSetOrWillSetDynamicReplacement =
          vd->hasDidSetOrWillSetDynamicReplacement();
      assert((vd->isStatic() || hasDidSetOrWillSetDynamicReplacement) &&
             "stored property in extension?!");
      if (!hasDidSetOrWillSetDynamicReplacement) {
        emitTypeMemberGlobalVariable(SGM, vd);
        visitAccessors(vd);
        return;
      }
    }

    if (auto *thunk = vd->getDistributedThunk()) {
      auto thunkRef = SILDeclRef(thunk).asDistributed();
      SGM.emitFunctionDefinition(thunkRef,
                                 SGM.getFunction(thunkRef, ForDefinition));
    }

    visitAbstractStorageDecl(vd);
  }

  void visitSubscriptDecl(SubscriptDecl *sd) {
    SGM.emitArgumentGenerators(sd, sd->getIndices());
    visitAbstractStorageDecl(sd);
  }

  void visitEnumCaseDecl(EnumCaseDecl *ecd) {}
  void visitEnumElementDecl(EnumElementDecl *ed) {
    llvm_unreachable("enum elements aren't allowed in extensions");
  }

  void visitAbstractStorageDecl(AbstractStorageDecl *asd) {
    if (asd->isObjC())
      SGM.emitObjCPropertyMethodThunks(asd);
    
    SGM.tryEmitPropertyDescriptor(asd);
    visitAccessors(asd);
  }

  void visitAccessors(AbstractStorageDecl *asd) {
    asd->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visitFuncDecl(accessor);
    });
  }

  void visitMissingDecl(MissingDecl *missing) {
    llvm_unreachable("missing decl in SILGen");
  }

  void visitMacroDecl(MacroDecl *md) {
    llvm_unreachable("macros aren't allowed in extensions");
  }
};

void SILGenModule::visitExtensionDecl(ExtensionDecl *ed) {
  SILGenExtension(*this).emitExtension(ed);
}

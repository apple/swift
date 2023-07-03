//===--- Verifier.cpp - Verification of Swift SIL Code --------------------===//
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

#define DEBUG_TYPE "sil-verifier"

#include "VerifierPrivate.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Range.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/AddressWalker.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PostOrder.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVTable.h"
#include "swift/SIL/SILVTableVisitor.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::silverifier;

using Lowering::AbstractionPattern;

// This flag controls the default behaviour when hitting a verification
// failure (abort/exit).
static llvm::cl::opt<bool> AbortOnFailure(
                              "verify-abort-on-failure",
                              llvm::cl::init(true));

static llvm::cl::opt<bool> ContinueOnFailure("verify-continue-on-failure",
                                             llvm::cl::init(false));

static llvm::cl::opt<bool> DumpModuleOnFailure("verify-dump-module-on-failure",
                                             llvm::cl::init(false));

// This verification is affects primarily debug info and end users don't derive
// a benefit from seeing its results.
static llvm::cl::opt<bool> VerifyDIHoles("verify-di-holes", llvm::cl::init(
#ifndef NDEBUG
                                                                true
#else
                                                                false
#endif
                                                                ));

static llvm::cl::opt<bool> SkipConvertEscapeToNoescapeAttributes(
    "verify-skip-convert-escape-to-noescape-attributes", llvm::cl::init(false));

// Allow unit tests to gradually migrate toward -allow-critical-edges=false.
static llvm::cl::opt<bool> AllowCriticalEdges("allow-critical-edges",
                                              llvm::cl::init(true));
extern llvm::cl::opt<bool> SILPrintDebugInfo;

// The verifier is basically all assertions, so don't compile it with NDEBUG to
// prevent release builds from triggering spurious unused variable warnings.

//===----------------------------------------------------------------------===//
//                                SILVerifier
//===----------------------------------------------------------------------===//

/// Returns true if A is an opened existential type or is equal to an
/// archetype from F's generic context.
static bool isArchetypeValidInFunction(ArchetypeType *A, const SILFunction *F) {
  if (!isa<PrimaryArchetypeType>(A) && !isa<PackArchetypeType>(A))
    return true;
  if (isa<LocalArchetypeType>(A))
    return true;
  if (isa<OpaqueTypeArchetypeType>(A))
    return true;

  // Ok, we have a primary archetype, make sure it is in the nested generic
  // environment of our caller.
  if (auto *genericEnv = F->getGenericEnvironment())
    if (A->getGenericEnvironment() == genericEnv)
      return true;

  return false;
}

namespace {

/// When resilience is bypassed, direct access is legal, but the decls are still
/// resilient.
template <typename DeclType>
bool checkResilience(DeclType *D, ModuleDecl *M,
                     ResilienceExpansion expansion) {
  return !D->getModuleContext()->getBypassResilience() &&
         D->isResilient(M, expansion);
}

/// Metaprogramming-friendly base class.
template <class Impl>
class SILVerifierBase : public SILInstructionVisitor<Impl> {
public:
  // visitCLASS calls visitPARENT and checkCLASS.
  // checkCLASS does nothing by default.
#define INST(CLASS, PARENT)                                     \
  void visit##CLASS(CLASS *I) {                                 \
    static_cast<Impl*>(this)->visit##PARENT(I);                 \
    static_cast<Impl*>(this)->check##CLASS(I);                  \
  }                                                             \
  void check##CLASS(CLASS *I) {}
#include "swift/SIL/SILNodes.def"

  void visitSILInstruction(SILInstruction *I) {
    static_cast<Impl*>(this)->checkSILInstruction(I);
  }
  void checkSILInstruction(SILInstruction *I) {}
};
} // end anonymous namespace

namespace {

/// Verify invariants on a key path component.
void verifyKeyPathComponent(SILModule &M,
                            TypeExpansionContext typeExpansionContext,
                            llvm::function_ref<void(bool, StringRef)> require,
                            CanType &baseTy,
                            CanType leafTy,
                            const KeyPathPatternComponent &component,
                            ArrayRef<Operand> operands,
                            CanGenericSignature patternSig,
                            SubstitutionMap patternSubs,
                            bool forPropertyDescriptor,
                            bool hasIndices) {
  auto expansion = typeExpansionContext.getResilienceExpansion();
  auto opaque = AbstractionPattern::getOpaque();
  auto loweredBaseTy =
      M.Types.getLoweredType(opaque, baseTy, typeExpansionContext);
  auto componentTy = component.getComponentType().subst(patternSubs)
    ->getCanonicalType();
  auto loweredComponentTy =
      M.Types.getLoweredType(opaque, componentTy, typeExpansionContext);
  auto getTypeInExpansionContext = [&](CanType ty) -> CanType {
    return M.Types.getLoweredType(opaque, ty, typeExpansionContext).getASTType();
  };
  auto checkIndexEqualsAndHash = [&]{
    if (!component.getSubscriptIndices().empty()) {
      // Equals should be
      // <Sig...> @convention(thin) (RawPointer, RawPointer) -> Bool
      {
        auto equals = component.getSubscriptIndexEquals();
        require(equals, "key path pattern with indexes must have equals "
                        "operator");
        
        auto substEqualsType = equals->getLoweredFunctionType()
          ->substGenericArgs(M, patternSubs, TypeExpansionContext::minimal());
        
        require(substEqualsType->getParameters().size() == 2,
                "must have two arguments");
        for (unsigned i = 0; i < 2; ++i) {
          auto param = substEqualsType->getParameters()[i];
          require(param.getConvention()
                    == ParameterConvention::Direct_Unowned,
                  "indices pointer should be trivial");
          require(param.getInterfaceType()->isUnsafeRawPointer(),
                  "indices pointer should be an UnsafeRawPointer");
        }
        
        require(substEqualsType->getResults().size() == 1,
                "must have one result");
        
        require(substEqualsType->getResults()[0].getConvention()
                  == ResultConvention::Unowned,
                "result should be unowned");
        require(substEqualsType->getResults()[0].getInterfaceType()->isBool(),
                "result should be Bool");
      }
      {
        // Hash should be
        // <Sig...> @convention(thin) (RawPointer) -> Int
        auto hash = component.getSubscriptIndexHash();
        require(hash, "key path pattern with indexes must have hash "
                      "operator");
        
        auto substHashType = hash->getLoweredFunctionType()
          ->substGenericArgs(M, patternSubs, TypeExpansionContext::minimal());
        
        require(substHashType->getParameters().size() == 1,
                "must have two arguments");
        auto param = substHashType->getParameters()[0];
        require(param.getConvention()
                  == ParameterConvention::Direct_Unowned,
                "indices pointer should be trivial");
        require(param.getInterfaceType()->isUnsafeRawPointer(),
                "indices pointer should be an UnsafeRawPointer");
        
        require(substHashType->getResults().size() == 1,
                "must have one result");
        
        require(substHashType->getResults()[0].getConvention()
                  == ResultConvention::Unowned,
                "result should be unowned");
        require(substHashType->getResults()[0].getInterfaceType()->isInt(),
                "result should be Int");
      }
    } else {
      require(!component.getSubscriptIndexEquals()
              && !component.getSubscriptIndexHash(),
              "component without indexes must not have equals/hash");
    }
  };

  switch (auto kind = component.getKind()) {
  case KeyPathPatternComponent::Kind::StoredProperty: {
    auto property = component.getStoredPropertyDecl();
    if (expansion == ResilienceExpansion::Minimal) {
      require(property->getEffectiveAccess() >= AccessLevel::Public,
              "Key path in serialized function cannot reference non-public "
              "property");
    }

    auto fieldTy = baseTy->getTypeOfMember(M.getSwiftModule(), property)
                         ->getReferenceStorageReferent()
                         ->getCanonicalType();
    require(getTypeInExpansionContext(fieldTy) ==
                getTypeInExpansionContext(componentTy),
            "property decl should be a member of the base with the same type "
            "as the component");
    require(property->hasStorage(), "property must be stored");
    require(!checkResilience(property, M.getSwiftModule(), expansion),
            "cannot access storage of resilient property");
    auto propertyTy =
        loweredBaseTy.getFieldType(property, M, typeExpansionContext);
    require(propertyTy.getObjectType()
              == loweredComponentTy.getObjectType(),
            "component type should match the maximal abstraction of the "
            "formal type");
    break;
  }
    
  case KeyPathPatternComponent::Kind::GettableProperty:
  case KeyPathPatternComponent::Kind::SettableProperty: {
    if (forPropertyDescriptor) {
      require(component.getSubscriptIndices().empty()
              && !component.getSubscriptIndexEquals()
              && !component.getSubscriptIndexHash(),
              "property descriptor should not have index information");
      
      require(component.getExternalDecl() == nullptr
              && component.getExternalSubstitutions().empty(),
              "property descriptor should not refer to another external decl");
    } else {
      require(hasIndices == !component.getSubscriptIndices().empty(),
              "component for subscript should have indices");
    }
    
    auto normalArgConvention = ParameterConvention::Indirect_In_Guaranteed;
  
    // Getter should be <Sig...> @convention(thin) (@in_guaranteed Base) -> @out Result
    {
      auto getter = component.getComputedPropertyGetter();
      if (expansion == ResilienceExpansion::Minimal) {
        require(getter->hasValidLinkageForFragileRef(),
                "Key path in serialized function should not reference "
                "less visible getters");
      }

      auto substGetterType = getter->getLoweredFunctionType()->substGenericArgs(
          M, patternSubs, TypeExpansionContext::minimal());
      require(substGetterType->getRepresentation() ==
                SILFunctionTypeRepresentation::Thin,
              "getter should be a thin function");
      
      require(substGetterType->getNumParameters() == 1 + hasIndices,
              "getter should have one parameter");
      auto baseParam = substGetterType->getParameters()[0];
      require(baseParam.getConvention() == normalArgConvention,
              "getter base parameter should have normal arg convention");
      require(getTypeInExpansionContext(baseParam.getArgumentType(
                  M, substGetterType, typeExpansionContext)) ==
                  loweredBaseTy.getASTType(),
              "getter base parameter should match base of component");

      if (hasIndices) {
        auto indicesParam = substGetterType->getParameters()[1];
        require(indicesParam.getConvention()
                  == ParameterConvention::Direct_Unowned,
                "indices pointer should be trivial");
        require(
            indicesParam
                    .getArgumentType(M, substGetterType, typeExpansionContext)
                    ->isUnsafeRawPointer(),
            "indices pointer should be an UnsafeRawPointer");
      }

      require(substGetterType->getNumResults() == 1,
              "getter should have one result");
      auto result = substGetterType->getResults()[0];
      require(result.getConvention() == ResultConvention::Indirect,
              "getter result should be @out");
      require(getTypeInExpansionContext(result.getReturnValueType(
                  M, substGetterType, typeExpansionContext)) ==
                  getTypeInExpansionContext(loweredComponentTy.getASTType()),
              "getter result should match the maximal abstraction of the "
              "formal component type");
    }
    
    if (kind == KeyPathPatternComponent::Kind::SettableProperty) {
      // Setter should be
      // <Sig...> @convention(thin) (@in_guaranteed Result, @in Base) -> ()
      
      auto setter = component.getComputedPropertySetter();
      if (expansion == ResilienceExpansion::Minimal) {
        require(setter->hasValidLinkageForFragileRef(),
                "Key path in serialized function should not reference "
                "less visible setters");
      }

      auto substSetterType = setter->getLoweredFunctionType()
        ->substGenericArgs(M, patternSubs, TypeExpansionContext::minimal());
      
      require(substSetterType->getRepresentation() ==
                SILFunctionTypeRepresentation::Thin,
              "setter should be a thin function");
      
      require(substSetterType->getNumParameters() == 2 + hasIndices,
              "setter should have two parameters");

      auto newValueParam = substSetterType->getParameters()[0];
      // TODO: This should probably be unconditionally +1 when we
      // can represent that.
      require(newValueParam.getConvention() == normalArgConvention,
              "setter value parameter should have normal arg convention");

      auto baseParam = substSetterType->getParameters()[1];
      require(baseParam.getConvention() == normalArgConvention
              || baseParam.getConvention() ==
                  ParameterConvention::Indirect_Inout,
              "setter base parameter should be normal arg convention "
              "or @inout");
      
      if (hasIndices) {
        auto indicesParam = substSetterType->getParameters()[2];
        require(indicesParam.getConvention()
                  == ParameterConvention::Direct_Unowned,
                "indices pointer should be trivial");
        require(
            indicesParam
                    .getArgumentType(M, substSetterType, typeExpansionContext)
                    ->isUnsafeRawPointer(),
            "indices pointer should be an UnsafeRawPointer");
      }

      require(getTypeInExpansionContext(newValueParam.getArgumentType(
                  M, substSetterType, typeExpansionContext)) ==
                  getTypeInExpansionContext(loweredComponentTy.getASTType()),
              "setter value should match the maximal abstraction of the "
              "formal component type");

      require(substSetterType->getNumResults() == 0,
              "setter should have no results");
    }
    
    if (!forPropertyDescriptor) {
      for (auto &index : component.getSubscriptIndices()) {
        auto opIndex = index.Operand;
        auto contextType =
          index.LoweredType.subst(M, patternSubs);
        require(contextType == operands[opIndex].get()->getType(),
                "operand must match type required by pattern");
        SILType loweredType = index.LoweredType;
        require(
            loweredType.isLoweringOf(typeExpansionContext, M, index.FormalType),
            "pattern index formal type doesn't match lowered type");
      }

      checkIndexEqualsAndHash();
    }
    
    break;
  }
  case KeyPathPatternComponent::Kind::OptionalChain: {
    require(baseTy->getOptionalObjectType()->isEqual(componentTy),
            "chaining component should unwrap optional");
    require((bool)leafTy->getOptionalObjectType(),
            "key path with chaining component should have optional "
            "result");
    break;
  }
  case KeyPathPatternComponent::Kind::OptionalForce: {
    require(baseTy->getOptionalObjectType()->isEqual(componentTy),
            "forcing component should unwrap optional");
    break;
  }
  case KeyPathPatternComponent::Kind::OptionalWrap: {
    require(componentTy->getOptionalObjectType()->isEqual(baseTy),
            "wrapping component should wrap optional");
    break;
  }
  case KeyPathPatternComponent::Kind::TupleElement: {
    require(baseTy->is<TupleType>(),
            "invalid baseTy, should have been a TupleType");
      
    auto tupleTy = baseTy->castTo<TupleType>();
    auto eltIdx = component.getTupleIndex();
      
    require(eltIdx < tupleTy->getNumElements(),
            "invalid element index, greater than # of tuple elements");

    auto eltTy = tupleTy->getElementType(eltIdx)
      ->getReferenceStorageReferent();
    
    require(eltTy->isEqual(componentTy),
            "tuple element type should match the type of the component");

    break;
  }
  }
  
  baseTy = componentTy;
}

/// Check if according to the SIL language model this memory /must only/ be used
/// immutably. Today this is only applied to in_guaranteed arguments and
/// open_existential_addr. We should expand it as needed.
struct ImmutableAddressUseVerifier {
  SmallVector<Operand *, 32> worklist;

  bool isConsumingOrMutatingArgumentConvention(SILArgumentConvention conv) {
    switch (conv) {
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Pack_Guaranteed:
      return false;

    case SILArgumentConvention::Indirect_InoutAliasable:
      // DISCUSSION: We do not consider inout_aliasable to be "truly mutating"
      // since today it is just used as a way to mark a captured argument and
      // not that something truly has mutating semantics. The reason why this
      // is safe is that the typechecker guarantees that if our value was
      // immutable, then the use in the closure must be immutable as well.
      //
      // TODO: Remove this in favor of using Inout and In_Guaranteed.
      return false;

    case SILArgumentConvention::Pack_Out:
    case SILArgumentConvention::Pack_Owned:
    case SILArgumentConvention::Pack_Inout:
    case SILArgumentConvention::Indirect_Out:
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_Inout:
      return true;

    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
    case SILArgumentConvention::Direct_Owned:
      assert(conv.isIndirectConvention() && "Expect an indirect convention");
      return true; // return something "conservative".
    }
    llvm_unreachable("covered switch isn't covered?!");
  }

  bool isConsumingOrMutatingApplyUse(Operand *use) {
    ApplySite apply(use->getUser());
    assert(apply && "Not an apply instruction kind");
    auto conv = apply.getArgumentConvention(*use);
    return isConsumingOrMutatingArgumentConvention(conv);
  }

  bool isConsumingOrMutatingYieldUse(Operand *use) {
    // For now, just say that it is non-consuming for now.
    auto *yield = cast<YieldInst>(use->getUser());
    auto conv = yield->getArgumentConventionForOperand(*use);
    return isConsumingOrMutatingArgumentConvention(conv);
  }

  // A "copy_addr %src [take] to *" is consuming on "%src".
  // A "copy_addr * to * %dst" is mutating on "%dst".
  bool isConsumingOrMutatingCopyAddrUse(Operand *use) {
    auto *copyAddr = cast<CopyAddrInst>(use->getUser());
    if (copyAddr->getDest() == use->get())
      return true;
    if (copyAddr->getSrc() == use->get() && copyAddr->isTakeOfSrc() == IsTake)
      return true;
    return false;
  }

  bool isConsumingOrMutatingExplicitCopyAddrUse(Operand *use) {
    auto *copyAddr = cast<ExplicitCopyAddrInst>(use->getUser());
    if (copyAddr->getDest() == use->get())
      return true;
    if (copyAddr->getSrc() == use->get() && copyAddr->isTakeOfSrc() == IsTake)
      return true;
    return false;
  }

  bool isAddrCastToNonConsuming(SingleValueInstruction *i) {
    // Check if any of our uses are consuming. If none of them are consuming, we
    // are good to go.
    return llvm::none_of(i->getUses(), [&](Operand *use) -> bool {
      auto *inst = use->getUser();
      switch (inst->getKind()) {
      default:
        return false;
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst:
      case SILInstructionKind::PartialApplyInst:
      case SILInstructionKind::BeginApplyInst:
        return isConsumingOrMutatingApplyUse(use);
      }
    });
  }

  bool isMutatingOrConsuming(SILValue address) {
    llvm::copy(address->getUses(), std::back_inserter(worklist));
    while (!worklist.empty()) {
      auto *use = worklist.pop_back_val();
      auto *inst = use->getUser();

      if (inst->isTypeDependentOperand(*use))
        continue;

      // TODO: Can this switch be restructured so break -> error, continue ->
      // next iteration, return -> return the final result.
      switch (inst->getKind()) {
      case SILInstructionKind::BuiltinInst: {
        // If we are processing a polymorphic builtin that takes an address,
        // skip the builtin. This is because the builtin must be specialized to
        // a non-memory reading builtin that works on trivial object values
        // before the diagnostic passes end (or be DCEed) or we emit a
        // diagnostic.
        if (auto builtinKind = cast<BuiltinInst>(inst)->getBuiltinKind()) {
          if (isPolymorphicBuiltin(*builtinKind)) {
            break;
          }
        }

        // Otherwise this is a builtin that we are not expecting to see, so bail
        // and assert.
        llvm::errs() << "Unhandled, unexpected builtin instruction: " << *inst;
        llvm_unreachable("invoking standard assertion failure");
        break;
      }
      case SILInstructionKind::MarkDependenceInst:
      case SILInstructionKind::LoadBorrowInst:
      case SILInstructionKind::ExistentialMetatypeInst:
      case SILInstructionKind::ValueMetatypeInst:
      case SILInstructionKind::FixLifetimeInst:
      case SILInstructionKind::KeyPathInst:
      case SILInstructionKind::SwitchEnumAddrInst:
      case SILInstructionKind::SelectEnumAddrInst:
        break;
      case SILInstructionKind::DebugValueInst:
        if (cast<DebugValueInst>(inst)->hasAddrVal())
          break;
        else {
          llvm::errs() << "Unhandled, unexpected instruction: " << *inst;
          llvm_unreachable("invoking standard assertion failure");
        }
      case SILInstructionKind::AddressToPointerInst:
        // We assume that the user is attempting to do something unsafe since we
        // are converting to a raw pointer. So just ignore this use.
        //
        // TODO: Can we do better?
        break;
      case SILInstructionKind::BranchInst:
      case SILInstructionKind::CondBranchInst:
        // We do not analyze through branches and cond_br instructions and just
        // assume correctness. This is so that we can avoid having to analyze
        // through phi loops and since we want to remove address phis (meaning
        // that this eventually would never be able to happen). Once that
        // changes happens, we should remove this code and just error below.
        break;
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst:
      case SILInstructionKind::PartialApplyInst:
      case SILInstructionKind::BeginApplyInst:
        if (isConsumingOrMutatingApplyUse(use))
          return true;
        break;
      case SILInstructionKind::YieldInst:
        if (isConsumingOrMutatingYieldUse(use))
          return true;
        break;
      case SILInstructionKind::BeginAccessInst:
        if (cast<BeginAccessInst>(inst)->getAccessKind() != SILAccessKind::Read)
          return true;
        break;
      case SILInstructionKind::EndAccessInst:
        break;
      case SILInstructionKind::MarkUnresolvedMoveAddrInst:
        // We model mark_unresolved_move_addr as a copy_addr [init]. So no
        // mutation can happen. The checker will prove eventually that we can
        // convert it to a copy_addr [take] [init].
        break;
      case SILInstructionKind::ExplicitCopyAddrInst:
        if (isConsumingOrMutatingExplicitCopyAddrUse(use))
          return true;
        else
          break;
      case SILInstructionKind::CopyAddrInst:
        if (isConsumingOrMutatingCopyAddrUse(use))
          return true;
        else
          break;
      case SILInstructionKind::DestroyAddrInst:
        return true;
      case SILInstructionKind::UpcastInst:
      case SILInstructionKind::UncheckedAddrCastInst: {
        if (isAddrCastToNonConsuming(cast<SingleValueInstruction>(inst))) {
          break;
        }
        return true;
      }
      case SILInstructionKind::CheckedCastAddrBranchInst:
        switch (cast<CheckedCastAddrBranchInst>(inst)->getConsumptionKind()) {
        case CastConsumptionKind::BorrowAlways:
          llvm_unreachable("checked_cast_addr_br cannot have BorrowAlways");
        case CastConsumptionKind::CopyOnSuccess:
          break;
        case CastConsumptionKind::TakeAlways:
        case CastConsumptionKind::TakeOnSuccess:
          return true;
        }
        break;
      case SILInstructionKind::LoadInst:
        // A 'non-taking' value load is harmless.
        if (cast<LoadInst>(inst)->getOwnershipQualifier() ==
            LoadOwnershipQualifier::Take)
          return true;
        break;
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Load##Name##Inst:                                   \
    if (cast<Load##Name##Inst>(inst)->isTake())                                \
      return true;                                                             \
    break;
#include "swift/AST/ReferenceStorage.def"
      case SILInstructionKind::OpenExistentialAddrInst:
        // If we have a mutable use, return true. Otherwise fallthrough since we
        // want to look through immutable uses.
        if (cast<OpenExistentialAddrInst>(inst)->getAccessKind() !=
            OpenedExistentialAccess::Immutable)
          return true;
        LLVM_FALLTHROUGH;
      case SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst:
      case SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst:
      case SILInstructionKind::StructElementAddrInst:
      case SILInstructionKind::TupleElementAddrInst:
      case SILInstructionKind::IndexAddrInst:
      case SILInstructionKind::TailAddrInst:
      case SILInstructionKind::IndexRawPointerInst:
      case SILInstructionKind::MarkMustCheckInst:
      case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
      case SILInstructionKind::PackElementGetInst:
        // Add these to our worklist.
        for (auto result : inst->getResults()) {
          llvm::copy(result->getUses(), std::back_inserter(worklist));
        }
        break;
      case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
        auto type =
            cast<UncheckedTakeEnumDataAddrInst>(inst)->getOperand()->getType();
        if (type.getOptionalObjectType()) {
          for (auto result : inst->getResults()) {
            llvm::copy(result->getUses(), std::back_inserter(worklist));
          }
          break;
        }
        llvm::errs() << "Unhandled, unexpected instruction: " << *inst;
        llvm_unreachable("invoking standard assertion failure");
        break;
      }
      case SILInstructionKind::TuplePackElementAddrInst: {
        if (&cast<TuplePackElementAddrInst>(inst)->getOperandRef(
              TuplePackElementAddrInst::TupleOperand) == use) {
          for (auto result : inst->getResults()) {
            llvm::copy(result->getUses(), std::back_inserter(worklist));
          }

          break;
        }

        return false;
      }
      case SILInstructionKind::PackElementSetInst: {
        if (&cast<PackElementSetInst>(inst)->getOperandRef(
              PackElementSetInst::PackOperand) == use)
          return true;

        return false;
      }
      default:
        llvm::errs() << "Unhandled, unexpected instruction: " << *inst;
        llvm_unreachable("invoking standard assertion failure");
        break;
      }
    }
    return false;
  }
};

static void checkAddressWalkerCanVisitAllTransitiveUses(SILValue address) {
  struct Visitor final : TransitiveAddressWalker {
    bool visitUse(Operand *use) override { return true; }
    void onError(Operand *use) override {}
  };

  Visitor visitor;
  if (std::move(visitor).walk(address) != AddressUseKind::Unknown)
    return;

  llvm::errs() << "TransitiveAddressWalker walker failed to know how to visit "
                  "a user when visiting: "
               << *address << '\n';
  llvm::report_fatal_error("invoking standard assertion failure");
}

/// The SIL verifier walks over a SIL function / basic block / instruction,
/// checking and enforcing its invariants.
class SILVerifier : public SILVerifierBase<SILVerifier> {
  ModuleDecl *M;
  const SILFunction &F;
  SILPassManager *passManager;
  SILFunctionConventions fnConv;
  Lowering::TypeConverter &TC;

  bool SingleFunction = true;
  bool checkLinearLifetime = false;

  SmallVector<std::pair<StringRef, SILType>, 16> DebugVars;
  const SILInstruction *CurInstruction = nullptr;
  const SILArgument *CurArgument = nullptr;
  std::unique_ptr<DominanceInfo> Dominance;

  // Used for dominance checking within a basic block.
  llvm::DenseMap<const SILInstruction *, unsigned> InstNumbers;

  std::unique_ptr<DeadEndBlocks> DEBlocks;

  LoadBorrowImmutabilityAnalysis loadBorrowImmutabilityAnalysis;

  /// A cache of the isOperandInValueUse check. When we process an operand, we
  /// fix this for each of its uses.
  llvm::DenseSet<std::pair<SILValue, const Operand *>> isOperandInValueUsesCache;

  /// Check that this operand appears in the use-chain of the value it uses.
  bool isOperandInValueUses(const Operand *operand) {
    SILValue value = operand->get();

    // First check the cache.
    if (isOperandInValueUsesCache.contains({value, operand}))
      return true;

    // Otherwise, compute the value and initialize the cache for each of the
    // operand's value uses.
    bool foundUse = false;
    for (auto *use : value->getUses()) {
      if (use == operand) {
        foundUse = true;
      }
      isOperandInValueUsesCache.insert({value, use});
    }

    return foundUse;
  }

  SILVerifier(const SILVerifier&) = delete;
  void operator=(const SILVerifier&) = delete;
public:
  bool isSILOwnershipEnabled() const {
    return F.getModule().getOptions().VerifySILOwnership;
  }

  void _require(bool condition, const Twine &complaint,
                const std::function<void()> &extraContext = nullptr) {
    if (condition) return;

    StringRef funcName;
    if (CurInstruction)
      funcName = CurInstruction->getFunction()->getName();
    else if (CurArgument)
      funcName = CurArgument->getFunction()->getName();
    if (ContinueOnFailure) {
      llvm::dbgs() << "Begin Error in function " << funcName << "\n";
    }

    llvm::dbgs() << "SIL verification failed: " << complaint << "\n";
    if (extraContext)
      extraContext();

    if (CurInstruction) {
      llvm::dbgs() << "Verifying instruction:\n";
      CurInstruction->printInContext(llvm::dbgs());
    } else if (CurArgument) {
      llvm::dbgs() << "Verifying argument:\n";
      CurArgument->printInContext(llvm::dbgs());
    }
    if (ContinueOnFailure) {
      llvm::dbgs() << "End Error in function " << funcName << "\n";
      return;
    }

    llvm::dbgs() << "In function:\n";
    F.print(llvm::dbgs());
    if (DumpModuleOnFailure) {
      // Don't do this by default because modules can be _very_ large.
      llvm::dbgs() << "In module:\n";
      F.getModule().print(llvm::dbgs());
    }

    // We abort by default because we want to always crash in
    // the debugger.
    if (AbortOnFailure)
      abort();
    else
      exit(1);
  }
#define require(condition, complaint) \
  _require(bool(condition), complaint ": " #condition)

  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireObjectType(SILType type, const Twine &valueDescription,
                     const char *typeName) {
    _require(type.isObject(), valueDescription + " must be an object");
    auto result = type.getAs<T>();
    _require(bool(result), valueDescription + " must have type " + typeName);
    return result;
  }
  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireObjectType(SILValue value, const Twine &valueDescription,
                     const char *typeName) {
    return _requireObjectType<T>(value->getType(), valueDescription, typeName);
  }
#define requireObjectType(type, value, valueDescription) \
  _requireObjectType<type>(value, valueDescription, #type)

  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireAddressType(SILType type, const Twine &valueDescription,
                      const char *typeName) {
    _require(type.isAddress(), valueDescription + " must be an address");
    auto result = type.getAs<T>();
    _require(bool(result), valueDescription + " must have type " + typeName);
    return result;
  }
  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireAddressType(SILValue value, const Twine &valueDescription,
                     const char *typeName) {
    return _requireAddressType<T>(value->getType(), valueDescription, typeName);
  }
#define requireAddressType(type, value, valueDescription) \
  _requireAddressType<type>(value, valueDescription, #type)

  template <class T>
  typename CanTypeWrapperTraits<T>::type
  _forbidObjectType(SILType type, const Twine &valueDescription,
                    const char *typeName) {
    _require(type.isObject(), valueDescription + " must be an object");
    auto result = type.getAs<T>();
    _require(!bool(result),
             valueDescription + " must not have type " + typeName);
    return result;
  }
  template <class T>
  typename CanTypeWrapperTraits<T>::type
  _forbidObjectType(SILValue value, const Twine &valueDescription,
                    const char *typeName) {
    return _forbidObjectType<T>(value->getType(), valueDescription, typeName);
  }
#define forbidObjectType(type, value, valueDescription)                        \
  _forbidObjectType<type>(value, valueDescription, #type)

  // Require that the operand is a non-optional, non-unowned reference-counted
  // type.
  void requireReferenceValue(SILValue value, const Twine &valueDescription) {
    require(value->getType().isObject(),
            valueDescription + " must be an object");
    require(value->getType().isReferenceCounted(F.getModule()) ||
                value->getType().isForeignReferenceType(),
            valueDescription + " must have reference semantics");
    forbidObjectType(UnownedStorageType, value, valueDescription);
  }

  // Require that the operand is a reference-counted type, or an Optional
  // thereof.
  void requireReferenceOrOptionalReferenceValue(SILValue value,
                                                const Twine &valueDescription) {
    require(value->getType().isObject(), valueDescription +" must be an object");
    
    auto objectTy = value->getType().unwrapOptionalType();
    
    require(objectTy.isReferenceCounted(F.getModule()),
            valueDescription + " must have reference semantics");
  }
  
  // Require that the operand is a type that supports reference storage
  // modifiers.
  void requireReferenceStorageCapableValue(SILValue value,
                                           const Twine &valueDescription) {
    requireReferenceOrOptionalReferenceValue(value, valueDescription);
    require(!value->getType().is<SILFunctionType>(),
            valueDescription + " cannot apply to a function type");
  }

  /// Require the operand to be `$Optional<Builtin.Executor>`.
  void requireOptionalExecutorType(SILValue value, const Twine &what) {
    auto type = value->getType();
    require(type.isObject(), what + " must be an object type");
    auto objectType = type.getASTType().getOptionalObjectType();
    require(objectType && objectType == M->getASTContext().TheExecutorType,
            what + " must be Optional<Builtin.Executor>");
  }

  /// Assert that two types are equal.
  void requireSameType(Type type1, Type type2, const Twine &complaint) {
    _require(type1->isEqual(type2), complaint,
             [&] { llvm::dbgs() << "  " << type1 << "\n  " << type2 << '\n'; });
  }

  /// Assert that two types are equal.
  void requireSameType(SILType type1, SILType type2, const Twine &complaint) {
    _require(type1 == type2, complaint,
             [&] { llvm::dbgs() << "  " << type1 << "\n  " << type2 << '\n'; });
  }

  /// Require two function types to be ABI-compatible.
  void requireABICompatibleFunctionTypes(CanSILFunctionType type1,
                                         CanSILFunctionType type2,
                                         const Twine &what,
                                         SILFunction &inFunction) {
    auto complain = [=](const char *msg) -> std::function<void()> {
      return [=]{
        llvm::dbgs() << "  " << msg << '\n'
                     << "  " << type1 << "\n  " << type2 << '\n';
      };
    };
    auto complainBy = [=](std::function<void()> msg) -> std::function<void()> {
      return [=]{
        msg();
        llvm::dbgs() << '\n';
        llvm::dbgs() << "  " << type1 << "\n  " << type2 << '\n';
      };
    };

    // If we didn't have a failure, return.
    auto Result = type1->isABICompatibleWith(type2, inFunction);
    if (Result.isCompatible())
      return;

    if (!Result.hasPayload()) {
      _require(false, what, complain(Result.getMessage().data()));
    } else {
      _require(false, what, complainBy([=] {
                 llvm::dbgs() << " " << Result.getMessage().data()
                              << ".\nParameter: " << Result.getPayload();
               }));
    }
  }

  void requireSameFunctionComponents(CanSILFunctionType type1,
                                     CanSILFunctionType type2,
                                     const Twine &what) {
    require(type1->getNumResults() == type2->getNumResults(),
            "results of " + what + " do not match in count");
    for (auto i : indices(type1->getResults())) {
      require(type1->getResults()[i] == type2->getResults()[i],
              "result " + Twine(i) + " of " + what + " do not match");
    }
    require(type1->getParameters().size() ==
            type2->getParameters().size(),
            "inputs of " + what + " do not match in count");
    for (auto i : indices(type1->getParameters())) {
      require(type1->getParameters()[i] ==
              type2->getParameters()[i],
              "input " + Twine(i) + " of " + what + " do not match");
    }
  }

  template <class T>
  T *requireValueKind(SILValue value, const Twine &what) {
    auto match = dyn_cast<T>(value);
    _require(match != nullptr, what, [=] { llvm::dbgs() << value; });
    return match;
  }

  static unsigned numInstsInFunction(const SILFunction &F) {
    unsigned numInsts = 0;
    for (auto &BB : F) {
      numInsts += std::distance(BB.begin(), BB.end());
    }
    return numInsts;
  }

  SILVerifier(const SILFunction &F, SILPassManager *passManager,
              bool SingleFunction, bool checkLinearLifetime)
      : M(F.getModule().getSwiftModule()), F(F),
        passManager(passManager),
        fnConv(F.getConventionsInContext()), TC(F.getModule().Types),
        SingleFunction(SingleFunction),
        checkLinearLifetime(checkLinearLifetime),
        Dominance(nullptr),
        InstNumbers(numInstsInFunction(F)),
        loadBorrowImmutabilityAnalysis(DEBlocks.get(), &F) {
    if (F.isExternalDeclaration())
      return;

    // Check to make sure that all blocks are well formed.  If not, the
    // SILVerifier object will explode trying to compute dominance info.
    unsigned InstIdx = 0;
    for (auto &BB : F) {
      require(!BB.empty(), "Basic blocks cannot be empty");
      require(isa<TermInst>(BB.back()),
              "Basic blocks must end with a terminator instruction");
      for (auto &I : BB)
        InstNumbers[&I] = InstIdx++;
    }

    Dominance.reset(new DominanceInfo(const_cast<SILFunction *>(&F)));

    auto *DebugScope = F.getDebugScope();
    require(DebugScope, "All SIL functions must have a debug scope");
    require(DebugScope->Parent.get<SILFunction *>() == &F,
            "Scope of SIL function points to different function");
  }

  // Checks dominance between two instructions.
  // This does not use DominanceInfo.properlyDominates, because for large basic
  // blocks it would result in quadratic behavior.
  bool properlyDominates(SILInstruction *a, SILInstruction *b) {
    auto aBlock = a->getParent(), bBlock = b->getParent();
    require(aBlock->getParent() == bBlock->getParent(),
            "instructions are not in the same function");

    // If the blocks are different, it's as easy as whether A's block
    // dominates B's block.
    if (aBlock != bBlock)
      return Dominance->properlyDominates(aBlock, bBlock);

    return InstNumbers[a] < InstNumbers[b];
  }

  void visitSILPhiArgument(SILPhiArgument *arg) {
    // Verify that the `isPhiArgument` property is sound:
    // - Phi arguments come from branches.
    // - Non-phi arguments have a single predecessor.
    assert(arg->isPhi() && "precondition");
    for (SILBasicBlock *predBB : arg->getParent()->getPredecessorBlocks()) {
      auto *TI = predBB->getTerminator();
      if (F.hasOwnership()) {
        require(isa<BranchInst>(TI), "All phi inputs must be branch operands.");

        // Address-only values are potentially unmovable when borrowed. See also
        // checkOwnershipForwardingInst. A phi implies a move of its arguments
        // because they can't necessarily all reuse the same storage.
        require((!arg->getType().isAddressOnly(F)
                 || arg->getOwnershipKind() != OwnershipKind::Guaranteed),
                "Guaranteed address-only phi not allowed--implies a copy");
      } else {
        // FIXME: when critical edges are removed and cond_br arguments are
        // disallowed, only allow BranchInst.
        require(isa<BranchInst>(TI) || isa<CondBranchInst>(TI),
                "All phi argument inputs must be from branches.");
      }
    }
    if (arg->isPhi()) {
      // As a property of well-formed SIL, we disallow address-type
      // phis. Supporting them would prevent reliably reasoning about the
      // underlying storage of memory access. This reasoning is important for
      // diagnosing violations of memory access rules and supporting future
      // optimizations such as bitfield packing. Address-type block arguments
      // also create unnecessary complexity for SIL optimization passes that
      // need to reason about memory aliasing.
      require(!arg->getType().isAddress(),
              "Block arguments cannot be addresses");
    }
  }

  void visitSILArgument(SILArgument *arg) {
    CurArgument = arg;
    checkLegalType(arg->getFunction(), arg, nullptr);

    // Ensure flags on the argument are not stale.
    require(!arg->getFunction()->hasOwnership() ||
                computeIsReborrow(arg) == arg->isReborrow(),
            "Stale reborrow flag");

    if (checkLinearLifetime) {
      checkValueBaseOwnership(arg);
    }
    if (auto *phiArg = dyn_cast<SILPhiArgument>(arg)) {
      if (phiArg->isPhi())
        visitSILPhiArgument(phiArg);
      else {
        // A non-phi BlockArgument must have a single predecessor unless it is
        // unreachable.
        require(arg->getParent()->pred_empty()
                    || arg->getParent()->getSinglePredecessorBlock(),
                "Non-branch terminator must have a unique successor.");
      }
      return;
    }

    // If we are not in lowered SIL and have an in_guaranteed function argument,
    // verify that we do not mutate or consume it.
    auto *fArg = cast<SILFunctionArgument>(arg);
    if (fArg->getType().isAddress())
      checkAddressWalkerCanVisitAllTransitiveUses(fArg);

    if (fArg->getModule().getStage() == SILStage::Lowered ||
        !fArg->getType().isAddress() ||
        !fArg->hasConvention(SILArgumentConvention::Indirect_In_Guaranteed))
      return;

    require(!ImmutableAddressUseVerifier().isMutatingOrConsuming(fArg),
            "Found mutating or consuming use of an in_guaranteed parameter?!");
  }

  void visitSILInstruction(SILInstruction *I) {
    CurInstruction = I;
    checkSILInstruction(I);

    // Check the SILLLocation attached to the instruction,
    // as well as debug-variable-carrying instructions.
    checkInstructionsDebugInfo(I);

    // Check ownership and types.
    SILFunction *F = I->getFunction();
    assert(F && "Expected value base with parent function");

    for (auto result : I->getResults()) {
      checkLegalType(F, result, I);
      if (checkLinearLifetime) {
        checkValueBaseOwnership(result);
      }
    }
  }

  void checkValueBaseOwnership(ValueBase *V) {
    // If ownership is not enabled, bail.
    if (!isSILOwnershipEnabled())
      return;

    SILFunction *F = V->getFunction();
    assert(F && "Expected value base with parent function");
    // If we do not have qualified ownership, then do not verify value base
    // ownership.
    if (!F->hasOwnership()) {
      require(SILValue(V)->getOwnershipKind() == OwnershipKind::None,
              "Once ownership is gone, all values should have none ownership");
      return;
    }
    SILValue(V).verifyOwnership(DEBlocks.get());
  }

  void checkSILInstruction(SILInstruction *I) {
    const SILBasicBlock *BB = I->getParent();
    require(BB, "Instruction with null parent");

    // Check that non-terminators look ok.
    if (!isa<TermInst>(I)) {
      require(!BB->empty(), "Can't be in a parent block if it is empty");
      if (!I->isStaticInitializerInst()) {
        require(&*BB->rbegin() != I,
                "Non-terminators cannot be the last in a block");
      }
    } else {
      require(&*BB->rbegin() == I,
              "Terminator must be the last in block");
    }

    // Verify that all of our uses are in this function.
    for (auto result : I->getResults()) {
      for (Operand *use : result->getUses()) {
        auto user = use->getUser();
        require(user, "instruction user is null?");
        require(isa<SILInstruction>(user),
                "instruction used by non-instruction");
        auto userI = cast<SILInstruction>(user);
        require(userI->getParent(),
                "instruction used by unparented instruction");
        if (I->isStaticInitializerInst()) {
          require(userI->getParent() == BB,
                "instruction used by instruction not in same static initializer");
        } else {
          require(userI->getFunction() == &F,
                  "instruction used by instruction in different function");
        }

        auto operands = userI->getAllOperands();
        require(operands.begin() <= use && use <= operands.end(),
                "use doesn't actually belong to instruction it claims to");
      }
    }

    // Verify some basis structural stuff about an instruction's operands.
    for (auto &operand : I->getAllOperands()) {
      require(operand.get(), "instruction has null operand");
      require(!isa<PlaceholderValue>(operand.get()),
              "instruction has placeholder operand");

      if (auto *valueI = operand.get()->getDefiningInstruction()) {
        require(valueI->getParent(),
                "instruction uses value of unparented instruction");
        if (I->isStaticInitializerInst()) {
          require(valueI->getParent() == BB,
              "instruction uses value which is not in same static initializer");
        } else {
          require(valueI->getFunction() == &F,
                  "instruction uses value of instruction from another function");
          require(properlyDominates(valueI, I),
                  "instruction isn't dominated by its operand");
        }
      }
      
      if (auto *valueBBA = dyn_cast<SILArgument>(operand.get())) {
        require(!I->isStaticInitializerInst(),
                "static initializer inst cannot refer to SILArgument");
        require(valueBBA->getParent(),
                "instruction uses value of unparented instruction");
        require(valueBBA->getFunction() == &F,
                "bb argument value from another function");
        require(Dominance->dominates(valueBBA->getParent(), I->getParent()),
                "instruction isn't dominated by its bb argument operand");
      }

      require(operand.getUser() == I,
              "instruction's operand's owner isn't the instruction");
      require(isOperandInValueUses(&operand), "operand value isn't used by operand");

      if (operand.isTypeDependent()) {
        require(isa<SILInstruction>(I),
               "opened archetype operand should refer to a SILInstruction");
      }

      // Make sure that if operand is generic that its primary archetypes match
      // the function context.
      checkLegalType(I->getFunction(), operand.get(), I);

      // If we are not in OSSA, our operand constraint should be invalid for a
      // type dependent operand (that is Optional::None) and if we have a non
      // type dependent operand then we should have a constraint of
      // OwnershipKind::Any, UseLifetimeConstraint::NonLifetimeEnding.
      if (!I->getFunction()->hasOwnership()) {
        auto constraint = operand.getOwnershipConstraint();
        require(constraint.getPreferredKind() == OwnershipKind::Any &&
                    constraint.getLifetimeConstraint() ==
                        UseLifetimeConstraint::NonLifetimeEnding,
                "In non-ossa all non-type dependent operands must have a "
                "constraint of Any, NonLifetimeEnding");
      } else {
        // Perform some structural checks on the operand if we have ownership.

        // Make sure that our operand constraint isn't Unowned. There do not
        // exist in SIL today any instructions that require an Unowned
        // value. This is because we want to always allow for guaranteed and
        // owned values to be passed as values to operands that take unowned.
        auto constraint = operand.getOwnershipConstraint();
        require(constraint.getPreferredKind() != OwnershipKind::Unowned,
                "Operand constraint should never have an unowned preferred "
                "kind since guaranteed and owned values can always be passed "
                "in unowned positions");

        require(operand.getOperandOwnership() !=
                        OperandOwnership::InteriorPointer ||
                    InteriorPointerOperandKind::get(&operand) !=
                        InteriorPointerOperandKind::Invalid,
                "All operands with InteriorPointer operand ownership should be "
                "added to the InteriorPointerOperand utility");
      }
    }

    if (I->getFunction()->hasOwnership()) {
      if (auto fwdOp = ForwardingOperation(I))
        checkOwnershipForwardingInst(fwdOp);
    }
  }

  // Verify the result of any forwarding terminator, including switch_enum.
  void checkForwardedTermResult(OwnershipForwardingTermInst *term,
                                SILBasicBlock *destBB) {
    require(destBB->getNumArguments() == 1,
            "OwnershipForwardingTermInst needs a single result");
    auto *arg = destBB->getArgument(0);
    auto argKind = arg->getOwnershipKind();
    // A terminator may cast a nontrivial to a trivial type. e.g. a trivial
    // payload in a nontrivial enum. If the result is trivial, it no longer
    // requires ownership.
    if (arg->getType().isTrivial(F) && argKind == OwnershipKind::None)
      return;

    require(argKind == term->getForwardingOwnershipKind(),
            "OwnershipForwardingTermInst nontrivial result "
            "must have the same ownership");
  }

  /// A terminator result is forwarded from the terminator's operand. Forwarding
  /// a nontrivial value to another nontrivial value can never gain or lose
  /// ownership information. If the terminator's operand has ownership and the
  /// result is nontrivial, then the result must have identical ownership.
  ///
  /// The terminator result can only "lose" ownership if the operand is
  /// nontrivial but the result is trivial. It is still valid for the trivial
  /// result to retain the operand's ownership, but unnecessary and produces
  /// less efficient SIL.
  void checkOwnershipForwardingTermInst(OwnershipForwardingTermInst *term) {
    // Verifying switch_enum ownership requires evaluating the payload
    // types. These are fully verified by checkSwitchEnumInst.
    if (isa<SwitchEnumInst>(term))
      return;

    for (auto &succ : term->getSuccessors()) {
      checkForwardedTermResult(term, succ.getBB());
    }
  }

  /// For an instruction \p i that forwards ownership from an operand to one
  /// of its results, check forwarding invariants.
  void checkOwnershipForwardingInst(ForwardingOperation fwdOp) {
    auto ownership = fwdOp.getForwardingOwnershipKind();

    if (fwdOp.canForwardOwnedCompatibleValuesOnly()) {
      ValueOwnershipKind kind = OwnershipKind::Owned;
      require(kind.isCompatibleWith(ownership),
              "OwnedFirstArgForwardingSingleValueInst's ownership kind must be "
              "compatible with owned");
    }

    if (fwdOp.canForwardGuaranteedCompatibleValuesOnly()) {
      ValueOwnershipKind kind = OwnershipKind::Guaranteed;
      require(kind.isCompatibleWith(ownership),
              "GuaranteedFirstArgForwardingSingleValueInst's ownership kind "
              "must be compatible with guaranteed");
    }

    if (auto *term = dyn_cast<OwnershipForwardingTermInst>(*fwdOp)) {
      checkOwnershipForwardingTermInst(term);
    }

    // Address-only values are potentially unmovable when borrowed. Ensure that
    // guaranteed address-only values are forwarded with the same
    // representation. Non-destructive projection is allowed. Aggregation and
    // destructive disaggregation is not allowed. See SIL.rst, Forwarding
    // Addres-Only Values.
    if (ownership == OwnershipKind::Guaranteed && fwdOp.isAddressOnly()) {
      require(fwdOp.hasSameRepresentation(),
              "Forwarding a guaranteed address-only value requires the same "
              "representation since no move or copy is allowed.");
    }
  }

  void checkDebugVariable(SILInstruction *inst) {
    llvm::Optional<SILDebugVariable> varInfo;
    if (auto *di = dyn_cast<AllocStackInst>(inst))
      varInfo = di->getVarInfo();
    else if (auto *di = dyn_cast<AllocBoxInst>(inst))
      varInfo = di->getVarInfo();
    else if (auto *di = dyn_cast<DebugValueInst>(inst))
      varInfo = di->getVarInfo();

    if (!varInfo)
      return;

    // Retrieve debug variable type
    SILType DebugVarTy;
    if (varInfo->Type)
      DebugVarTy = *varInfo->Type;
    else {
      // Fetch from related SSA value
      switch (inst->getKind()) {
      case SILInstructionKind::AllocStackInst:
      case SILInstructionKind::AllocBoxInst:
        DebugVarTy = inst->getResult(0)->getType();
        break;
      case SILInstructionKind::DebugValueInst:
        DebugVarTy = inst->getOperand(0)->getType();
        if (DebugVarTy.isAddress()) {
          // FIXME: op_deref could be applied to address types only.
          // FIXME: Add this check
          if (varInfo->DIExpr.startsWithDeref())
            DebugVarTy = DebugVarTy.getObjectType();
        }
        break;
      default:
        llvm_unreachable("impossible instruction kind");
      }
    }

    auto *debugScope = inst->getDebugScope();
    if (varInfo->ArgNo)
      require(!varInfo->Name.empty(), "function argument without a name");

    // Check that there is at most one debug variable defined for each argument
    // slot if our debug scope is not an inlined call site.
    //
    // This catches SIL transformations that accidentally remove inline
    // information (stored in the SILDebugScope) from debug-variable-carrying
    // instructions.
    if (debugScope && !debugScope->InlinedCallSite)
      if (unsigned argNum = varInfo->ArgNo) {
        // It is a function argument.
        if (argNum < DebugVars.size() && !DebugVars[argNum].first.empty()) {
          require(DebugVars[argNum].first == varInfo->Name,
                  "Scope contains conflicting debug variables for one function "
                  "argument");
          // The source variable might change its location (e.g. due to
          // optimizations). Check for most common transformations (e.g. loading
          // to SSA value and vice versa) as well
          require(DebugVars[argNum].second == DebugVarTy ||
                  (DebugVars[argNum].second.isAddress() &&
                   DebugVars[argNum].second.getObjectType() == DebugVarTy) ||
                  (DebugVarTy.isAddress() &&
                   DebugVars[argNum].second == DebugVarTy.getObjectType()),
                  "conflicting debug variable type!");
          DebugVars[argNum].second = DebugVarTy;
        } else {
          // Reserve enough space.
          while (DebugVars.size() <= argNum) {
            DebugVars.push_back({StringRef(), SILType()});
          }
        }
        DebugVars[argNum] = {varInfo->Name, DebugVarTy};
      }

    // Check the (auxiliary) debug variable scope
    if (const SILDebugScope *VarDS = varInfo->Scope)
      require(VarDS->getInlinedFunction() == debugScope->getInlinedFunction(),
              "Scope of the debug variable should have the same parent function"
              " as that of instruction.");

    // Check debug info expression
    if (const auto &DIExpr = varInfo->DIExpr) {
      for (auto It = DIExpr.element_begin(), ItEnd = DIExpr.element_end();
           It != ItEnd;) {
        require(It->getKind() == SILDIExprElement::OperatorKind,
                "dangling di-expression operand");
        auto Op = It->getAsOperator();
        const auto *DIExprInfo = SILDIExprInfo::get(Op);
        require(DIExprInfo, "unrecognized di-expression operator");
        ++It;
        // Check operand kinds
        for (auto OpK : DIExprInfo->OperandKinds)
          require(It != ItEnd && (It++)->getKind() == OpK,
                  "di-expression operand kind mismatch");

        if (Op == SILDIExprOperator::Fragment)
          require(It == ItEnd, "op_fragment directive needs to be at the end "
                               "of a di-expression");
      }
    }
  }

  void checkInstructionsDebugInfo(SILInstruction *inst) {
    // First verify structural debug info information.
    inst->verifyDebugInfo();

    // Check the debug scope.
    auto *debugScope = inst->getDebugScope();
    if (debugScope && !maybeScopeless(*inst))
      require(debugScope, "instruction has a location, but no scope");
    require(!debugScope ||
                debugScope->getParentFunction() == inst->getFunction(),
            "debug scope of instruction belongs to a different function");

    checkDebugVariable(inst);
  }

  /// Check that the types of this value producer are all legal in the function
  /// context in which it exists.
  void checkLegalType(SILFunction *F, ValueBase *value, SILInstruction *I) {
    SILType type = value->getType();
    if (type.is<SILTokenType>()) {
      require(isLegalSILTokenProducer(value),
              "SIL tokens can only be produced as the results of specific "
              "instructions");
      return;
    }

    checkLegalType(F, type, I);
  }

  static bool isLegalSILTokenProducer(SILValue value) {
    if (auto *baResult = isaResultOf<BeginApplyInst>(value))
      return baResult->isBeginApplyToken();

    if (isa<OpenPackElementInst>(value))
      return true;

    // Add more token cases here as they arise.

    return false;
  }

  /// Check that the given type is a legal SIL value type.
  void checkLegalType(SILFunction *F, SILType type, SILInstruction *I) {
    checkLegalSILType(F, type.getASTType(), I);
  }

  /// Check that the given type is a legal SIL value type.
  void checkLegalSILType(SILFunction *F, CanType rvalueType, SILInstruction *I) {
    // These types should have been removed by lowering.
    require(!isa<LValueType>(rvalueType),
            "l-value types are not legal in SIL");
    require(!isa<AnyFunctionType>(rvalueType),
            "AST function types are not legal in SIL");

    // Tuples should have had their element lowered.
    if (auto tuple = dyn_cast<TupleType>(rvalueType)) {
      for (auto eltTy : tuple.getElementTypes()) {
        checkLegalSILType(F, eltTy, I);
      }
      return;
    }

    // Optionals should have had their objects lowered.
    if (auto objectType = rvalueType.getOptionalObjectType()) {
      return checkLegalSILType(F, objectType, I);
    }

    // Metatypes should have explicit representations.
    if (auto metatype = dyn_cast<AnyMetatypeType>(rvalueType)) {
      require(metatype->hasRepresentation(),
              "metatypes in SIL must have a representation");;
      // fallthrough for archetype check
    }

    rvalueType.visit([&](CanType t) {
      auto A = dyn_cast<ArchetypeType>(t);
      if (!A)
        return;
      require(isArchetypeValidInFunction(A, F),
              "Operand is of an ArchetypeType that does not exist in the "
              "Caller's generic param list.");
      if (auto localA = getLocalArchetypeOf(A)) {
        auto *openingInst =
            F->getModule().getRootLocalArchetypeDefInst(localA.getRoot(), F);
        require(I == nullptr || openingInst == I ||
                    properlyDominates(openingInst, I),
                "Use of a local archetype should be dominated by a "
                "definition of this root local archetype");
      }
    });
  }

  /// \return True if all of the users of the AllocStack instruction \p ASI are
  /// inside the same basic block.
  static bool isSingleBlockUsage(AllocStackInst *ASI, DominanceInfo *Dominance){
    SILBasicBlock *BB = ASI->getParent();
    for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI)
      if (UI->getUser()->getParent() != BB &&
          Dominance->isReachableFromEntry(UI->getUser()->getParent()))
        return false;

    return true;
  }

  void checkAllocStackInst(AllocStackInst *AI) {
    require(AI->getType().isAddress(),
            "result of alloc_stack must be an address type");

    verifyLocalArchetype(AI, AI->getElementType().getASTType());

    require(!AI->isVarInfoInvalidated() || !bool(AI->getVarInfo()),
            "AllocStack Var Info should be None if invalidated");

    checkAddressWalkerCanVisitAllTransitiveUses(AI);

    // There used to be a check if all uses of ASI are inside the alloc-dealloc
    // range. But apparently it can be the case that ASI has uses after the
    // dealloc_stack. This can come up if the source contains a
    // withUnsafePointer where the pointer escapes.
    // It's illegal code but the compiler should not crash on it.
  }

  void checkAllocPackInst(AllocPackInst *AI) {
    requireAddressType(SILPackType, AI->getType(),
                       "result of alloc_pack must be an address of "
                       "lowered pack type");
  }

  void checkAllocRefBase(AllocRefInstBase *ARI) {
    requireReferenceValue(ARI, "Result of alloc_ref");
    verifyLocalArchetype(ARI, ARI->getType().getASTType());
    auto Types = ARI->getTailAllocatedTypes();
    auto Counts = ARI->getTailAllocatedCounts();
    unsigned NumTypes = Types.size();
    require(NumTypes == Counts.size(), "Mismatching types and counts");
    require(NumTypes == 0 || !ARI->isObjC(),
            "Can't tail allocate with ObjC class");
    for (unsigned Idx = 0; Idx < NumTypes; ++Idx) {
      verifyLocalArchetype(ARI, Types[Idx].getASTType());
      require(Counts[Idx].get()->getType().is<BuiltinIntegerType>(),
              "count needs integer type");
    }
  }

  void checkAllocRefInst(AllocRefInst *AI) {
    require(AI->isObjC() || AI->getType().getClassOrBoundGenericClass(),
            "alloc_ref must allocate class");
    checkAllocRefBase(AI);
  }

  void checkAllocRefDynamicInst(AllocRefDynamicInst *ARDI) {
    SILValue Metadata = ARDI->getMetatypeOperand();
    require(Metadata->getType().is<AnyMetatypeType>(),
            "operand of alloc_ref_dynamic must be of metatype type");
    auto metaTy = Metadata->getType().castTo<AnyMetatypeType>();
    require(metaTy->hasRepresentation(),
            "operand of alloc_ref_dynamic must have a metatype representation");
    if (ARDI->isObjC()) {
      require(metaTy->getRepresentation() == MetatypeRepresentation::ObjC,
              "alloc_ref_dynamic @objc requires operand of ObjC metatype");
    } else {
      require(metaTy->getRepresentation() == MetatypeRepresentation::Thick,
              "alloc_ref_dynamic requires operand of thick metatype");
    }
    checkAllocRefBase(ARDI);
  }

  /// Check the substitutions passed to an apply or partial_apply.
  CanSILFunctionType checkApplySubstitutions(SubstitutionMap subs,
                                             SILType calleeTy) {
    auto fnTy = requireObjectType(SILFunctionType, calleeTy, "callee operand");

    // If there are substitutions, verify them and apply them to the callee.
    if (!subs.hasAnySubstitutableParams()) {
      require(!fnTy->isPolymorphic(),
              "callee of apply without substitutions must not be polymorphic");
      return fnTy;
    }
    require(fnTy->isPolymorphic(),
            "callee of apply with substitutions must be polymorphic");

    // Each archetype occurring in the substitutions list should belong to the
    // current function.
    for (auto replacementType : subs.getReplacementTypes()) {
      replacementType->getCanonicalType().visit([&](CanType t) {
        auto A = dyn_cast<ArchetypeType>(t);
        if (!A)
          return;
        require(isArchetypeValidInFunction(A, &F),
                "Replacement type of a substitution contains an ArchetypeType "
                "that does not exist in the Caller's generic param list.");
      });
    }

    if (subs.getGenericSignature().getCanonicalSignature() !=
          fnTy->getInvocationGenericSignature().getCanonicalSignature()) {
      llvm::dbgs() << "substitution map's generic signature: ";
      subs.getGenericSignature()->print(llvm::dbgs());
      llvm::dbgs() << "\n";
      llvm::dbgs() << "callee's generic signature: ";
      fnTy->getInvocationGenericSignature()->print(llvm::dbgs());
      llvm::dbgs() << "\n";
      require(false,
              "Substitution map does not match callee in apply instruction");
    }
    // Apply the substitutions.
    return fnTy->substGenericArgs(F.getModule(), subs, F.getTypeExpansionContext());
  }

  /// Check that for each local archetype or dynamic self type in substitutions
  /// or the called type, there is a type dependent operand.
  void checkApplyTypeDependentArguments(ApplySite AS) {
    SILInstruction *AI = AS.getInstruction();

    llvm::DenseSet<SILInstruction *> allOpeningInsts;
    unsigned hasDynamicSelf = 0;

    // Function to collect local archetypes in allOpeningInsts and set
    // hasDynamicSelf.
    auto handleType = [&](CanType Ty) {
      if (const auto A = dyn_cast<LocalArchetypeType>(Ty)) {
        require(isArchetypeValidInFunction(A, AI->getFunction()),
                "Archetype to be substituted must be valid in function.");

        const auto root = A.getRoot();

        // Check that opened archetypes are properly tracked inside
        // the current function.
        auto *openingInst = F.getModule().getRootLocalArchetypeDefInst(
            root, AI->getFunction());
        require(openingInst == AI || properlyDominates(openingInst, AI),
                "Use of a local archetype should be dominated by a "
                "definition of this root opened archetype");

        // Remember all the opening instructions.  We unique by instruction
        // identity when building the list of type dependency operands, and
        // some instructions can open multiple archetypes.
        allOpeningInsts.insert(openingInst);
      }
      if (Ty->hasDynamicSelfType()) {
        hasDynamicSelf = 1;
      }
    };

    // Search for local archetypes and dynamic self.
    for (auto Replacement : AS.getSubstitutionMap().getReplacementTypes()) {
      Replacement->getCanonicalType().visit(handleType);
    }
    AS.getSubstCalleeType().visit(handleType);

    require(allOpeningInsts.size() + hasDynamicSelf ==
                AI->getTypeDependentOperands().size(),
            "Number of local archetypes and dynamic self in the substitutions "
            "list should match the number of type dependent operands");

    for (auto &Op : AI->getTypeDependentOperands()) {
      auto V = Op.get();
      if (isa<SILArgument>(V)) {
        require(hasDynamicSelf,
                "dynamic self operand without dynamic self type");
        require(AI->getFunction()->hasDynamicSelfMetadata(),
                "self metadata operand in function without self metadata param");
        require((ValueBase *)V == AI->getFunction()->getDynamicSelfMetadata(),
                "wrong self metadata operand");
      } else {
        auto DI = V->getDefiningInstruction();
        require(DI,
                "local archetype operand should refer to a SIL instruction");
        require(allOpeningInsts.count(DI),
                "local archetype operand does not correspond to any local "
                "archetype from the substitutions list");

        bool matchedDependencyResult = false;
        DI->forEachDefinedLocalArchetype(
            [&](CanLocalArchetypeType archetype, SILValue dependency) {
          if (dependency == V)
            matchedDependencyResult = true;
        });
        require(matchedDependencyResult,
                "local archetype operand was not the dependency result "
                "of the opening instruction");
      }
    }
  }

  void checkFullApplySite(FullApplySite site) {
    checkApplyTypeDependentArguments(site);

    // Then make sure that we have a type that can be substituted for the
    // callee.
    auto substTy = checkApplySubstitutions(site.getSubstitutionMap(),
                                           site.getCallee()->getType());
    require(site.getOrigCalleeType()->getRepresentation() ==
            site.getSubstCalleeType()->getRepresentation(),
            "calling convention difference between types");

    require(!site.getSubstCalleeType()->isPolymorphic(),
            "substituted callee type should not be generic");

    requireSameType(SILType::getPrimitiveObjectType(substTy),
                    SILType::getPrimitiveObjectType(site.getSubstCalleeType()),
            "substituted callee type does not match substitutions");

    // Check that the arguments and result match.
    SILFunctionConventions substConv(substTy, F.getModule());
    require(site.getNumArguments() == substConv.getNumSILArguments(),
            "apply doesn't have right number of arguments for function");
    for (size_t i = 0, size = site.getNumArguments(); i < size; ++i) {
      requireSameType(
          site.getArguments()[i]->getType(),
          substConv.getSILArgumentType(i, F.getTypeExpansionContext()),
          "operand of 'apply' doesn't match function input type");
    }
  }

  void checkApplyInst(ApplyInst *AI) {
    checkFullApplySite(AI);

    SILFunctionConventions calleeConv(AI->getSubstCalleeType(), F.getModule());
    requireSameType(
        AI->getType(), calleeConv.getSILResultType(F.getTypeExpansionContext()),
        "type of apply instruction doesn't match function result type");
    if (AI->isNonThrowing()) {
      require(calleeConv.funcTy->hasErrorResult(),
              "nothrow flag used for callee without error result");
    } else {
      require(!calleeConv.funcTy->hasErrorResult(),
              "apply instruction cannot call function with error result");
    }

    if (AI->isNonAsync()) {
      require(calleeConv.funcTy->isAsync(),
              "noasync flag used for sync callee");
    } else {
      require(!calleeConv.funcTy->isAsync() || AI->getFunction()->isAsync(),
              "cannot call an async function from a non async function");
    }

    require(!calleeConv.funcTy->isCoroutine(),
            "cannot call coroutine with normal apply");
  }

  void checkTryApplyInst(TryApplyInst *AI) {
    checkFullApplySite(AI);

    SILFunctionConventions calleeConv(AI->getSubstCalleeType(), F.getModule());

    require(!calleeConv.funcTy->isCoroutine(),
            "cannot call coroutine with normal apply");

    if (AI->isNonAsync()) {
      require(calleeConv.funcTy->isAsync(),
              "noasync flag used for sync callee");
    } else {
      require(!calleeConv.funcTy->isAsync() || AI->getFunction()->isAsync(),
              "cannot call an async function from a non async function");
    }

    auto normalBB = AI->getNormalBB();
    require(normalBB->args_size() == 1,
            "normal destination of try_apply must take one argument");
    requireSameType((*normalBB->args_begin())->getType(),
                    calleeConv.getSILResultType(F.getTypeExpansionContext()),
                    "normal destination of try_apply must take argument "
                    "of normal result type");

    auto errorBB = AI->getErrorBB();
    require(calleeConv.funcTy->hasErrorResult(),
            "try_apply must call function with error result");
    require(errorBB->args_size() == 1,
            "error destination of try_apply must take one argument");
    requireSameType((*errorBB->args_begin())->getType(),
                    calleeConv.getSILErrorType(F.getTypeExpansionContext()),
                    "error destination of try_apply must take argument "
                    "of error result type");
  }

  void checkBeginApplyInst(BeginApplyInst *AI) {
    checkFullApplySite(AI);

    SILFunctionConventions calleeConv(AI->getSubstCalleeType(), F.getModule());
    auto yieldResults = AI->getYieldedValues();
    auto yields = calleeConv.getYields();
    require(yields.size() == yieldResults.size(),
            "length mismatch in callee yields vs. begin_apply results");
    for (auto i : indices(yields)) {
      requireSameType(
          yieldResults[i]->getType(),
          calleeConv.getSILType(yields[i], F.getTypeExpansionContext()),
          "callee yield type does not match begin_apply result type");
    }

    if (AI->isNonThrowing()) {
      require(calleeConv.funcTy->hasErrorResult(),
              "nothrow flag used for callee without error result");
    } else {
      require(!calleeConv.funcTy->hasErrorResult(),
              "begin_apply instruction cannot call function with error result");
    }

    require(calleeConv.funcTy->getCoroutineKind() == SILCoroutineKind::YieldOnce,
            "must call yield_once coroutine with begin_apply");
    require(!calleeConv.funcTy->isAsync() || AI->getFunction()->isAsync(),
            "cannot call an async function from a non async function");
  }

  void checkAbortApplyInst(AbortApplyInst *AI) {
    require(getAsResultOf<BeginApplyInst>(AI->getOperand())->isBeginApplyToken(),
            "operand of abort_apply must be a begin_apply");
  }

  void checkEndApplyInst(EndApplyInst *AI) {
    require(getAsResultOf<BeginApplyInst>(AI->getOperand())->isBeginApplyToken(),
            "operand of end_apply must be a begin_apply");
  }

  void verifyLLVMIntrinsic(BuiltinInst *BI, llvm::Intrinsic::ID ID) {
    // Certain llvm intrinsic require constant values as their operands.
    // Consequently, these must not be phi nodes (aka. basic block arguments).
    switch (ID) {
    default:
      break;
    case llvm::Intrinsic::ctlz: // llvm.ctlz
    case llvm::Intrinsic::cttz: // llvm.cttz
      break;
    case llvm::Intrinsic::memcpy:
    case llvm::Intrinsic::memmove:
    case llvm::Intrinsic::memset:
      require(!isa<SILArgument>(BI->getArguments()[3]),
              "isvolatile argument of memory intrinsics must be an integer "
              "literal");
      break;
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::lifetime_end:
    case llvm::Intrinsic::invariant_start:
      require(!isa<SILArgument>(BI->getArguments()[0]),
              "size argument of memory use markers must be an integer literal");
      break;
    case llvm::Intrinsic::invariant_end:
      require(!isa<SILArgument>(BI->getArguments()[1]),
              "llvm.invariant.end parameter #2 must be an integer literal");
      break;
    }
  }

  void checkPartialApplyInst(PartialApplyInst *PAI) {
    auto resultInfo = requireObjectType(SILFunctionType, PAI,
                                        "result of partial_apply");
    verifySILFunctionType(resultInfo);
    require(resultInfo->getExtInfo().hasContext(),
            "result of closure cannot have a thin function type");

    checkApplyTypeDependentArguments(PAI);

    auto substTy = checkApplySubstitutions(PAI->getSubstitutionMap(),
                                        PAI->getCallee()->getType());

    require(!PAI->getSubstCalleeType()->isPolymorphic(),
            "substituted callee type should not be generic");

    requireSameType(SILType::getPrimitiveObjectType(substTy),
                    SILType::getPrimitiveObjectType(PAI->getSubstCalleeType()),
            "substituted callee type does not match substitutions");

    // The arguments must match the suffix of the original function's input
    // types.
    require(PAI->getArguments().size() +
              resultInfo->getParameters().size()
              == substTy->getParameters().size(),
            "result of partial_apply should take as many inputs as were not "
            "applied by the instruction");

    SILFunctionConventions substConv(substTy, F.getModule());
    unsigned appliedArgStartIdx =
        substConv.getNumSILArguments() - PAI->getNumArguments();
    for (auto p : llvm::enumerate(PAI->getArguments())) {
      requireSameType(
          p.value()->getType(),
          substConv.getSILArgumentType(appliedArgStartIdx + p.index(),
                                       F.getTypeExpansionContext()),
          "applied argument types do not match suffix of function type's "
          "inputs");
    }

    // The arguments to the result function type must match the prefix of the
    // original function's input types.
    for (unsigned i = 0, size = resultInfo->getParameters().size();
         i < size; ++i) {
      require(resultInfo->getParameters()[i] ==
              substTy->getParameters()[i],
              "inputs to result function type do not match unapplied inputs "
              "of original function");
    }

    require(resultInfo->getNumResults() == substTy->getNumResults(),
            "applied results do not agree in count with function type");
    for (unsigned i = 0, size = resultInfo->getNumResults(); i < size; ++i) {
      auto originalResult = resultInfo->getResults()[i];
      auto expectedResult = substTy->getResults()[i];

      // The "returns inner pointer" convention doesn't survive through a
      // partial application, since the thunk takes responsibility for
      // lifetime-extending 'self'.
      if (expectedResult.getConvention()
            == ResultConvention::UnownedInnerPointer) {
        expectedResult = SILResultInfo(
            expectedResult.getReturnValueType(F.getModule(), substTy,
                                              F.getTypeExpansionContext()),
            ResultConvention::Unowned);
        require(originalResult == expectedResult,
                "result type of result function type for partially applied "
                "@unowned_inner_pointer function should have @unowned"
                "convention");

      // The "autoreleased" convention doesn't survive through a
      // partial application, since the thunk takes responsibility for
      // retaining the return value.
      } else if (expectedResult.getConvention()
            == ResultConvention::Autoreleased) {
        expectedResult = SILResultInfo(
            expectedResult.getReturnValueType(F.getModule(), substTy,
                                              F.getTypeExpansionContext()),
            ResultConvention::Owned);
        require(originalResult == expectedResult,
                "result type of result function type for partially applied "
                "@autoreleased function should have @owned convention");

      } else {
        require(originalResult == expectedResult,
                "result type of result function type does not match original "
                "function");
      }
    }

    // TODO: Impose additional constraints when partial_apply when the
    // -disable-sil-partial-apply flag is enabled. We want to reduce
    // partial_apply to being only a means of associating a closure invocation
    // function with its context.
    //
    // When we reach that point, we should be able to more deeply redesign
    // PartialApplyInst to simplify the representation to carry a single
    // argument.
    if (PAI->getModule().getOptions().DisableSILPartialApply) {
      // Should only be one context argument.
      require(PAI->getArguments().size() == 1,
              "partial_apply should have a single context argument");
      
      // Callee should already have the thin convention, and result should be
      // thick.
      require(resultInfo->getRepresentation() ==
                SILFunctionTypeRepresentation::Thick,
              "partial_apply result should have thick convention");
      require(PAI->getCallee()->getType().castTo<SILFunctionType>()
                ->getRepresentation() ==
              SILFunctionTypeRepresentation::Thin,
              "partial_apply callee should have thin convention");
      
      // TODO: Check that generic signature matches box's generic signature,
      // once we have boxes with generic signatures.
      require(!PAI->getCalleeFunction()->getGenericEnvironment(),
              "partial_apply context must capture generic environment for "
              "callee");
      
      // Result's callee convention should match context argument's convention.
      require(substTy->getParameters().back().getConvention()
                == resultInfo->getCalleeConvention(),
              "partial_apply context argument must have the same convention "
              "as the resulting function's callee convention");
      
      auto isSwiftRefcounted = [](SILType t) -> bool {
        if (t.is<SILBoxType>())
          return true;
        if (t.getASTType() == t.getASTContext().TheNativeObjectType)
          return true;
        if (auto clazz = t.getClassOrBoundGenericClass())
          // Must be a class defined in Swift.
          return clazz->hasKnownSwiftImplementation();
        return false;
      };
      
      // The context argument must be a swift-refcounted box or class.
      require(isSwiftRefcounted(PAI->getArguments().front()->getType()),
              "partial_apply context argument must be swift-refcounted");
    }
  }

  void checkBuiltinInst(BuiltinInst *BI) {
    // Check for special constraints on llvm intrinsics.
    if (BI->getIntrinsicInfo().ID != llvm::Intrinsic::not_intrinsic) {
      verifyLLVMIntrinsic(BI, BI->getIntrinsicInfo().ID);
      return;
    }

    auto builtinKind = BI->getBuiltinKind();
    auto arguments = BI->getArguments();

    // Check that 'getCurrentAsyncTask' only occurs within an async function.
    if (builtinKind == BuiltinValueKind::GetCurrentAsyncTask) {
      require(F.isAsync(),
          "getCurrentAsyncTask builtin can only be used in an async function");
      return;
    }

    if (builtinKind == BuiltinValueKind::InitializeDefaultActor ||
        builtinKind == BuiltinValueKind::DestroyDefaultActor) {
      require(arguments.size() == 1,
              "default-actor builtin can only operate on a single object");
      auto argType = arguments[0]->getType().getASTType();
      auto argClass = argType.getClassOrBoundGenericClass();
      require((argClass && argClass->isRootDefaultActor(M,
                                        F.getResilienceExpansion())) ||
              isa<BuiltinNativeObjectType>(argType),
              "default-actor builtin can only operate on default actors");
      return;
    }

    // Check that 'getCurrentAsyncTask' only occurs within an async function.
    if (builtinKind == BuiltinValueKind::GetCurrentExecutor) {
      require(F.isAsync(),
              "getCurrentExecutor can only be used in an async function");
      require(arguments.empty(), "getCurrentExecutor takes no arguments");
      requireOptionalExecutorType(BI, "result of getCurrentExecutor");
      return;
    }

    if (builtinKind == BuiltinValueKind::BuildOrdinarySerialExecutorRef ||
        builtinKind == BuiltinValueKind::BuildComplexEqualitySerialExecutorRef ||
        builtinKind == BuiltinValueKind::BuildDefaultActorExecutorRef) {
      require(arguments.size() == 1,
              "builtin expects one argument");
      require(arguments[0]->getType().isObject(),
              "operand of builtin should have object type");
      requireObjectType(BuiltinExecutorType, BI,
                        "result of build*ExecutorRef");
      return;
    }

    if (builtinKind == BuiltinValueKind::BuildMainActorExecutorRef) {
      require(arguments.size() == 0,
              "buildMainActorExecutorRef expects no arguments");
      requireObjectType(BuiltinExecutorType, BI,
                        "result of build*ExecutorRef");
      return;
    }

    if (builtinKind == BuiltinValueKind::Copy) {
      // We expect that this builtin will be specialized during transparent
      // inlining into explicit_copy_value if we inline into a non-generic
      // context. If the builtin still remains and is not in the specific copy
      // semantic function (which is the only function marked with
      // semantics::LIFETIMEMANAGEMENT_COPY), then we know that we did
      // transparent inlining into a function that did not result in the Builtin
      // being specialized out which is user error.
      //
      // NOTE: Once we have opaque values, this restriction will go away. This
      // is just so we can call Builtin.copy outside of the stdlib.
      auto semanticName = semantics::LIFETIMEMANAGEMENT_COPY;
      require(BI->getFunction()->hasSemanticsAttr(semanticName),
              "_copy used within a generic context");
    }
  }
  
  void checkFunctionRefBaseInst(FunctionRefBaseInst *FRI) {
    auto fnType = requireObjectType(SILFunctionType, FRI,
                                    "result of function_ref");
    require(!fnType->getExtInfo().hasContext(),
            "function_ref should have a context-free function result");

    // Note: in SingleFunction mode, we relax some of these checks because
    // we may not have linked everything yet.

    SILFunction *RefF = FRI->getInitiallyReferencedFunction();

    if (isa<FunctionRefInst>(FRI))
      require(
          !RefF->isDynamicallyReplaceable(),
          "function_ref cannot reference a [dynamically_replaceable] function");
    else if (isa<PreviousDynamicFunctionRefInst>(FRI)) {
      require(!RefF->isDynamicallyReplaceable(),
              "previous_function_ref cannot reference a "
              "[dynamically_replaceable] function");
      require(RefF->getDynamicallyReplacedFunction(),
              "previous_function_ref must reference a "
              "[dynamic_replacement_for:...] function");
    } else if (isa<DynamicFunctionRefInst>(FRI))
      require(RefF->isDynamicallyReplaceable(),
              "dynamic_function_ref must reference a "
              "[dynamically_replaceable] function");

    // In canonical SIL, direct reference to a shared_external declaration
    // is an error; we should have deserialized a body. In raw SIL, including
    // the merge-modules phase, we may not have deserialized the body yet as we
    // may not have run the SILLinker pass.
    if (F.getModule().getStage() >= SILStage::Canonical) {
      if (RefF->isExternalDeclaration()) {
        require(SingleFunction ||
                !hasSharedVisibility(RefF->getLinkage()) ||
                RefF->hasForeignBody(),
                "external declarations of SILFunctions with shared visibility is "
                "not allowed");
      }
    }

    // A direct reference to a non-public or shared but not fragile function
    // from a fragile function is an error.
    if (F.isSerialized()) {
      require((SingleFunction && RefF->isExternalDeclaration()) ||
              RefF->hasValidLinkageForFragileRef(),
              "function_ref inside fragile function cannot "
              "reference a private or hidden symbol");
    }

    verifySILFunctionType(fnType);
  }

  void checkFunctionRefInst(FunctionRefInst *FRI) {
    checkFunctionRefBaseInst(FRI);
  }

  void checkDynamicFunctionRefInst(DynamicFunctionRefInst *FRI) {
    checkFunctionRefBaseInst(FRI);
  }

  void checkPreviousDynamicFunctionRefInst(PreviousDynamicFunctionRefInst *FRI) {
    checkFunctionRefBaseInst(FRI);
  }

  void checkAllocGlobalInst(AllocGlobalInst *AGI) {
    SILGlobalVariable *RefG = AGI->getReferencedGlobal();
    if (auto *VD = RefG->getDecl()) {
      require(!checkResilience(VD, F.getModule().getSwiftModule(),
                               F.getResilienceExpansion()),
              "cannot access storage of resilient global");
    }
    if (F.isSerialized()) {
      require(RefG->isSerialized()
                || hasPublicVisibility(RefG->getLinkage()),
              "alloc_global inside fragile function cannot "
              "reference a private or hidden symbol");
    }
  }

  void checkGlobalAccessInst(GlobalAccessInst *GAI) {
    SILGlobalVariable *RefG = GAI->getReferencedGlobal();
    requireSameType(
        GAI->getType().getObjectType(),
        RefG->getLoweredTypeInContext(F.getTypeExpansionContext()),
        "global_addr/value must be the type of the variable it references");
    if (auto *VD = RefG->getDecl()) {
      require(!checkResilience(VD, F.getModule().getSwiftModule(),
                               F.getResilienceExpansion()),
              "cannot access storage of resilient global");
    }
    if (F.isSerialized()) {
      require(RefG->isSerialized()
              || hasPublicVisibility(RefG->getLinkage()),
              "global_addr/value inside fragile function cannot "
              "reference a private or hidden symbol");
    }
  }

  void checkGlobalAddrInst(GlobalAddrInst *GAI) {
    require(GAI->getType().isAddress(),
            "global_addr must have an address result type");
    require(!GAI->getReferencedGlobal()->isInitializedObject(),
            "global_addr cannot refer to a statically initialized object");
    checkGlobalAccessInst(GAI);
    checkAddressWalkerCanVisitAllTransitiveUses(GAI);
  }

  void checkGlobalValueInst(GlobalValueInst *GVI) {
    require(GVI->getType().isObject(),
            "global_value must have an address result type");
    checkGlobalAccessInst(GVI);
  }

  void checkObjectInst(ObjectInst *) {
    require(false, "object instruction is only allowed in a static initializer");
  }

  void checkIntegerLiteralInst(IntegerLiteralInst *ILI) {
    require(ILI->getType().is<AnyBuiltinIntegerType>(),
            "invalid integer literal type");
  }

  void checkLoadInst(LoadInst *LI) {
    require(LI->getType().isObject(), "Result of load must be an object");
    require(!fnConv.useLoweredAddresses()
                || LI->getType().isLoadable(*LI->getFunction()),
            "Load must have a loadable type");
    require(LI->getOperand()->getType().isAddress(),
            "Load operand must be an address");
    requireSameType(LI->getOperand()->getType().getObjectType(), LI->getType(),
                    "Load operand type and result type mismatch");

    // Ownership semantic checks.
    switch (LI->getOwnershipQualifier()) {
    case LoadOwnershipQualifier::Unqualified:
      // We should not see loads with unqualified ownership when SILOwnership is
      // enabled.
      require(!F.hasOwnership(),
              "Load with unqualified ownership in a qualified function");
      break;
    case LoadOwnershipQualifier::Copy:
      require(LI->getModule().getStage() == SILStage::Raw ||
                  !LI->getOperand()->getType().isMoveOnly(),
              "'MoveOnly' types can only be copied in Raw SIL?!");
      [[fallthrough]];
    case LoadOwnershipQualifier::Take:
      require(F.hasOwnership(),
              "Load with qualified ownership in an unqualified function");
      // TODO: Could probably make this a bit stricter.
      require(!LI->getType().isTrivial(*LI->getFunction()),
              "load [copy] or load [take] can only be applied to non-trivial "
              "types");
      break;
    case LoadOwnershipQualifier::Trivial:
      require(F.hasOwnership(),
              "Load with qualified ownership in an unqualified function");
      require(LI->getType().isTrivial(*LI->getFunction()),
              "A load with trivial ownership must load a trivial type");
      break;
    }
  }

  void checkLoadBorrowInst(LoadBorrowInst *LBI) {
    require(
        F.hasOwnership(),
        "Inst with qualified ownership in a function that is not qualified");
    require(LBI->getType().isObject(), "Result of load must be an object");
    require(!fnConv.useLoweredAddresses()
            || LBI->getType().isLoadable(*LBI->getFunction()),
            "Load must have a loadable type");
    require(LBI->getOperand()->getType().isAddress(),
            "Load operand must be an address");
    requireSameType(LBI->getOperand()->getType().getObjectType(),
                    LBI->getType(),
                    "Load operand type and result type mismatch");
    require(loadBorrowImmutabilityAnalysis.isImmutable(LBI),
            "Found load borrow that is invalidated by a local write?!");
  }

  void checkBeginBorrowInst(BeginBorrowInst *bbi) {
    if (!bbi->isLexical())
      return;
    // Lexical begin_borrows of instances of some SILBoxType must derive from
    // alloc_boxes or captures.
    auto value = bbi->getOperand();
    if (!value->getType().is<SILBoxType>())
      return;
    while (true) {
      // Inlining may introduce additional begin_borrow instructions.
      if (auto bbi = dyn_cast<BeginBorrowInst>(value))
        value = bbi->getOperand();
      // SILGen introduces copy_value instructions.
      else if (auto cvi = dyn_cast<CopyValueInst>(value))
        value = cvi->getOperand();
      // SILGen inserts mark_uninitialized instructions of alloc_boxes.
      else if (auto *mui = dyn_cast<MarkUninitializedInst>(value))
        value = mui->getOperand();
      else
        break;
    }
    require(isa<AllocBoxInst>(value) || isa<SILFunctionArgument>(value),
            "Lexical borrows of SILBoxTypes must be of vars or captures.");
  }

  void checkEndBorrowInst(EndBorrowInst *EBI) {
    require(
        F.hasOwnership(),
        "Inst with qualified ownership in a function that is not qualified");
    if (EBI->getOperand()->getType().isAddress()) {
      require(isa<StoreBorrowInst>(EBI->getOperand()),
              "end_borrow of an address not produced by store_borrow");
    }
  }

  void checkEndLifetimeInst(EndLifetimeInst *I) {
    require(!I->getOperand()->getType().isTrivial(*I->getFunction()),
            "Source value should be non-trivial");
    require(!fnConv.useLoweredAddresses() || F.hasOwnership(),
            "end_lifetime is only valid in functions with qualified "
            "ownership");
  }

  void checkUncheckedValueCastInst(UncheckedValueCastInst *) {
    require(
        F.hasOwnership(),
        "Inst with qualified ownership in a function that is not qualified");
  }

  void checkUncheckedOwnershipConversionInst(
    UncheckedOwnershipConversionInst *uoci) {
    require(
        F.hasOwnership(),
        "Inst with qualified ownership in a function that is not qualified");
    require(!uoci->getType().isAddress(),
            "cannot convert ownership of an address");
    require(uoci->getType() == uoci->getOperand()->getType(),
            "converting ownership does not affect the type");
  }

  template <class AI>
  void checkAccessEnforcement(AI *AccessInst) {
    if (AccessInst->getModule().getStage() != SILStage::Raw) {
      require(AccessInst->getEnforcement() != SILAccessEnforcement::Unknown,
              "access must have known enforcement outside raw stage");
    }
  }

  bool checkScopedAddressUses(ScopedAddressValue scopedAddress,
                              SSAPrunedLiveness *scopedAddressLiveness,
                              DeadEndBlocks *deadEndBlocks) {
    SmallVector<Operand *, 4> uses;
    findTransitiveUsesForAddress(scopedAddress.value, &uses);

    // Check if the collected uses are well-scoped.
    for (auto *use : uses) {
      auto *user = use->getUser();
      if (deadEndBlocks->isDeadEnd(user->getParent())) {
        continue;
      }
      if (scopedAddress.isScopeEndingUse(use)) {
        continue;
      }
      if (!scopedAddressLiveness->isWithinBoundary(user)) {
        llvm::errs() << "User found outside scope: " << *user;
        return false;
      }
    }
    return true;
  }

  void checkBeginAccessInst(BeginAccessInst *BAI) {
    requireSameType(BAI->getType(), BAI->getSource()->getType(),
                    "result must be same type as operand");
    require(BAI->getType().isAddress(),
            "begin_access operand must have address type");

    checkAccessEnforcement(BAI);

    switch (BAI->getAccessKind()) {
    case SILAccessKind::Init:
      // A signed access preserves the access marker until IRGen
      require(BAI->getEnforcement() == SILAccessEnforcement::Static ||
                  BAI->getEnforcement() == SILAccessEnforcement::Signed,
              "init accesses cannot use non-static/non-signed enforcement");
      break;

    case SILAccessKind::Deinit:
      // A signed access preserves the access marker until IRGen.
      //
      // NOTE: We allow for deinit to be non-static before Lowered SIL to allow
      // for it to be used to represent deinit accesses for move only types
      // stored in classes, globals, and escaping mutable captures. This is ok
      // to do since even though we allow for them to be represented there, the
      // move only checker passes will always emit an error for them implying
      // that we will never get to LoweredSIL and codegen.
      require(BAI->getEnforcement() == SILAccessEnforcement::Static ||
                  BAI->getEnforcement() == SILAccessEnforcement::Signed ||
                  BAI->getModule().getStage() != SILStage::Lowered,
              "init accesses cannot use non-static/non-signed enforcement");
      break;
    case SILAccessKind::Read:
    case SILAccessKind::Modify:
      break;
    }

    // Verify that all formal accesses patterns are recognized as part of a
    // allowlist. The presence of an unknown pattern means that analysis will
    // silently fail, and the compiler may be introducing undefined behavior
    // with no other way to detect it.
    //
    // For example, AccessEnforcementWMO runs very late in the
    // pipeline and assumes valid storage for all dynamic Read/Modify access. It
    // also requires that Unidentified access fit a allowlist on known
    // non-internal globals or class properties.
    //
    // First check that identifyFormalAccess returns without asserting. For
    // Unsafe enforcement, that is sufficient. For any other enforcement
    // level also require that it returns a valid AccessStorage object.
    // Unsafe enforcement is used for some unrecognizable access patterns,
    // like debugger variables. The compiler never cares about the source of
    // those accesses.
    identifyFormalAccess(BAI);
    // FIXME: rdar://57291811 - the following check for valid storage will be
    // reenabled shortly. A fix is planned. In the meantime, the possibility that
    // a real miscompilation could be caused by this failure is insignificant.
    // I will probably enable a much broader SILVerification of address-type
    // block arguments first to ensure we never hit this check again.
    /*
    AccessStorage storage = identifyFormalAccess(BAI);
    if (BAI->getEnforcement() != SILAccessEnforcement::Unsafe)
      require(storage, "Unknown formal access pattern");
    */
  }

  void checkEndAccessInst(EndAccessInst *EAI) {
    auto BAI = dyn_cast<BeginAccessInst>(EAI->getOperand());
    require(BAI != nullptr,
            "operand of end_access must be a begin_access");

    if (EAI->isAborting()) {
      require(BAI->getAccessKind() == SILAccessKind::Init ||
              BAI->getAccessKind() == SILAccessKind::Deinit,
              "aborting access must apply to init or deinit");
    }
  }

  void checkBeginUnpairedAccessInst(BeginUnpairedAccessInst *BUAI) {
    require(BUAI->getEnforcement() != SILAccessEnforcement::Unknown,
            "unpaired access can never use unknown enforcement");
    require(BUAI->getSource()->getType().isAddress(),
            "address operand must have address type");
    requireAddressType(BuiltinUnsafeValueBufferType, BUAI->getBuffer(),
                       "scratch buffer operand");

    checkAccessEnforcement(BUAI);

    switch (BUAI->getAccessKind()) {
    case SILAccessKind::Init:
      require(BUAI->getEnforcement() == SILAccessEnforcement::Static,
              "init accesses cannot use non-static enforcement");
      break;
    case SILAccessKind::Deinit:
      // NOTE: We allow for deinit to be non-static before Lowered SIL to allow
      // for it to be used to represent deinit accesses for move only types
      // stored in classes, globals, and escaping mutable captures. This is ok
      // to do since even though we allow for them to be represented there, the
      // move only checker passes will always emit an error for them implying
      // that we will never get to LoweredSIL and codegen.
      require(
          BUAI->getEnforcement() == SILAccessEnforcement::Static ||
              BUAI->getModule().getStage() != SILStage::Lowered,
          "deinit accesses cannot use non-static enforcement in Lowered SIL");
      break;

    case SILAccessKind::Read:
    case SILAccessKind::Modify:
      break;
    }

    // First check that identifyFormalAccess never asserts.
    AccessStorage storage = identifyFormalAccess(BUAI);
    // Only allow Unsafe and Builtin access to have invalid storage.
    if (BUAI->getEnforcement() != SILAccessEnforcement::Unsafe
        && !BUAI->isFromBuiltin()) {
      require(storage, "Unknown formal access pattern");
    }
  }

  void checkEndUnpairedAccessInst(EndUnpairedAccessInst *I) {
    require(I->getEnforcement() != SILAccessEnforcement::Unknown,
            "unpaired access can never use unknown enforcement");
    requireAddressType(BuiltinUnsafeValueBufferType, I->getBuffer(),
                       "scratch buffer operand");

    checkAccessEnforcement(I);
  }

  void checkStoreInst(StoreInst *SI) {
    require(SI->getSrc()->getType().isObject(),
            "Can't store from an address source");
    require(!fnConv.useLoweredAddresses()
                || SI->getSrc()->getType().isLoadable(*SI->getFunction()),
            "Can't store a non loadable type");
    require(SI->getDest()->getType().isAddress(),
            "Must store to an address dest");
    requireSameType(SI->getDest()->getType().getObjectType(),
                    SI->getSrc()->getType(),
                    "Store operand type and dest type mismatch");

    // Perform ownership checks.
    switch (SI->getOwnershipQualifier()) {
    case StoreOwnershipQualifier::Unqualified:
      // We should not see loads with unqualified ownership when SILOwnership is
      // enabled.
      require(!F.hasOwnership(),
              "Qualified store in function with unqualified ownership?!");
      break;
    case StoreOwnershipQualifier::Init:
    case StoreOwnershipQualifier::Assign:
      require(
          F.hasOwnership(),
          "Inst with qualified ownership in a function that is not qualified");
      // TODO: Could probably make this a bit stricter.
      require(!SI->getSrc()->getType().isTrivial(*SI->getFunction()),
              "store [init] or store [assign] can only be applied to "
              "non-trivial types");
      break;
    case StoreOwnershipQualifier::Trivial: {
      require(
          F.hasOwnership(),
          "Inst with qualified ownership in a function that is not qualified");
      SILValue Src = SI->getSrc();
      require(Src->getType().isTrivial(*SI->getFunction()) ||
                  Src->getOwnershipKind() == OwnershipKind::None,
              "A store with trivial ownership must store a type with trivial "
              "ownership");
      break;
    }
    }
  }

  void checkStoreBorrowInst(StoreBorrowInst *SI) {
    // A store_borrow must be to an alloc_stack.  That alloc_stack can only be
    // used by store_borrows (and dealloc_stacks).
    require(SI->getSrc()->getType().isObject(),
            "Can't store from an address source");
    require(!fnConv.useLoweredAddresses()
                || SI->getSrc()->getType().isLoadable(*SI->getFunction()),
            "Can't store a non loadable type");
    require(SI->getDest()->getType().isAddress(),
            "Must store to an address dest");
    // Note: This is the current implementation and the design is not final.
    require(isa<AllocStackInst>(SI->getDest()),
            "store_borrow destination can only be an alloc_stack");
    requireSameType(SI->getDest()->getType().getObjectType(),
                    SI->getSrc()->getType(),
                    "Store operand type and dest type mismatch");

    SSAPrunedLiveness scopedAddressLiveness(SI->getFunction());
    ScopedAddressValue scopedAddress(SI);
    // FIXME: Reenable @test_load_borrow_store_borrow_nested in
    // store_borrow_verify_errors once computeLivess can successfully handle a
    // store_borrow within a load_borrow. This can be fixed in two ways
    //
    // (1) With complete lifetimes, this no longer needs to perform transitive
    // liveness at all.
    //
    // (2) findInnerTransitiveGuaranteedUses, which ends up being called on the
    // load_borrow to compute liveness, can be taught to transitively process
    // InteriorPointer uses instead of returning PointerEscape. We need to make
    // sure all uses of the utility need to handle this first.
    AddressUseKind useKind =
        scopedAddress.computeTransitiveLiveness(scopedAddressLiveness);
    bool success = useKind == AddressUseKind::NonEscaping;

    require(!success || checkScopedAddressUses(
              scopedAddress, &scopedAddressLiveness, DEBlocks.get()),
            "Ill formed store_borrow scope");

    require(!success || !hasOtherStoreBorrowsInLifetime(
              SI, &scopedAddressLiveness, DEBlocks.get()),
            "A store_borrow cannot be nested within another "
            "store_borrow to its destination");

    for (auto *use : SI->getDest()->getUses()) {
      auto *user = use->getUser();
      require(
          user == SI || isa<StoreBorrowInst>(user) ||
              isa<DeallocStackInst>(user),
          "A store_borrow destination can be used only via its return address");
    }
  }

  void checkAssignInst(AssignInst *AI) {
    SILValue Src = AI->getSrc(), Dest = AI->getDest();
    require(AI->getModule().getStage() == SILStage::Raw,
            "assign instruction can only exist in raw SIL");
    require(Src->getType().isObject(), "Can't assign from an address source");
    require(Dest->getType().isAddress(), "Must store to an address dest");
    requireSameType(Dest->getType().getObjectType(), Src->getType(),
                    "Store operand type and dest type mismatch");
  }

  // Usually the assign_by_wrapper initializer or setter has a single argument
  // (the value). But in case of a tuple, the initializer/setter expect the
  // tuple elements as separate arguments.
  void checkAssignByWrapperArgsRecursively(SILType ty,
                               SILFunctionConventions &conv, unsigned &argIdx) {
    if (auto tupleTy = ty.getAs<TupleType>()) {
      for (Type et : tupleTy->getElementTypes()) {
        SILType elTy = SILType::getPrimitiveType(CanType(et), ty.getCategory());
        checkAssignByWrapperArgsRecursively(elTy, conv, argIdx);
      }
      return;
    }
    require(argIdx < conv.getNumSILArguments(),
            "initializer or setter has too few arguments");
    SILType argTy =
        conv.getSILArgumentType(argIdx++, F.getTypeExpansionContext());
    if (ty.isAddress() && argTy.isObject())
      ty = ty.getObjectType();
    requireSameType(ty, argTy, "wrong argument type of initializer or setter");
  }

  void checkAssignByWrapperArgs(SILType ty, SILFunctionConventions &conv) {
    unsigned argIdx = conv.getSILArgIndexOfFirstParam();
    checkAssignByWrapperArgsRecursively(ty, conv, argIdx);
    require(argIdx == conv.getNumSILArguments(),
            "initializer or setter has too many arguments");
  }

  void checkAssignByWrapperInst(AssignByWrapperInst *AI) {
    SILValue Src = AI->getSrc(), Dest = AI->getDest();
    require(AI->getModule().getStage() == SILStage::Raw,
            "assign instruction can only exist in raw SIL");
    require(Dest->getType().isAddress(), "Must store to an address dest");

    SILValue initFn = AI->getInitializer();
    CanSILFunctionType initTy = initFn->getType().castTo<SILFunctionType>();
    SILFunctionConventions initConv(initTy, AI->getModule());
    checkAssignByWrapperArgs(Src->getType(), initConv);
    switch (initConv.getNumIndirectSILResults()) {
      case 0:
        require(initConv.getNumDirectSILResults() == 1,
                "wrong number of init function results");
        requireSameType(
            Dest->getType().getObjectType(),
            *initConv.getDirectSILResultTypes(F.getTypeExpansionContext())
                 .begin(),
            "wrong init function result type");
        break;
      case 1:
        require(initConv.getNumDirectSILResults() == 0,
                "wrong number of init function results");
        requireSameType(
            Dest->getType(),
            *initConv.getIndirectSILResultTypes(F.getTypeExpansionContext())
                 .begin(),
            "wrong indirect init function result type");
        break;
      default:
        require(false, "wrong number of indirect init function results");
    }

    SILValue setterFn = AI->getSetter();
    CanSILFunctionType setterTy = setterFn->getType().castTo<SILFunctionType>();
    SILFunctionConventions setterConv(setterTy, AI->getModule());
    require(setterConv.getNumIndirectSILResults() == 0,
            "set function has indirect results");
    checkAssignByWrapperArgs(Src->getType(), setterConv);
  }

  void checkAssigOrInitInstAccessorArgs(SILType argTy,
                                        SILFunctionConventions &conv) {
    unsigned argIdx = conv.getSILArgIndexOfFirstParam();
    checkAssignByWrapperArgsRecursively(argTy, conv, argIdx);
  }

  void checkAssignOrInitInst(AssignOrInitInst *AI) {
    SILValue Src = AI->getSrc();
    require(AI->getModule().getStage() == SILStage::Raw,
            "assign_or_init can only exist in raw SIL");

    SILValue initFn = AI->getInitializer();
    SILValue setterFn = AI->getSetter();

    // Check init - it's an unapplied reference that takes property addresses
    // and `initialValue`.
    {
      CanSILFunctionType initTy = initFn->getType().castTo<SILFunctionType>();
      SILFunctionConventions initConv(initTy, AI->getModule());

      require(initConv.getNumIndirectSILResults() ==
                  AI->getInitializedProperties().size(),
              "init function has invalid number of indirect results");
      checkAssigOrInitInstAccessorArgs(Src->getType(), initConv);
    }

    // Check setter - it's a partially applied reference which takes
    // `initialValue`.
    CanSILFunctionType setterTy = setterFn->getType().castTo<SILFunctionType>();
    SILFunctionConventions setterConv(setterTy, AI->getModule());
    require(setterConv.getNumIndirectSILResults() == 0,
            "set function has indirect results");
    checkAssignByWrapperArgs(Src->getType(), setterConv);
  };

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...)                    \
  void checkLoad##Name##Inst(Load##Name##Inst *LWI) {                          \
    require(LWI->getType().isObject(), "Result of load must be an object");    \
    auto isOptional = bool(LWI->getType().getOptionalObjectType());            \
    auto optionality = optionalityOf(ReferenceOwnership::Name);                \
    if (optionality == ReferenceOwnershipOptionality::Required)                \
      require(isOptional, "Optionality mismatch");                             \
    if (optionality == ReferenceOwnershipOptionality::Disallowed)              \
      require(!isOptional, "Optionality mismatch");                            \
    auto PointerType = LWI->getOperand()->getType();                           \
    auto PointerRVType = PointerType.getASTType();                             \
    require(PointerType.isAddress() && PointerRVType->is<Name##StorageType>(), \
            "load_" #name " operand must be a " #name " address");             \
    requireSameType(                                                           \
        PointerRVType->getReferenceStorageReferent()->getCanonicalType(),      \
        LWI->getType().getASTType(),                                           \
        "Load operand type and result type mismatch");                         \
  }                                                                            \
  void checkStore##Name##Inst(Store##Name##Inst *SWI) {                        \
    auto SrcTy = SWI->getSrc()->getType();                                     \
    require(SrcTy.isObject(), "Can't store from an address source");           \
    auto isOptional = bool(SrcTy.getOptionalObjectType());                     \
    auto optionality = optionalityOf(ReferenceOwnership::Name);                \
    if (optionality == ReferenceOwnershipOptionality::Required)                \
      require(isOptional, "Optionality mismatch");                             \
    if (optionality == ReferenceOwnershipOptionality::Disallowed)              \
      require(!isOptional, "Optionality mismatch");                            \
    auto PointerType = SWI->getDest()->getType();                              \
    auto PointerRVType = PointerType.getASTType();                             \
    require(PointerType.isAddress() && PointerRVType->is<Name##StorageType>(), \
            "store_" #name " address operand must be a " #name " address");    \
    requireSameType(                                                           \
        PointerRVType->getReferenceStorageReferent()->getCanonicalType(),      \
        SrcTy.getASTType(), "Store operand type and dest type mismatch");      \
  }
#define LOADABLE_REF_STORAGE_HELPER(Name, name)                                \
  void checkRefTo##Name##Inst(RefTo##Name##Inst *I) {                          \
    requireReferenceStorageCapableValue(I->getOperand(),                       \
                                        "Operand of ref_to_" #name);           \
    auto operandType = I->getOperand()->getType().getASTType();                \
    auto resultType =                                                          \
        requireObjectType(Name##StorageType, I, "Result of ref_to_" #name);    \
    requireSameType(resultType.getReferentType(), operandType,                 \
                    "Result of ref_to_" #name " does not have the "            \
                    "operand's type as its referent type");                    \
  }                                                                            \
  void check##Name##ToRefInst(Name##ToRefInst *I) {                            \
    auto operandType = requireObjectType(Name##StorageType, I->getOperand(),   \
                                         "Operand of " #name "_to_ref");       \
    requireReferenceStorageCapableValue(I, "Result of " #name "_to_ref");      \
    auto resultType = I->getType().getASTType();                               \
    requireSameType(operandType.getReferentType(), resultType,                 \
                    "Operand of " #name "_to_ref does not have the "           \
                    "operand's type as its referent type");                    \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, closure)        \
  void checkStrongRetain##Name##Inst(StrongRetain##Name##Inst *RI) {           \
    auto ty = requireObjectType(Name##StorageType, RI->getOperand(),           \
                                "Operand of strong_retain_" #name);            \
    closure();                                                                 \
    (void)ty;                                                                  \
    require(!F.hasOwnership(), "strong_retain_" #name " is only in "           \
                               "functions with unqualified "                   \
                               "ownership");                                   \
  }                                                                            \
  void check##Name##RetainInst(Name##RetainInst *RI) {                         \
    auto ty = requireObjectType(Name##StorageType, RI->getOperand(),           \
                                "Operand of " #name "_retain");                \
    closure();                                                                 \
    (void)ty;                                                                  \
    require(!F.hasOwnership(),                                                 \
            #name "_retain is only in functions with unqualified ownership");  \
  }                                                                            \
  void check##Name##ReleaseInst(Name##ReleaseInst *RI) {                       \
    auto ty = requireObjectType(Name##StorageType, RI->getOperand(),           \
                                "Operand of " #name "_release");               \
    closure();                                                                 \
    (void)ty;                                                                  \
    require(!F.hasOwnership(),                                                 \
            #name "_release is only in functions with unqualified ownership"); \
  }                                                                            \
  void StrongcheckCopy##Name##ValueInst(StrongCopy##Name##ValueInst *I) {      \
    auto ty = requireObjectType(Name##StorageType, I->getOperand(),            \
                                "Operand of " #name "_retain");                \
    closure();                                                                 \
    (void)ty;                                                                  \
    /* *NOTE* We allow copy_##name##_value to be used throughout the entire */ \
    /* pipeline even though it is a higher level instruction. */               \
  }

#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  LOADABLE_REF_STORAGE_HELPER(Name, name) \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, []{})
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  LOADABLE_REF_STORAGE_HELPER(Name, name) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, [&]{ \
    require(ty->isLoadable(ResilienceExpansion::Maximal), \
            "'" #name "' type must be loadable"); \
  })
#define UNCHECKED_REF_STORAGE(Name, name, ...)                                 \
  LOADABLE_REF_STORAGE_HELPER(Name, name)                                      \
  void checkStrongCopy##Name##ValueInst(StrongCopy##Name##ValueInst *I) {      \
    auto ty = requireObjectType(Name##StorageType, I->getOperand(),            \
                                "Operand of " #name "_retain");                \
    (void)ty;                                                                  \
    /* *NOTE* We allow copy_##name##_value to be used throughout the entire */ \
    /* pipeline even though it is a higher level instruction. */               \
  }

#include "swift/AST/ReferenceStorage.def"
#undef LOADABLE_REF_STORAGE_HELPER
#undef ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER

  void checkMarkUninitializedInst(MarkUninitializedInst *MU) {
    SILValue Src = MU->getOperand();
    require(MU->getModule().getStage() == SILStage::Raw,
            "mark_uninitialized instruction can only exist in raw SIL");
    require(Src->getType().isAddress() ||
            Src->getType().getClassOrBoundGenericClass() ||
            Src->getType().getAs<SILBoxType>(),
            "mark_uninitialized must be an address, class, or box type");
    requireSameType(Src->getType(), MU->getType(),
                    "operand and result type mismatch");
    // FIXME: When the work to force MUI to be on Allocations/SILArguments
    // complete, turn on this assertion.
    require(isa<AllocationInst>(Src)
            || isa<GlobalAddrInst>(Src)
            // TODO: Should we support SILUndef on mark_uninitialized? We
            // currently have a test that verifies this behavior, but it seems
            // like this would always be a bug due to the implications around
            // the code in DI. This just bakes in the current behavior.
            || isa<SILUndef>(Src)
            // We allow SILArguments to support the case of initializing
            // initializers. In such a case, the underlying case is allocated
            // outside by the allocating initializer and we pass in the to be
            // initialized value as a SILArgument.
            || isa<SILArgument>(Src)
            // FIXME: We only support pointer to address here to not break LLDB. It is
            // important that long term we get rid of this since this is a situation
            // where LLDB is breaking SILGen/DI invariants by not creating a new
            // independent stack location for the pointer to address.
            || isa<PointerToAddressInst>(Src),
            "Mark Uninitialized must be applied to a storage location");
  }

  void checkMarkFunctionEscapeInst(MarkFunctionEscapeInst *MFE) {
    require(MFE->getModule().getStage() == SILStage::Raw,
            "mark_function_escape instruction can only exist in raw SIL");
    for (auto Elt : MFE->getElements())
      require(Elt->getType().isAddress(), "MFE must refer to variable addrs");
  }

  void checkCopyAddrInst(CopyAddrInst *cai) {
    require(cai->getSrc()->getType().isAddress(), "Src value should be lvalue");
    require(cai->getDest()->getType().isAddress(),
            "Dest address should be lvalue");
    requireSameType(cai->getDest()->getType(), cai->getSrc()->getType(),
                    "Store operand type and dest type mismatch");
    require(F.isTypeABIAccessible(cai->getDest()->getType()),
            "cannot directly copy type with inaccessible ABI");
    require(cai->getModule().getStage() == SILStage::Raw ||
                (cai->isTakeOfSrc() || !cai->getSrc()->getType().isMoveOnly()),
            "'MoveOnly' types can only be copied in Raw SIL?!");
  }

  void checkExplicitCopyAddrInst(ExplicitCopyAddrInst *ecai) {
    require(F.hasOwnership(), "explicit_copy_* is only valid in OSSA.");
    require(ecai->getSrc()->getType().isAddress(),
            "Src value should be lvalue");
    require(ecai->getDest()->getType().isAddress(),
            "Dest address should be lvalue");
    requireSameType(ecai->getDest()->getType(), ecai->getSrc()->getType(),
                    "Store operand type and dest type mismatch");
    require(F.isTypeABIAccessible(ecai->getDest()->getType()),
            "cannot directly copy type with inaccessible ABI");
  }

  void checkMarkUnresolvedMoveAddrInst(MarkUnresolvedMoveAddrInst *SI) {
    require(F.hasOwnership(), "Only valid in OSSA.");
    require(F.getModule().getStage() == SILStage::Raw, "Only valid in Raw SIL");
    require(SI->getSrc()->getType().isAddress(), "Src value should be lvalue");
    require(SI->getDest()->getType().isAddress(),
            "Dest address should be lvalue");
    requireSameType(SI->getDest()->getType(), SI->getSrc()->getType(),
                    "Store operand type and dest type mismatch");
    require(F.isTypeABIAccessible(SI->getDest()->getType()),
            "cannot directly copy type with inaccessible ABI");
  }

  void checkRetainValueInst(RetainValueInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(!F.hasOwnership(),
            "retain_value is only in functions with unqualified ownership");
  }

  void checkRetainValueAddrInst(RetainValueAddrInst *I) {
    require(I->getOperand()->getType().isAddress(),
            "Source value should be an address value");
    require(!F.hasOwnership(),
            "retain_value is only in functions with unqualified ownership");
  }

  void checkCopyValueInst(CopyValueInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(!I->getOperand()->getType().isTrivial(*I->getFunction()),
            "Source value should be non-trivial");
    require(!fnConv.useLoweredAddresses() || F.hasOwnership(),
            "copy_value is only valid in functions with qualified "
            "ownership");
    require(I->getModule().getStage() == SILStage::Raw ||
                !I->getOperand()->getType().isMoveOnly(),
            "'MoveOnly' types can only be copied in Raw SIL?!");
  }

  void checkExplicitCopyValueInst(ExplicitCopyValueInst *I) {
    require(F.hasOwnership(), "explicit_copy_* is only valid in OSSA.");
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(!I->getOperand()->getType().isTrivial(*I->getFunction()),
            "Source value should be non-trivial");
  }

  void checkDestroyValueInst(DestroyValueInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(!I->getOperand()->getType().isTrivial(*I->getFunction()),
            "Source value should be non-trivial");
    require(!fnConv.useLoweredAddresses() || F.hasOwnership(),
            "destroy_value is only valid in functions with qualified "
            "ownership");
  }

  void checkReleaseValueInst(ReleaseValueInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(!F.hasOwnership(),
            "release_value is only in functions with unqualified ownership");
  }

  void checkReleaseValueAddrInst(ReleaseValueAddrInst *I) {
    require(I->getOperand()->getType().isAddress(),
            "Source value should be an address value");
    require(!F.hasOwnership(),
            "release_value is only in functions with unqualified ownership");
  }

  void checkAutoreleaseValueInst(AutoreleaseValueInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    // TODO: This instruction could in principle be generalized.
    require(I->getOperand()->getType().hasRetainablePointerRepresentation(),
            "Source value must be a reference type or optional thereof");
  }
  
  void checkSetDeallocatingInst(SetDeallocatingInst *I) {
    require(I->getOperand()->getType().isObject(),
            "Source value should be an object value");
    require(I->getOperand()->getType().hasRetainablePointerRepresentation(),
            "Source value must be a reference type");
  }
  
  void checkCopyBlockInst(CopyBlockInst *I) {
    require(I->getOperand()->getType().isBlockPointerCompatible(),
            "operand of copy_block should be a block");
    requireSameType(I->getOperand()->getType(), I->getType(),
                    "result of copy_block should be same type as operand");
  }
  void checkCopyBlockInst(CopyBlockWithoutEscapingInst *I) {
    require(I->getBlock()->getType().isBlockPointerCompatible(),
            "operand of copy_block should be a block");
    requireSameType(I->getBlock()->getType(), I->getType(),
                    "result of copy_block should be same type as operand");
    auto FnTy = requireObjectType(SILFunctionType, I->getClosure(),
                                  "copy_block_without_escaping operand");
    require(!FnTy->isNoEscape(),
            "closure parameter must not be a @noescape closure");
  }

  void checkProjectBoxInst(ProjectBoxInst *I) {
    require(I->getOperand()->getType().isObject(),
            "project_box operand should be a value");
    auto boxTy = I->getOperand()->getType().getAs<SILBoxType>();
    require(boxTy, "project_box operand should be a @box type");
    requireSameType(I->getType(),
                    getSILBoxFieldType(F.getTypeExpansionContext(), boxTy,
                                       F.getModule().Types, I->getFieldIndex()),
                    "project_box result should be address of boxed type");

    // If we have a mark_uninitialized as a user, the mark_uninitialized must be
    // our only user. This is a requirement that is asserted by allocbox to
    // stack. This check just embeds the requirement into the IR.
    require(I->hasOneUse() ||
            none_of(I->getUses(),
                    [](Operand *Op) -> bool {
                      return isa<MarkUninitializedInst>(Op->getUser());
                    }),
            "project_box with more than one user when a user is a "
            "mark_uninitialized");
    checkAddressWalkerCanVisitAllTransitiveUses(I);
  }

  void checkProjectExistentialBoxInst(ProjectExistentialBoxInst *PEBI) {
    SILType operandType = PEBI->getOperand()->getType();
    require(operandType.isObject(),
            "project_existential_box operand must not be address");

    require(operandType.canUseExistentialRepresentation(
                                              ExistentialRepresentation::Boxed),
            "project_existential_box operand must be boxed existential");

    require(PEBI->getType().isAddress(),
            "project_existential_box result must be an address");

    if (auto *AEBI = dyn_cast<AllocExistentialBoxInst>(PEBI->getOperand())) {
      // The lowered type must be the properly-abstracted form of the AST type.
      SILType exType = AEBI->getExistentialType();
      auto archetype = OpenedArchetypeType::get(exType.getASTType(),
                                                F.getGenericSignature());

      auto loweredTy = F.getLoweredType(Lowering::AbstractionPattern(archetype),
                                        AEBI->getFormalConcreteType())
                                          .getAddressType();

      requireSameType(loweredTy, PEBI->getType(),
              "project_existential_box result should be the lowered "
              "concrete type of its alloc_existential_box");
    }
  }
  
  void checkStructInst(StructInst *SI) {
    auto *structDecl = SI->getType().getStructOrBoundGenericStruct();
    require(structDecl, "StructInst must return a struct");
    require(!structDecl->hasUnreferenceableStorage(),
            "Cannot build a struct with unreferenceable storage from elements "
            "using StructInst");
    require(!checkResilience(structDecl, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient struct");
    require(SI->getType().isObject(),
            "StructInst must produce an object");

    SILType structTy = SI->getType();
    auto opi = SI->getElements().begin(), opEnd = SI->getElements().end();
    for (VarDecl *field : structDecl->getStoredProperties()) {
      require(opi != opEnd,
              "number of struct operands does not match number of stored "
              "member variables of struct");

      SILType loweredType =
          structTy.getFieldType(field, F.getModule(), F.getTypeExpansionContext());
      if (SI->getModule().getStage() != SILStage::Lowered) {
        requireSameType((*opi)->getType(), loweredType,
                        "struct operand type does not match field type");
      }
      ++opi;
    }
  }

  void checkEnumInst(EnumInst *UI) {
    EnumDecl *ud = UI->getType().getEnumOrBoundGenericEnum();
    require(ud, "EnumInst must return an enum");
    require(UI->getElement()->getParentEnum() == ud,
            "EnumInst case must be a case of the result enum type");
    require(UI->getType().isObject(),
            "EnumInst must produce an object");
    require(UI->hasOperand() == UI->getElement()->hasAssociatedValues(),
            "EnumInst must take an argument iff the element does");

    if (UI->getElement()->hasAssociatedValues()) {
      require(UI->getOperand()->getType().isObject(),
              "EnumInst operand must be an object");
      SILType caseTy = UI->getType().getEnumElementType(
          UI->getElement(), F.getModule(), F.getTypeExpansionContext());
      if (UI->getModule().getStage() != SILStage::Lowered) {
        requireSameType(caseTy, UI->getOperand()->getType(),
                        "EnumInst operand type does not match type of case");
      }
    }
  }

  void checkInitEnumDataAddrInst(InitEnumDataAddrInst *UI) {
    EnumDecl *ud = UI->getOperand()->getType().getEnumOrBoundGenericEnum();
    require(ud, "InitEnumDataAddrInst must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "InitEnumDataAddrInst case must be a case of the enum operand type");
    require(UI->getElement()->hasAssociatedValues(),
            "InitEnumDataAddrInst case must have a data type");
    require(UI->getOperand()->getType().isAddress(),
            "InitEnumDataAddrInst must take an address operand");
    require(UI->getType().isAddress(),
            "InitEnumDataAddrInst must produce an address");

    SILType caseTy = UI->getOperand()->getType().getEnumElementType(
        UI->getElement(), F.getModule(), F.getTypeExpansionContext());

    if (UI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          caseTy, UI->getType(),
          "InitEnumDataAddrInst result does not match type of enum case");
    }
  }

  void checkUncheckedEnumDataInst(UncheckedEnumDataInst *UI) {
    EnumDecl *ud = UI->getOperand()->getType().getEnumOrBoundGenericEnum();
    require(ud, "UncheckedEnumData must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "UncheckedEnumData case must be a case of the enum operand type");
    require(UI->getElement()->getArgumentInterfaceType(),
            "UncheckedEnumData case must have a data type");
    require(UI->getOperand()->getType().isObject(),
            "UncheckedEnumData must take an address operand");
    require(UI->getType().isObject(),
            "UncheckedEnumData must produce an address");

    SILType caseTy = UI->getOperand()->getType().getEnumElementType(
        UI->getElement(), F.getModule(), F.getTypeExpansionContext());

    if (UI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          caseTy, UI->getType(),
          "UncheckedEnumData result does not match type of enum case");
    }
  }

  void checkUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *UI) {
    EnumDecl *ud = UI->getOperand()->getType().getEnumOrBoundGenericEnum();
    require(ud, "UncheckedTakeEnumDataAddrInst must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "UncheckedTakeEnumDataAddrInst case must be a case of the enum operand type");
    require(UI->getElement()->getArgumentInterfaceType(),
            "UncheckedTakeEnumDataAddrInst case must have a data type");
    require(UI->getOperand()->getType().isAddress(),
            "UncheckedTakeEnumDataAddrInst must take an address operand");
    require(UI->getType().isAddress(),
            "UncheckedTakeEnumDataAddrInst must produce an address");

    SILType caseTy = UI->getOperand()->getType().getEnumElementType(
        UI->getElement(), F.getModule(), F.getTypeExpansionContext());

    if (UI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(caseTy, UI->getType(),
                      "UncheckedTakeEnumDataAddrInst result "
                      "does not match type of enum case");
    }
  }

  void checkInjectEnumAddrInst(InjectEnumAddrInst *IUAI) {
    require(IUAI->getOperand()->getType().is<EnumType>()
              || IUAI->getOperand()->getType().is<BoundGenericEnumType>(),
            "InjectEnumAddrInst must take an enum operand");
    require(IUAI->getElement()->getParentEnum()
              == IUAI->getOperand()->getType().getEnumOrBoundGenericEnum(),
            "InjectEnumAddrInst case must be a case of the enum operand type");
    require(IUAI->getOperand()->getType().isAddress(),
            "InjectEnumAddrInst must take an address operand");
  }

  void checkTupleInst(TupleInst *TI) {
    CanTupleType ResTy = requireObjectType(TupleType, TI, "Result of tuple");
    require(!ResTy.containsPackExpansionType(),
            "tuple instruction cannot be used with tuples containing "
            "pack expansions");

    require(TI->getElements().size() == ResTy->getNumElements(),
            "Tuple field count mismatch!");

    if (TI->getModule().getStage() != SILStage::Lowered) {
      for (size_t i = 0, size = TI->getElements().size(); i < size; ++i) {
        requireSameType(TI->getElement(i)->getType().getASTType(),
                        ResTy.getElementType(i),
                        "Tuple element arguments do not match tuple type!");
      }
    }
  }

  // Is a SIL type a potential lowering of a formal type?
  bool isLoweringOf(SILType loweredType, CanType formalType) {
    return loweredType.isLoweringOf(F.getTypeExpansionContext(), F.getModule(),
                                    formalType);
  }
  
  void checkMetatypeInst(MetatypeInst *MI) {
    require(MI->getType().is<MetatypeType>(),
            "metatype instruction must be of metatype type");
    auto MetaTy = MI->getType().castTo<MetatypeType>();
    require(MetaTy->hasRepresentation(),
            "metatype instruction must have a metatype representation");
    verifyLocalArchetype(MI, MetaTy.getInstanceType());
  }
  void checkValueMetatypeInst(ValueMetatypeInst *MI) {
    require(MI->getType().is<MetatypeType>(),
            "value_metatype instruction must be of metatype type");
    require(MI->getType().castTo<MetatypeType>()->hasRepresentation(),
            "value_metatype instruction must have a metatype representation");
    auto formalInstanceTy
      = MI->getType().castTo<MetatypeType>().getInstanceType();
    require(isLoweringOf(MI->getOperand()->getType(), formalInstanceTy),
            "value_metatype result must be formal metatype of "
            "lowered operand type");
  }
  void checkExistentialMetatypeInst(ExistentialMetatypeInst *MI) {
    require(MI->getType().is<ExistentialMetatypeType>(),
            "existential_metatype instruction must be of metatype type");
    require(MI->getType().castTo<ExistentialMetatypeType>()->hasRepresentation(),
            "value_metatype instruction must have a metatype representation");
    require(MI->getOperand()->getType().isAnyExistentialType(),
            "existential_metatype operand must be of protocol type");

    // The result of an existential_metatype instruction is an existential
    // metatype with the same constraint type as its existential operand.
    auto formalInstanceTy
      = MI->getType().castTo<ExistentialMetatypeType>().getInstanceType();
    if (formalInstanceTy->isConstraintType()) {
      require(MI->getOperand()->getType().is<ExistentialType>(),
              "existential_metatype operand must be an existential type");
      formalInstanceTy =
          ExistentialType::get(formalInstanceTy)->getCanonicalType();
    }
    require(isLoweringOf(MI->getOperand()->getType(), formalInstanceTy),
            "existential_metatype result must be formal metatype of "
            "lowered operand type");
  }

  void checkStrongRetainInst(StrongRetainInst *RI) {
    requireReferenceValue(RI->getOperand(), "Operand of strong_retain");
    require(!F.hasOwnership(),
            "strong_retain is only in functions with unqualified ownership");
  }
  void checkStrongReleaseInst(StrongReleaseInst *RI) {
    requireReferenceValue(RI->getOperand(), "Operand of release");
    require(!F.hasOwnership(),
            "strong_release is only in functions with unqualified ownership");
  }

  void checkDeallocStackInst(DeallocStackInst *DI) {
    require(isa<SILUndef>(DI->getOperand()) ||
                isa<AllocStackInst>(DI->getOperand()) ||
                (isa<PartialApplyInst>(DI->getOperand()) &&
                 cast<PartialApplyInst>(DI->getOperand())->isOnStack()),
            "Operand of dealloc_stack must be an alloc_stack or partial_apply "
            "[stack]");
  }
  void checkDeallocPackInst(DeallocPackInst *DI) {
    require(isa<SILUndef>(DI->getOperand()) ||
            isa<AllocPackInst>(DI->getOperand()),
            "Operand of dealloc_pack must be an alloc_pack");
  }
  void checkDeallocRefInst(DeallocRefInst *DI) {
    require(DI->getOperand()->getType().isObject(),
            "Operand of dealloc_ref must be object");
    auto *cd = DI->getOperand()->getType().getClassOrBoundGenericClass();
    require(cd, "Operand of dealloc_ref must be of class type");

    require(!checkResilience(cd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot directly deallocate resilient class");
  }
  void checkDeallocPartialRefInst(DeallocPartialRefInst *DPRI) {
    require(DPRI->getInstance()->getType().isObject(),
            "First operand of dealloc_partial_ref must be object");
    auto class1 = DPRI->getInstance()->getType().getClassOrBoundGenericClass();
    require(class1,
            "First operand of dealloc_partial_ref must be of class type");
    require(DPRI->getMetatype()->getType().is<MetatypeType>(),
            "Second operand of dealloc_partial_ref must be a metatype");
    auto class2 = DPRI->getMetatype()->getType().castTo<MetatypeType>()
        ->getInstanceType()->getClassOrBoundGenericClass();
    require(class2,
            "Second operand of dealloc_partial_ref must be a class metatype");
    require(class2->isSuperclassOf(class1),
            "First operand not superclass of second instance type");
  }

  void checkAllocBoxInst(AllocBoxInst *AI) {
    auto boxTy = AI->getType().getAs<SILBoxType>();
    require(boxTy, "alloc_box must have a @box type");

    require(AI->getType().isObject(),
            "result of alloc_box must be an object");
    for (unsigned field : indices(AI->getBoxType()->getLayout()->getFields())) {
      verifyLocalArchetype(AI, getSILBoxFieldLoweredType(
                                    F.getTypeExpansionContext(), AI->getBoxType(),
                                    F.getModule().Types, field));
    }

    // An alloc_box with a mark_uninitialized user can not have any other users.
    require(none_of(AI->getUses(),
                    [](Operand *Op) -> bool {
                      return isa<MarkUninitializedInst>(Op->getUser());
                    }) ||
                AI->hasOneUse(),
            "An alloc_box with a mark_uninitialized user can not have any "
            "other users.");
  }

  void checkDeallocBoxInst(DeallocBoxInst *DI) {
    auto boxTy = DI->getOperand()->getType().getAs<SILBoxType>();
    require(boxTy, "operand must be a @box type");
    require(DI->getOperand()->getType().isObject(),
            "operand must be an object");
  }

  void checkDestroyAddrInst(DestroyAddrInst *DI) {
    require(DI->getOperand()->getType().isAddress(),
            "Operand of destroy_addr must be address");
    require(F.isTypeABIAccessible(DI->getOperand()->getType()),
            "cannot directly destroy type with inaccessible ABI");
  }

  void checkBindMemoryInst(BindMemoryInst *BI) {
    require(BI->getBoundType(), "BI must have a bound type");
    require(BI->getBase()->getType().is<BuiltinRawPointerType>(),
            "bind_memory base be a RawPointer");
    requireSameType(BI->getIndex()->getType(),
                    SILType::getBuiltinWordType(F.getASTContext()),
                    "bind_memory index must be a Word");
  }

  void checkRebindMemoryInst(RebindMemoryInst *rbi) {
    require(rbi->getBase()->getType().is<BuiltinRawPointerType>(),
            "rebind_memory base be a RawPointer");
    requireSameType(rbi->getInToken()->getType(),
                    SILType::getBuiltinWordType(F.getASTContext()),
                    "rebind_memory token must be a Builtin.Int64");
  }

  void checkIndexAddrInst(IndexAddrInst *IAI) {
    require(IAI->getType().isAddress(), "index_addr must produce an address");
    requireSameType(
        IAI->getType(), IAI->getBase()->getType(),
        "index_addr must produce an address of the same type as its base");
    require(IAI->getIndex()->getType().is<BuiltinIntegerType>(),
            "index_addr index must be of a builtin integer type");
  }

  void checkTailAddrInst(TailAddrInst *IAI) {
    require(IAI->getType().isAddress(), "tail_addr must produce an address");
    require(IAI->getIndex()->getType().is<BuiltinIntegerType>(),
            "tail_addr index must be of a builtin integer type");
  }

  void checkIndexRawPointerInst(IndexRawPointerInst *IAI) {
    require(IAI->getType().is<BuiltinRawPointerType>(),
            "index_raw_pointer must produce a RawPointer");
    require(IAI->getBase()->getType().is<BuiltinRawPointerType>(),
            "index_raw_pointer base must be a RawPointer");
    require(IAI->getIndex()->getType().is<BuiltinIntegerType>(),
            "index_raw_pointer index must be of a builtin integer type");
  }

  void checkTupleExtractInst(TupleExtractInst *EI) {
    CanTupleType operandTy = requireObjectType(TupleType, EI->getOperand(),
                                               "Operand of tuple_extract");
    require(!operandTy.containsPackExpansionType(),
            "tuple_extract cannot be used with tuples containing "
            "pack expansions");

    require(EI->getType().isObject(),
            "result of tuple_extract must be object");

    require(EI->getFieldIndex() < operandTy->getNumElements(),
            "invalid field index for tuple_extract instruction");

    require(EI->getForwardingOwnershipKind() == OwnershipKind::None ||
                EI->getForwardingOwnershipKind() == OwnershipKind::Guaranteed,
            "invalid forwarding ownership kind on tuple_extract instruction");
    if (EI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(EI->getType().getASTType(),
                      operandTy.getElementType(EI->getFieldIndex()),
                      "type of tuple_extract does not match type of element");
    }
  }

  void checkStructExtractInst(StructExtractInst *EI) {
    SILType operandTy = EI->getOperand()->getType();
    require(operandTy.isObject(),
            "cannot struct_extract from address");
    require(EI->getType().isObject(),
            "result of struct_extract cannot be address");
    StructDecl *sd = operandTy.getStructOrBoundGenericStruct();
    require(sd, "must struct_extract from struct");
    require(!checkResilience(sd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient struct");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot load computed property with struct_extract");

    require(EI->getField()->getDeclContext() == sd,
            "struct_extract field is not a member of the struct");

    require(EI->getForwardingOwnershipKind() == OwnershipKind::None ||
                EI->getForwardingOwnershipKind() == OwnershipKind::Guaranteed,
            "invalid forwarding ownership kind on tuple_extract instruction");

    require(!EI->getModule()
                    .getOptions()
                    .EnableImportPtrauthFieldFunctionPointers ||
                !EI->getField()->getPointerAuthQualifier().isPresent(),
            "Imported structs with ptrauth qualified fields should not be "
            "promoted to a value");

    if (EI->getModule().getStage() != SILStage::Lowered) {
      SILType loweredFieldTy = operandTy.getFieldType(
          EI->getField(), F.getModule(), F.getTypeExpansionContext());
      requireSameType(loweredFieldTy, EI->getType(),
                      "result of struct_extract does not match type of field");
    }
  }

  void checkTupleElementAddrInst(TupleElementAddrInst *EI) {
    require(EI->getType().isAddress(),
            "result of tuple_element_addr must be address");
    SILType operandTy = EI->getOperand()->getType();
    auto tupleType = requireAddressType(TupleType, operandTy,
            "operand of tuple_element_addr must be the address of a tuple");
    require(!tupleType.containsPackExpansionType(),
            "tuple_element_addr cannot be used with tuples containing "
            "pack expansions");

    require(EI->getFieldIndex() < tupleType->getNumElements(),
            "invalid field index for tuple_element_addr instruction");
    if (EI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          EI->getType().getASTType(),
          tupleType.getElementType(EI->getFieldIndex()),
          "type of tuple_element_addr does not match type of element");
    }
  }

  void checkStructElementAddrInst(StructElementAddrInst *EI) {
    SILType operandTy = EI->getOperand()->getType();
    require(operandTy.isAddress(),
            "must derive struct_element_addr from address");
    StructDecl *sd = operandTy.getStructOrBoundGenericStruct();
    require(sd, "struct_element_addr operand must be struct address");
    require(!checkResilience(sd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient struct");
    require(EI->getType().isAddress(),
            "result of struct_element_addr must be address");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot get address of computed property with struct_element_addr");

    require(EI->getField()->getDeclContext() == sd,
            "struct_element_addr field is not a member of the struct");

    if (EI->getModule().getOptions().EnableImportPtrauthFieldFunctionPointers &&
        EI->getField()->getPointerAuthQualifier().isPresent()) {
      for (auto *use : EI->getUses()) {
        auto *bai = dyn_cast<BeginAccessInst>(use->getUser());
        require(bai && bai->getEnforcement() == SILAccessEnforcement::Signed,
                "Access to ptrauth qualified fields should be scoped with "
                "begin_access [signed]/end_access");
      }
    }
    if (EI->getModule().getStage() != SILStage::Lowered) {
      SILType loweredFieldTy = operandTy.getFieldType(
          EI->getField(), F.getModule(), F.getTypeExpansionContext());
      requireSameType(
          loweredFieldTy, EI->getType(),
          "result of struct_element_addr does not match type of field");
    }
  }

  void checkRefElementAddrInst(RefElementAddrInst *EI) {
    requireReferenceValue(EI->getOperand(), "Operand of ref_element_addr");
    require(EI->getType().isAddress(),
            "result of ref_element_addr must be lvalue");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot get address of computed property with ref_element_addr");
    SILType operandTy = EI->getOperand()->getType();
    ClassDecl *cd = operandTy.getClassOrBoundGenericClass();
    require(cd, "ref_element_addr operand must be a class instance");
    require(!checkResilience(cd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient class");

    require(EI->getField()->getDeclContext() ==
                cd->getImplementationContext()->getAsGenericContext(),
            "ref_element_addr field must be a member of the class");

    if (EI->getModule().getStage() != SILStage::Lowered) {
      SILType loweredFieldTy = operandTy.getFieldType(
          EI->getField(), F.getModule(), F.getTypeExpansionContext());
      requireSameType(
          loweredFieldTy, EI->getType(),
          "result of ref_element_addr does not match type of field");
    }
    EI->getFieldIndex();  // Make sure we can access the field without crashing.
    checkAddressWalkerCanVisitAllTransitiveUses(EI);
  }

  void checkRefTailAddrInst(RefTailAddrInst *RTAI) {
    requireReferenceValue(RTAI->getOperand(), "Operand of ref_tail_addr");
    require(RTAI->getType().isAddress(),
            "result of ref_tail_addr must be lvalue");
    SILType operandTy = RTAI->getOperand()->getType();
    ClassDecl *cd = operandTy.getClassOrBoundGenericClass();
    require(cd, "ref_tail_addr operand must be a class instance");
    require(!checkResilience(cd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient class");
    require(cd, "ref_tail_addr operand must be a class instance");
    checkAddressWalkerCanVisitAllTransitiveUses(RTAI);
  }

  void checkDestructureStructInst(DestructureStructInst *DSI) {
    SILType operandTy = DSI->getOperand()->getType();
    StructDecl *sd = operandTy.getStructOrBoundGenericStruct();
    require(sd, "must struct_extract from struct");
    require(!checkResilience(sd, F.getModule().getSwiftModule(),
                             F.getResilienceExpansion()),
            "cannot access storage of resilient struct");
    if (F.hasOwnership()) {
      // Make sure that all of our destructure results ownership kinds are
      // compatible with our destructure_struct's ownership kind /and/ that if
      // our destructure ownership kind is non-trivial then all non-trivial
      // results must have the same ownership kind as our operand.
      auto parentKind = DSI->getForwardingOwnershipKind();
      for (const auto &result : DSI->getAllResultsBuffer()) {
        require(parentKind.isCompatibleWith(result.getOwnershipKind()),
                "destructure result with ownership that is incompatible with "
                "parent forwarding ownership kind");
        require(parentKind != OwnershipKind::None ||
                    result.getOwnershipKind() == OwnershipKind::None,
                "destructure with none ownership kind operand and non-none "
                "ownership kind result?!");
      }
      if (operandTy.getNominalOrBoundGenericNominal()
          ->getValueTypeDestructor()) {
        require(
          isa<DropDeinitInst>(lookThroughOwnershipInsts(DSI->getOperand())),
            "a destructure of a move-only-type-with-deinit requires a "
            "drop_deinit");
      }
    }
  }

  void checkDestructureTupleInst(DestructureTupleInst *dti) {
    if (F.hasOwnership()) {
      // Make sure that all of our destructure results ownership kinds are
      // compatible with our destructure_struct's ownership kind /and/ that if
      // our destructure ownership kind is non-trivial then all non-trivial
      // results must have the same ownership kind as our operand.
      auto parentKind = dti->getForwardingOwnershipKind();
      for (const auto &result : dti->getAllResultsBuffer()) {
        require(parentKind.isCompatibleWith(result.getOwnershipKind()),
                "destructure result with ownership that is incompatible with "
                "parent forwarding ownership kind");
        require(parentKind != OwnershipKind::None ||
                    result.getOwnershipKind() == OwnershipKind::None,
                "destructure with none ownership kind operand and non-none "
                "ownership kind result?!");
      }
    }
  }

  SILType getMethodSelfType(CanSILFunctionType ft) {
    SILFunctionConventions fnConv(ft, F.getModule());
    return fnConv.getSILType(ft->getParameters().back(),
                             F.getTypeExpansionContext());
  }

  void checkWitnessMethodInst(WitnessMethodInst *AMI) {
    auto methodType = requireObjectType(SILFunctionType, AMI,
                                        "result of witness_method");

    auto *protocol
      = dyn_cast<ProtocolDecl>(AMI->getMember().getDecl()->getDeclContext());
    require(protocol,
            "witness_method method must be a protocol method");

    require(methodType->getRepresentation()
              == F.getModule().Types.getProtocolWitnessRepresentation(protocol),
            "result of witness_method must have correct representation for protocol");

    require(methodType->isPolymorphic(),
            "result of witness_method must be polymorphic");

    auto genericSig = methodType->getInvocationGenericSignature();

    auto selfGenericParam = genericSig.getGenericParams()[0];
    require(selfGenericParam->getDepth() == 0
            && selfGenericParam->getIndex() == 0,
            "method should be polymorphic on Self parameter at depth 0 index 0");
    llvm::Optional<Requirement> selfRequirement;
    for (auto req : genericSig.getRequirements()) {
      if (req.getKind() != RequirementKind::SameType) {
        selfRequirement = req;
        break;
      }
    }

    require(selfRequirement &&
            selfRequirement->getKind() == RequirementKind::Conformance,
            "first non-same-typerequirement should be conformance requirement");
    const auto protos = genericSig->getRequiredProtocols(selfGenericParam);
    require(std::find(protos.begin(), protos.end(), protocol) != protos.end(),
            "requirement Self parameter must conform to called protocol");

    auto lookupType = AMI->getLookupType();
    if (getLocalArchetypeOf(lookupType) || lookupType->hasDynamicSelfType()) {
      require(AMI->getTypeDependentOperands().size() == 1,
              "Must have a type dependent operand for the opened archetype");
      verifyLocalArchetype(AMI, lookupType);
    } else {
      require(AMI->getTypeDependentOperands().empty(),
              "Should not have an operand for the opened existential");
    }
    if (!isa<ArchetypeType>(lookupType) && !isa<DynamicSelfType>(lookupType)) {
      require(AMI->getConformance().isConcrete(),
              "concrete type lookup requires concrete conformance");
      auto conformance = AMI->getConformance().getConcrete();
      requireSameType(
          conformance->getType(), AMI->getLookupType(),
          "concrete type lookup requires conformance that matches type");
    }

    require(AMI->getMember().requiresNewWitnessTableEntry(),
            "method does not have a witness table entry");
  }

  /// Verify the given type of a dynamic or @objc optional method reference.
  bool verifyDynamicMethodType(CanSILFunctionType verifiedTy, SILType selfType,
                               SILDeclRef method) {
    auto &C = F.getASTContext();

    // The type of the dynamic method must match the usual type of the method,
    // but with the more opaque Self type.
    auto constantInfo =
        F.getModule().Types.getConstantInfo(F.getTypeExpansionContext(), method);
    auto methodTy = constantInfo.SILFnType;

    assert(!methodTy->isCoroutine());

    // Map interface types to archetypes.
    if (auto *env = F.getModule().Types.getConstantGenericEnvironment(method)) {
      auto subs = env->getForwardingSubstitutionMap();
      methodTy = methodTy->substGenericArgs(F.getModule(), subs,
                                            F.getTypeExpansionContext());
    }
    assert(!methodTy->isPolymorphic());

    // Assume the parameter conventions are correct.
    SmallVector<SILParameterInfo, 4> params;
    {
      const auto actualParams = methodTy->getParameters();
      const auto verifiedParams = verifiedTy->getParameters();
      if (actualParams.size() != verifiedParams.size())
        return false;

      for (const auto idx : indices(actualParams)) {
        params.push_back(actualParams[idx].getWithConvention(
            verifiedParams[idx].getConvention()));
      }
    }

    // Have the 'self' parameter assume the type of 'self' at the call site.
    params.back() = params.back().getWithInterfaceType(selfType.getASTType());

    // Assume the result conventions are correct.
    SmallVector<SILResultInfo, 4> results;
    {
      const auto actualResults = methodTy->getResults();
      const auto verifiedResults = verifiedTy->getResults();
      if (actualResults.size() != verifiedResults.size())
        return false;

      for (const auto idx : indices(actualResults)) {
        results.push_back(actualResults[idx].getWithConvention(
            verifiedResults[idx].getConvention()));
      }
    }

    // If the method returns dynamic Self, substitute AnyObject for the
    // result type.
    if (auto fnDecl = dyn_cast<FuncDecl>(method.getDecl())) {
      if (fnDecl->hasDynamicSelfResult()) {
        auto anyObjectTy = C.getAnyObjectType();
        for (auto &result : results) {
          auto newResultTy =
              result
                  .getReturnValueType(F.getModule(), methodTy,
                                      F.getTypeExpansionContext())
                  ->replaceCovariantResultType(anyObjectTy, 0);
          result = SILResultInfo(newResultTy->getCanonicalType(),
                                 result.getConvention());
        }
      }
    }

    auto fnTy = SILFunctionType::get(nullptr,
                                     methodTy->getExtInfo(),
                                     methodTy->getCoroutineKind(),
                                     methodTy->getCalleeConvention(),
                                     params,
                                     methodTy->getYields(),
                                     results,
                                     methodTy->getOptionalErrorResult(),
                                     SubstitutionMap(), SubstitutionMap(),
                                     F.getASTContext());

    return fnTy->isBindableTo(verifiedTy);
  }

  /// Visitor class that checks whether a given decl ref has an entry in the
  /// class's vtable.
  class VerifyClassMethodVisitor
      : public SILVTableVisitor<VerifyClassMethodVisitor>
  {
  public:
    SILDeclRef Method;
    bool Seen = false;

    VerifyClassMethodVisitor(SILDeclRef method)
      : Method(method.getOverriddenVTableEntry()) {
      auto *theClass = cast<ClassDecl>(Method.getDecl()->getDeclContext());
      addVTableEntries(theClass);
    }

    void addMethod(SILDeclRef method) {
      if (Seen)
        return;
      if (method == Method)
        Seen = true;
    }

    void addMethodOverride(SILDeclRef base, SILDeclRef derived) {}

    void addPlaceholder(MissingMemberDecl *) {}
  };

  void checkClassMethodInst(ClassMethodInst *CMI) {
    auto member = CMI->getMember();
    auto overrideTy =
        TC.getConstantOverrideType(F.getTypeExpansionContext(), member);
    if (CMI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          CMI->getType(), SILType::getPrimitiveObjectType(overrideTy),
          "result type of class_method must match abstracted type of method");
    }
    auto methodType = requireObjectType(SILFunctionType, CMI,
                                        "result of class_method");
    require(!methodType->getExtInfo().hasContext(),
            "result method must be of a context-free function type");

    SILType operandType = CMI->getOperand()->getType();
    require(operandType.isClassOrClassMetatype(),
            "operand must be of a class type");
    require(getMethodSelfType(methodType).isClassOrClassMetatype(),
            "result must be a method of a class");
    
    require(!member.isForeign,
            "foreign method cannot be dispatched natively");
    require(!isa<ExtensionDecl>(member.getDecl()->getDeclContext()),
            "extension method cannot be dispatched natively");

    // The method ought to appear in the class vtable unless it's a foreign
    // reference type.
    require(VerifyClassMethodVisitor(member).Seen ||
            operandType.isForeignReferenceType(),
            "method does not appear in the class's vtable");
  }

  void checkSuperMethodInst(SuperMethodInst *CMI) {
    auto member = CMI->getMember();
    auto overrideTy =
        TC.getConstantOverrideType(F.getTypeExpansionContext(), member);
    if (CMI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          CMI->getType(), SILType::getPrimitiveObjectType(overrideTy),
          "result type of super_method must match abstracted type of method");
    }
    auto methodType = requireObjectType(SILFunctionType, CMI,
                                        "result of super_method");
    require(!methodType->getExtInfo().hasContext(),
            "result method must be of a context-free function type");
    SILType operandType = CMI->getOperand()->getType();
    require(operandType.isClassOrClassMetatype(),
            "operand must be of a class type");
    require(getMethodSelfType(methodType).isClassOrClassMetatype(),
            "result must be a method of a class");

    require(!member.isForeign,
            "foreign method cannot be dispatched natively");
    require(!isa<ExtensionDecl>(member.getDecl()->getDeclContext()),
            "extension method cannot be dispatched natively");

    auto decl = CMI->getMember().getDecl();
    auto methodClass = decl->getDeclContext()->getDeclaredInterfaceType();

    require(methodClass->getClassOrBoundGenericClass(),
            "super_method must look up a class method");

    // The method ought to appear in the class vtable.
    require(VerifyClassMethodVisitor(member).Seen,
            "method does not appear in the class's vtable");    
  }

  void checkObjCMethodInst(ObjCMethodInst *OMI) {
    auto member = OMI->getMember();
    require(member.isForeign,
            "native method cannot be dispatched via objc");

    auto methodType = requireObjectType(SILFunctionType, OMI,
                                        "result of objc_method");
    require(!methodType->getExtInfo().hasContext(),
            "result method must be of a context-free function type");
    require(methodType->getRepresentation()
            == SILFunctionTypeRepresentation::ObjCMethod,
            "wrong function type representation");

    auto operandType = OMI->getOperand()->getType();
    auto operandInstanceType = operandType.getASTType();
    if (auto metatypeType = dyn_cast<MetatypeType>(operandInstanceType))
      operandInstanceType = metatypeType.getInstanceType();

    if (operandInstanceType.getClassOrBoundGenericClass()) {
      auto overrideTy =
          TC.getConstantOverrideType(F.getTypeExpansionContext(), member);
      requireSameType(
          OMI->getType(), SILType::getPrimitiveObjectType(overrideTy),
          "result type of objc_method must match abstracted type of method");
    } else {
      require(isa<ArchetypeType>(operandInstanceType) ||
              operandInstanceType->isObjCExistentialType(),
              "operand type must be an archetype or self-conforming existential");
      verifyLocalArchetype(OMI, OMI->getType().getASTType());
    }

    // TODO: We should enforce that ObjC methods are dispatched on ObjC
    // metatypes, but IRGen appears not to care right now.
#if 0
    if (auto metaTy = operandType.getAs<AnyMetatypeType>()) {
      bool objcMetatype
        = metaTy->getRepresentation() == MetatypeRepresentation::ObjC;
      require(objcMetatype,
              "objc class methods must be invoked on objc metatypes");
    }
#endif
  }

  void checkObjCSuperMethodInst(ObjCSuperMethodInst *OMI) {
    auto member = OMI->getMember();
    auto overrideTy =
        TC.getConstantOverrideType(F.getTypeExpansionContext(), member);
    if (OMI->getModule().getStage() != SILStage::Lowered) {
      requireSameType(
          OMI->getType(), SILType::getPrimitiveObjectType(overrideTy),
          "result type of super_method must match abstracted type of method");
    }
    auto methodType = requireObjectType(SILFunctionType, OMI,
                                        "result of objc_super_method");
    require(!methodType->getExtInfo().hasContext(),
            "result method must be of a context-free function type");
    SILType operandType = OMI->getOperand()->getType();
    require(operandType.isClassOrClassMetatype(),
            "operand must be of a class type");
    require(getMethodSelfType(methodType).isClassOrClassMetatype(),
            "result must be a method of a class");

    require(member.isForeign,
            "native method cannot be dispatched via objc");

    auto decl = member.getDecl();
    auto methodClass = decl->getDeclContext()->getDeclaredInterfaceType();

    require(methodClass->getClassOrBoundGenericClass(),
            "objc_super_method must look up a class method");
  }

  void checkOpenExistentialAddrInst(OpenExistentialAddrInst *OEI) {
    SILType operandType = OEI->getOperand()->getType();
    require(operandType.isAddress(),
            "open_existential_addr must be applied to address");
    require(operandType.canUseExistentialRepresentation(
                                        ExistentialRepresentation::Opaque),
           "open_existential_addr must be applied to opaque existential");

    require(OEI->getType().isAddress(),
            "open_existential_addr result must be an address");

    auto archetype = getOpenedArchetypeOf(OEI->getType().getASTType());
    require(archetype,
        "open_existential_addr result must be an opened existential archetype");
    require(OEI->getModule().getRootLocalArchetypeDefInst(
                archetype, OEI->getFunction()) == OEI,
            "Archetype opened by open_existential_addr should be registered in "
            "SILFunction");

    // Check all the uses. Consuming or mutating uses must have mutable access
    // to the opened value.
    auto allowedAccessKind = OEI->getAccessKind();
    if (allowedAccessKind == OpenedExistentialAccess::Mutable)
      return;

    require(allowedAccessKind == OpenedExistentialAccess::Mutable ||
                !ImmutableAddressUseVerifier().isMutatingOrConsuming(OEI),
            "open_existential_addr uses that consumes or mutates but is not "
            "opened for mutation");
  }

  void checkOpenExistentialRefInst(OpenExistentialRefInst *OEI) {
    SILType operandType = OEI->getOperand()->getType();
    require(operandType.isObject(),
            "open_existential_ref operand must not be address");

    require(operandType.canUseExistentialRepresentation(
                                              ExistentialRepresentation::Class),
            "open_existential_ref operand must be class existential");

    CanType resultInstanceTy = OEI->getType().getASTType();

    require(OEI->getType().isObject(),
            "open_existential_ref result must be an address");

    auto archetype = getOpenedArchetypeOf(resultInstanceTy);
    require(archetype,
        "open_existential_ref result must be an opened existential archetype");
    require(OEI->getModule().getRootLocalArchetypeDefInst(
                archetype, OEI->getFunction()) == OEI,
            "Archetype opened by open_existential_ref should be registered in "
            "SILFunction");
  }

  void checkOpenExistentialBoxInst(OpenExistentialBoxInst *OEI) {
    SILType operandType = OEI->getOperand()->getType();
    require(operandType.isObject(),
            "open_existential_box operand must not be address");

    require(operandType.canUseExistentialRepresentation(
                                              ExistentialRepresentation::Boxed),
            "open_existential_box operand must be boxed existential");

    CanType resultInstanceTy = OEI->getType().getASTType();

    require(OEI->getType().isAddress(),
            "open_existential_box result must be an address");

    auto archetype = getOpenedArchetypeOf(resultInstanceTy);
    require(archetype,
        "open_existential_box result must be an opened existential archetype");
    require(OEI->getModule().getRootLocalArchetypeDefInst(
                archetype, OEI->getFunction()) == OEI,
            "Archetype opened by open_existential_box should be registered in "
            "SILFunction");
  }

  void checkOpenExistentialBoxValueInst(OpenExistentialBoxValueInst *OEI) {
    SILType operandType = OEI->getOperand()->getType();
    require(operandType.isObject(),
            "open_existential_box operand must not be address");

    require(operandType.canUseExistentialRepresentation(
                                              ExistentialRepresentation::Boxed),
            "open_existential_box operand must be boxed existential");

    CanType resultInstanceTy = OEI->getType().getASTType();

    require(!OEI->getType().isAddress(),
            "open_existential_box_value result must not be an address");

    auto archetype = getOpenedArchetypeOf(resultInstanceTy);
    require(archetype,
        "open_existential_box_value result not an opened existential archetype");
    require(OEI->getModule().getRootLocalArchetypeDefInst(
                archetype, OEI->getFunction()) == OEI,
            "Archetype opened by open_existential_box_value should be "
            "registered in SILFunction");
  }

  void checkOpenExistentialMetatypeInst(OpenExistentialMetatypeInst *I) {
    SILType operandType = I->getOperand()->getType();
    require(operandType.isObject(),
            "open_existential_metatype operand must not be address");
    require(operandType.is<ExistentialMetatypeType>(),
            "open_existential_metatype operand must be existential metatype");
    require(operandType.castTo<ExistentialMetatypeType>()->hasRepresentation(),
            "open_existential_metatype operand must have a representation");

    SILType resultType = I->getType();
    require(resultType.isObject(),
            "open_existential_metatype result must not be address");
    require(resultType.is<MetatypeType>(),
            "open_existential_metatype result must be metatype");
    require(resultType.castTo<MetatypeType>()->hasRepresentation(),
            "open_existential_metatype result must have a representation");
    require(operandType.castTo<ExistentialMetatypeType>()->getRepresentation()
              == resultType.castTo<MetatypeType>()->getRepresentation(),
            "open_existential_metatype result must match representation of "
            "operand");

    CanType operandInstTy =
      operandType.castTo<ExistentialMetatypeType>().getInstanceType();
    CanType resultInstTy = 
      resultType.castTo<MetatypeType>().getInstanceType();

    while (auto operandMetatype =
             dyn_cast<ExistentialMetatypeType>(operandInstTy)) {
      require(isa<MetatypeType>(resultInstTy),
              "metatype depth mismatch in open_existential_metatype result");
      operandInstTy = operandMetatype.getInstanceType();
      resultInstTy = cast<MetatypeType>(resultInstTy).getInstanceType();
    }

    require(operandInstTy.isExistentialType(),
            "ill-formed existential metatype in open_existential_metatype "
            "operand");
    auto archetype = getOpenedArchetypeOf(resultInstTy);
    require(archetype, "open_existential_metatype result must be an opened "
                       "existential metatype");
    require(
        I->getModule().getRootLocalArchetypeDefInst(archetype,
                                                     I->getFunction()) == I,
        "Archetype opened by open_existential_metatype should be registered in "
        "SILFunction");
  }

  void checkOpenExistentialValueInst(OpenExistentialValueInst *OEI) {
    SILType operandType = OEI->getOperand()->getType();
    require(!operandType.isAddress(),
            "open_existential_value must not be applied to address");
    require(operandType.canUseExistentialRepresentation(
                ExistentialRepresentation::Opaque),
            "open_existential_value must be applied to opaque existential");

    require(!OEI->getType().isAddress(),
            "open_existential_value result must not be an address");

    auto archetype = getOpenedArchetypeOf(OEI->getType().getASTType());
    require(archetype, "open_existential_value result must be an opened "
                       "existential archetype");
    require(OEI->getModule().getRootLocalArchetypeDefInst(
                archetype, OEI->getFunction()) == OEI,
            "Archetype opened by open_existential should be registered in "
            "SILFunction");
  }

  void checkAllocExistentialBoxInst(AllocExistentialBoxInst *AEBI) {
    SILType exType = AEBI->getExistentialType();
    require(exType.isObject(),
            "alloc_existential_box #0 result should be a value");
    require(exType.canUseExistentialRepresentation(
                                             ExistentialRepresentation::Boxed,
                                             AEBI->getFormalConcreteType()),
            "alloc_existential_box must be used with a boxed existential "
            "type");
    
    checkExistentialProtocolConformances(exType.getASTType(),
                                         AEBI->getFormalConcreteType(),
                                         AEBI->getConformances());
    verifyLocalArchetype(AEBI, AEBI->getFormalConcreteType());
  }

  void checkInitExistentialAddrInst(InitExistentialAddrInst *AEI) {
    SILType exType = AEI->getOperand()->getType();
    require(exType.isAddress(),
            "init_existential_addr must be applied to an address");
    require(exType.canUseExistentialRepresentation(
                                       ExistentialRepresentation::Opaque,
                                       AEI->getFormalConcreteType()),
            "init_existential_addr must be used with an opaque "
            "existential type");
    
    // The lowered type must be the properly-abstracted form of the AST type.
    auto archetype = OpenedArchetypeType::get(exType.getASTType(),
                                              F.getGenericSignature());

    auto loweredTy = F.getLoweredType(Lowering::AbstractionPattern(archetype),
                                      AEI->getFormalConcreteType())
                      .getAddressType();
    
    requireSameType(loweredTy, AEI->getLoweredConcreteType(),
                    "init_existential_addr result type must be the lowered "
                    "concrete type at the right abstraction level");

    require(isLoweringOf(AEI->getLoweredConcreteType(),
                         AEI->getFormalConcreteType()),
            "init_existential_addr payload must be a lowering of the formal "
            "concrete type");
    
    checkExistentialProtocolConformances(exType.getASTType(),
                                         AEI->getFormalConcreteType(),
                                         AEI->getConformances());
    verifyLocalArchetype(AEI, AEI->getFormalConcreteType());
  }

  void checkInitExistentialValueInst(InitExistentialValueInst *IEI) {
    SILType concreteType = IEI->getOperand()->getType();
    require(!concreteType.isAddress(),
            "init_existential_value must not be used on addresses");
    require(!IEI->getType().isAddress(),
            "init_existential_value result must not be an address");
    // The operand must be at the right abstraction level for the existential.
    SILType exType = IEI->getType();
    auto archetype = OpenedArchetypeType::get(exType.getASTType(),
                                              F.getGenericSignature());
    auto loweredTy = F.getLoweredType(Lowering::AbstractionPattern(archetype),
                                      IEI->getFormalConcreteType());
    requireSameType(
        concreteType, loweredTy,
        "init_existential_value operand must be lowered to the right "
        "abstraction level for the existential");

    require(isLoweringOf(IEI->getOperand()->getType(),
                         IEI->getFormalConcreteType()),
            "init_existential_value operand must be a lowering of the formal "
            "concrete type");

    checkExistentialProtocolConformances(exType.getASTType(),
                                         IEI->getFormalConcreteType(),
                                         IEI->getConformances());
    verifyLocalArchetype(IEI, IEI->getFormalConcreteType());
  }

  void checkInitExistentialRefInst(InitExistentialRefInst *IEI) {
    SILType concreteType = IEI->getOperand()->getType();
    require(concreteType.getASTType()->isBridgeableObjectType(),
            "init_existential_ref operand must be a class instance");
    require(IEI->getType().canUseExistentialRepresentation(
                                     ExistentialRepresentation::Class,
                                     IEI->getFormalConcreteType()),
            "init_existential_ref must be used with a class existential type");
    require(IEI->getType().isObject(),
            "init_existential_ref result must not be an address");
    
    // The operand must be at the right abstraction level for the existential.
    SILType exType = IEI->getType();
    auto archetype = OpenedArchetypeType::get(exType.getASTType(),
                                              F.getGenericSignature());
    auto loweredTy = F.getLoweredType(Lowering::AbstractionPattern(archetype),
                                      IEI->getFormalConcreteType());
    requireSameType(concreteType, loweredTy,
                    "init_existential_ref operand must be lowered to the right "
                    "abstraction level for the existential");
    
    require(isLoweringOf(IEI->getOperand()->getType(),
                         IEI->getFormalConcreteType()),
            "init_existential_ref operand must be a lowering of the formal "
            "concrete type");
    
    checkExistentialProtocolConformances(exType.getASTType(),
                                         IEI->getFormalConcreteType(),
                                         IEI->getConformances());
    verifyLocalArchetype(IEI, IEI->getFormalConcreteType());
  }

  void checkDeinitExistentialAddrInst(DeinitExistentialAddrInst *DEI) {
    SILType exType = DEI->getOperand()->getType();
    require(exType.isAddress(),
            "deinit_existential_addr must be applied to an address");
    require(exType.canUseExistentialRepresentation(
                ExistentialRepresentation::Opaque),
            "deinit_existential_addr must be applied to an opaque existential");
  }

  void checkDeinitExistentialValueInst(DeinitExistentialValueInst *DEI) {
    SILType exType = DEI->getOperand()->getType();
    require(!exType.isAddress(),
            "deinit_existential_value must not be applied to an address");
    require(
        exType.canUseExistentialRepresentation(
            ExistentialRepresentation::Opaque),
        "deinit_existential_value must be applied to an opaque existential");
  }
  
  void checkDeallocExistentialBoxInst(DeallocExistentialBoxInst *DEBI) {
    SILType exType = DEBI->getOperand()->getType();
    require(exType.isObject(),
            "dealloc_existential_box must be applied to a value");
    require(exType.canUseExistentialRepresentation(
                                       ExistentialRepresentation::Boxed),
            "dealloc_existential_box must be applied to a boxed "
            "existential");
  }

  void checkInitExistentialMetatypeInst(InitExistentialMetatypeInst *I) {
    SILType operandType = I->getOperand()->getType();
    require(operandType.isObject(),
            "init_existential_metatype operand must not be an address");
    require(operandType.is<MetatypeType>(),
            "init_existential_metatype operand must be a metatype");
    require(operandType.castTo<MetatypeType>()->hasRepresentation(),
            "init_existential_metatype operand must have a representation");

    SILType resultType = I->getType();
    require(resultType.is<ExistentialMetatypeType>(),
            "init_existential_metatype result must be an existential metatype");
    auto MetaTy = resultType.castTo<ExistentialMetatypeType>();
    require(resultType.isObject(),
            "init_existential_metatype result must not be an address");
    require(MetaTy->hasRepresentation(),
            "init_existential_metatype result must have a representation");
    require(MetaTy->getRepresentation()
              == operandType.castTo<MetatypeType>()->getRepresentation(),
            "init_existential_metatype result must match representation of "
            "operand");

    auto resultInstanceType = resultType.getASTType();
    auto operandInstanceType = operandType.getASTType();
    while (isa<ExistentialMetatypeType>(resultInstanceType)) {
      resultInstanceType =
          cast<ExistentialMetatypeType>(resultInstanceType).getInstanceType();
      operandInstanceType =
          cast<MetatypeType>(operandInstanceType).getInstanceType();
    }

    checkExistentialProtocolConformances(resultInstanceType,
                                         operandInstanceType,
                                         I->getConformances());
    verifyLocalArchetype(I, MetaTy.getInstanceType());
  }

  void checkExistentialProtocolConformances(CanType resultType,
                                            CanType concreteType,
                                ArrayRef<ProtocolConformanceRef> conformances) {
    auto layout = resultType.getExistentialLayout();
    auto protocols = layout.getProtocols();

    require(conformances.size() == protocols.size(),
            "init_existential instruction must have the "
            "right number of conformances");

    if (layout.requiresClass()) {
      require(concreteType->mayHaveSuperclass() ||
              (concreteType.isExistentialType() &&
               concreteType.getExistentialLayout().requiresClass()),
              "init_existential of class existential with non-class type");
    }

    if (auto superclass = layout.getSuperclass()) {
      require(superclass->isExactSuperclassOf(concreteType),
              "init_existential of subclass existential with wrong type");
    }

    for (auto i : indices(conformances)) {
      require(conformances[i].getRequirement() == protocols[i]->getDecl(),
              "init_existential instruction must have conformances in "
              "proper order");
    }
  }

  void verifyCheckedCast(bool isExact, SILType fromTy, SILType toTy) {
    // Verify common invariants.
    require(fromTy.isObject() && toTy.isObject(),
            "value checked cast src and dest must be objects");

    auto fromCanTy = fromTy.getASTType();
    auto toCanTy = toTy.getASTType();

    require(canSILUseScalarCheckedCastInstructions(F.getModule(),
                                                   fromCanTy, toCanTy),
            "invalid value checked cast src or dest types");

    // Peel off metatypes. If two types are checked-cast-able, so are their
    // metatypes.
    unsigned MetatyLevel = 0;
    while (isa<AnyMetatypeType>(fromCanTy) && isa<AnyMetatypeType>(toCanTy)) {
      auto fromMetaty = cast<AnyMetatypeType>(fromCanTy);
      auto toMetaty = cast<AnyMetatypeType>(toCanTy);
      
      // Check representations only for the top-level metatypes as only
      // those are SIL-lowered.
      if (!MetatyLevel) {
        // The representations must match.
        require(fromMetaty->getRepresentation() == toMetaty->getRepresentation(),
                "metatype checked cast cannot change metatype representation");

        // We can't handle the 'thin' case yet, but it shouldn't really even be
        // interesting.
        require(fromMetaty->getRepresentation() != MetatypeRepresentation::Thin,
                "metatype checked cast cannot check thin metatypes");
      }

      fromCanTy = fromMetaty.getInstanceType();
      toCanTy = toMetaty.getInstanceType();
      ++MetatyLevel;
    }

    if (isExact) {
      require(fromCanTy.getClassOrBoundGenericClass(),
              "downcast operand must be a class type");
      require(toCanTy.getClassOrBoundGenericClass(),
              "downcast must convert to a class type");
      require(fromCanTy->isBindableToSuperclassOf(toCanTy),
              "downcast must convert to a subclass");
    }
  }

  void checkUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *CI) {
    verifyCheckedCast(/*exact*/ false,
                      CI->getOperand()->getType(),
                      CI->getType());
    verifyLocalArchetype(CI, CI->getType().getASTType());
  }

  // Make sure that opcodes handled by isRCIdentityPreservingCast cannot cast
  // from a trivial to a reference type. Such a cast may dynamically
  // instantiate a new reference-counted object.
  void checkNoTrivialToReferenceCast(SingleValueInstruction *svi) {
    require(!svi->getOperand(0)->getType().isTrivial(*svi->getFunction())
            || svi->getType().isTrivial(*svi->getFunction()),
            "Unexpected trivial-to-reference conversion: ");
  }

  /// Verify if a given type is or contains a local archetype or dynamic self.
  /// If this is the case, verify that the provided instruction has a type
  /// dependent operand for it.
  void verifyLocalArchetype(SILInstruction *I, CanType Ty) {
    if (!Ty)
      return;
    // Check the type and all of its contained types.
    Ty.visit([&](CanType t) {
      SILValue Def;
      if (const auto archetypeTy = dyn_cast<LocalArchetypeType>(t)) {
        Def = I->getModule().getRootLocalArchetypeDefInst(
            archetypeTy.getRoot(), I->getFunction());
        require(Def, "Root opened archetype should be registered in SILModule");
      } else if (t->hasDynamicSelfType()) {
        require(I->getFunction()->hasSelfParam() ||
                I->getFunction()->hasDynamicSelfMetadata(),
              "Function containing dynamic self type must have self parameter");
        if (I->getFunction()->hasDynamicSelfMetadata())
          Def = I->getFunction()->getDynamicSelfMetadata();
        else
          Def = I->getFunction()->getSelfArgument();
      } else {
        return;
      }
      for (auto &TypeDefOp : I->getTypeDependentOperands()) {
        if (TypeDefOp.get() == Def)
          return;
      }
      require(false, "Instruction should contain a type dependent operand for "
                     "every used open archetype or dynamic self");
    });
  }

  void checkCheckedCastBranchInst(CheckedCastBranchInst *CBI) {
    verifyCheckedCast(CBI->isExact(),
                      CBI->getSourceLoweredType(),
                      CBI->getTargetLoweredType());
    verifyLocalArchetype(CBI, CBI->getTargetFormalType());

    require(CBI->getSuccessBB()->args_size() == 1,
            "success dest of checked_cast_br must take one argument");
    requireSameType(
        CBI->getSuccessBB()->args_begin()[0]->getType(),
        CBI->getTargetLoweredType(),
        "success dest block argument of checked_cast_br must match type of "
        "cast");
    if (F.hasOwnership()) {
      require(CBI->getFailureBB()->args_size() == 1,
              "failure dest of checked_cast_br must take one argument in "
              "ownership qualified sil");
      requireSameType(
          CBI->getFailureBB()->args_begin()[0]->getType(),
          CBI->getOperand()->getType(),
          "failure dest block argument must match type of original type in "
          "ownership qualified sil");
      auto succOwnershipKind =
          CBI->getSuccessBB()->args_begin()[0]->getOwnershipKind();
      require(succOwnershipKind.isCompatibleWith(
                  CBI->getOperand()->getOwnershipKind()),
              "succ dest block argument must have ownership compatible with "
              "the checked_cast_br operand");
      auto failOwnershipKind =
          CBI->getFailureBB()->args_begin()[0]->getOwnershipKind();
      require(failOwnershipKind.isCompatibleWith(
                  CBI->getOperand()->getOwnershipKind()),
              "failure dest block argument must have ownership compatible with "
              "the checked_cast_br operand");

      // Do not allow for checked_cast_br to forward guaranteed ownership if the
      // source type is an AnyObject.
      //
      // EXPLANATION: A checked_cast_br from an AnyObject may return a different
      // object. This occurs for instance if one has an AnyObject that is a
      // boxed AnyHashable (ClassType). This breaks with the guarantees of
      // checked_cast_br guaranteed, so we ban it.
      require(!CBI->getOperand()->getType().isAnyObject() ||
                  CBI->getOperand()->getOwnershipKind() !=
                      OwnershipKind::Guaranteed,
              "checked_cast_br with an AnyObject typed source cannot forward "
              "guaranteed ownership");
      require(CBI->preservesOwnership() ||
                  CBI->getOperand()->getOwnershipKind() !=
                      OwnershipKind::Guaranteed,
              "If checked_cast_br is not directly forwarding, it can not have "
              "guaranteed ownership");
    } else {
      require(CBI->getFailureBB()->args_empty(),
              "Failure dest of checked_cast_br must not take any argument in "
              "non-ownership qualified sil");
    }
  }

  void checkCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI) {
    require(CCABI->getSrc()->getType().isAddress(),
            "checked_cast_addr_br src must be an address");
    require(CCABI->getDest()->getType().isAddress(),
            "checked_cast_addr_br dest must be an address");

    require(
        CCABI->getSuccessBB()->args_size() == 0,
        "success dest block of checked_cast_addr_br must not take an argument");
    require(
        CCABI->getFailureBB()->args_size() == 0,
        "failure dest block of checked_cast_addr_br must not take an argument");
  }

  void checkThinToThickFunctionInst(ThinToThickFunctionInst *TTFI) {
    auto opFTy = requireObjectType(SILFunctionType, TTFI->getOperand(),
                                   "thin_to_thick_function operand");
    auto resFTy = requireObjectType(SILFunctionType, TTFI,
                                    "thin_to_thick_function result");
    require(opFTy->isPolymorphic() == resFTy->isPolymorphic(),
            "thin_to_thick_function operand and result type must differ only "
            " in thinness");
    requireSameFunctionComponents(opFTy, resFTy,
                                  "thin_to_thick_function operand and result");

    require(opFTy->getRepresentation() == SILFunctionType::Representation::Thin,
            "operand of thin_to_thick_function must be thin");
    require(resFTy->getRepresentation() == SILFunctionType::Representation::Thick,
            "result of thin_to_thick_function must be thick");

    auto adjustedOperandExtInfo =
        opFTy->getExtInfo()
            .intoBuilder()
            .withRepresentation(SILFunctionType::Representation::Thick)
            .withNoEscape(resFTy->isNoEscape())
            .build();
    require(adjustedOperandExtInfo.isEqualTo(resFTy->getExtInfo(),
                                             useClangTypes(opFTy)),
            "operand and result of thin_to_think_function must agree in "
            "particulars");
  }

  void checkThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCI) {
    auto opTy = requireObjectType(AnyMetatypeType, TTOCI->getOperand(),
                                  "thick_to_objc_metatype operand");
    auto resTy = requireObjectType(AnyMetatypeType, TTOCI,
                                   "thick_to_objc_metatype result");

    require(TTOCI->getOperand()->getType().is<MetatypeType>() ==
            TTOCI->getType().is<MetatypeType>(),
            "thick_to_objc_metatype cannot change metatype kinds");
    require(opTy->getRepresentation() == MetatypeRepresentation::Thick,
            "operand of thick_to_objc_metatype must be thick");
    require(resTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "operand of thick_to_objc_metatype must be ObjC");

    requireSameType(opTy->getInstanceType(), resTy->getInstanceType(),
                    "thick_to_objc_metatype instance types do not match");
  }

  void checkObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTI) {
    auto opTy = requireObjectType(AnyMetatypeType, OCTTI->getOperand(),
                                  "objc_to_thick_metatype operand");
    auto resTy = requireObjectType(AnyMetatypeType, OCTTI,
                                   "objc_to_thick_metatype result");

    require(OCTTI->getOperand()->getType().is<MetatypeType>() ==
            OCTTI->getType().is<MetatypeType>(),
            "objc_to_thick_metatype cannot change metatype kinds");
    require(opTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "operand of objc_to_thick_metatype must be ObjC");
    require(resTy->getRepresentation() == MetatypeRepresentation::Thick,
            "operand of objc_to_thick_metatype must be thick");

    requireSameType(opTy->getInstanceType(), resTy->getInstanceType(),
                    "objc_to_thick_metatype instance types do not match");
  }

  void checkUpcastInst(UpcastInst *UI) {
    require(UI->getType() != UI->getOperand()->getType(),
            "can't upcast to same type");
    require(UI->getType().isObject(), "cannot upcast address types");
    require(UI->getOperand()->getType().isMoveOnlyWrapped() ==
                UI->getType().isMoveOnlyWrapped(),
            "cast cannot be used to remove move only wrapped?!");
    checkNoTrivialToReferenceCast(UI);
    if (UI->getType().is<MetatypeType>()) {
      CanType instTy(UI->getType().castTo<MetatypeType>()->getInstanceType());
      require(UI->getOperand()->getType().is<MetatypeType>(),
              "upcast operand must be a class or class metatype instance");
      CanType opInstTy(UI->getOperand()->getType().castTo<MetatypeType>()
                         ->getInstanceType());
      auto instClass = instTy->getClassOrBoundGenericClass();
      require(instClass,
              "upcast must convert a class metatype to a class metatype");
      
      if (instClass->isTypeErasedGenericClass()) {
        require(instClass->getDeclaredTypeInContext()
                  ->isBindableToSuperclassOf(opInstTy),
                "upcast must cast to a superclass or an existential metatype");
      } else {
        require(instTy->isExactSuperclassOf(opInstTy),
                "upcast must cast to a superclass or an existential metatype");
      }
      return;
    }

    require(UI->getType().getCategory() ==
            UI->getOperand()->getType().getCategory(),
            "Upcast can only upcast in between types of the same "
            "SILValueCategory. This prevents address types from being cast to "
            "object types or vis-a-versa");

    auto ToTy = UI->getType();
    auto FromTy = UI->getOperand()->getType();

    // Upcast from Optional<B> to Optional<A> is legal as long as B is a
    // subclass of A.
    if (ToTy.getASTType().getOptionalObjectType() &&
        FromTy.getASTType().getOptionalObjectType()) {
      ToTy = SILType::getPrimitiveObjectType(
          ToTy.getASTType().getOptionalObjectType());
      FromTy = SILType::getPrimitiveObjectType(
          FromTy.getASTType().getOptionalObjectType());
    }

    auto ToClass = ToTy.getClassOrBoundGenericClass();
    require(ToClass,
            "upcast must convert a class instance to a class type");
      if (ToClass->isTypeErasedGenericClass()) {
        require(ToClass->getDeclaredTypeInContext()
                  ->isBindableToSuperclassOf(FromTy.getASTType()),
                "upcast must cast to a superclass or an existential metatype");
      } else {
        require(ToTy.isExactSuperclassOf(FromTy),
                "upcast must cast to a superclass or an existential metatype");
      }
  }

  void checkAddressToPointerInst(AddressToPointerInst *AI) {
    require(AI->getOperand()->getType().isAddress(),
            "address-to-pointer operand must be an address");
    require(AI->getType().getASTType()->isEqual(
                              AI->getType().getASTContext().TheRawPointerType),
            "address-to-pointer result type must be RawPointer");
  }
  
  void checkUncheckedRefCastInst(UncheckedRefCastInst *AI) {
    verifyLocalArchetype(AI, AI->getType().getASTType());
    require(AI->getOperand()->getType().isObject(),
            "unchecked_ref_cast operand must be a value");
    require(AI->getType().isObject(),
            "unchecked_ref_cast result must be an object");
    require(SILType::canRefCast(AI->getOperand()->getType(), AI->getType(),
                                AI->getModule()),
            "unchecked_ref_cast requires a heap object reference type");
  }

  void checkUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *AI) {
    auto srcTy = AI->getSrc()->getType();
    auto destTy = AI->getDest()->getType();
    require(srcTy.isAddress(),
            "unchecked_ref_cast_addr operand must be an address");
    require(destTy.isAddress(),
            "unchecked_ref_cast_addr result must be an address");
    // The static src/dest types cannot be checked here even if they are
    // loadable. unchecked_ref_cast_addr may accept nonreference static types
    // (as a result of specialization). These cases will never be promoted to
    // value bitcast, thus will cause the subsequent runtime cast to fail.
  }
  
  void checkUncheckedAddrCastInst(UncheckedAddrCastInst *AI) {
    verifyLocalArchetype(AI, AI->getType().getASTType());

    require(AI->getOperand()->getType().isAddress(),
            "unchecked_addr_cast operand must be an address");
    require(AI->getType().isAddress(),
            "unchecked_addr_cast result must be an address");
    require(AI->getOperand()->getType().isMoveOnlyWrapped() ==
                AI->getType().isMoveOnlyWrapped(),
            "Unchecked addr cast cannot be used to remove move only wrapped?!");
  }
  
  void checkUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *BI) {
    verifyLocalArchetype(BI, BI->getType().getASTType());
    require(BI->getOperand()->getType().isObject(),
            "unchecked_trivial_bit_cast must operate on a value");
    require(BI->getType().isObject(),
            "unchecked_trivial_bit_cast must produce a value");
    require(BI->getType().isTrivial(F),
            "unchecked_trivial_bit_cast must produce a value of trivial type");
    require(BI->getOperand()->getType().isMoveOnlyWrapped() ==
                BI->getType().isMoveOnlyWrapped(),
            "cast cannot be used to remove move only wrapped?!");
  }

  void checkUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *BI) {
    verifyLocalArchetype(BI, BI->getType().getASTType());
    require(BI->getOperand()->getType().isObject(),
            "unchecked_bitwise_cast must operate on a value");
    require(BI->getType().isObject(),
            "unchecked_bitwise_cast must produce a value");
    require(BI->getOperand()->getType().isMoveOnlyWrapped() ==
                BI->getType().isMoveOnlyWrapped(),
            "cast cannot be used to remove move only wrapped?!");
  }

  void checkRefToRawPointerInst(RefToRawPointerInst *AI) {
    require(AI->getOperand()->getType().isAnyClassReferenceType(),
            "ref-to-raw-pointer operand must be a class reference or"
            " NativeObject");
    requireSameType(AI->getType().getASTType(),
                    AI->getType().getASTContext().TheRawPointerType,
                    "ref-to-raw-pointer result must be RawPointer");
  }

  void checkRawPointerToRefInst(RawPointerToRefInst *AI) {
    verifyLocalArchetype(AI, AI->getType().getASTType());
    require(AI->getType()
              .getASTType()->isBridgeableObjectType()
            || AI->getType().getASTType()->isEqual(
                             AI->getType().getASTContext().TheNativeObjectType),
        "raw-pointer-to-ref result must be a class reference or NativeObject");
    requireSameType(AI->getOperand()->getType().getASTType(),
                    AI->getType().getASTContext().TheRawPointerType,
                    "raw-pointer-to-ref operand must be NativeObject");
  }
  
  void checkRefToBridgeObjectInst(RefToBridgeObjectInst *RI) {
    require(RI->getOperand(0)->getType().isObject(),
            "ref_to_bridge_object must convert from a value");
    require(RI->getOperand(0)->getType().getASTType()->isBridgeableObjectType(),
            "ref_to_bridge_object must convert from a heap object ref");
    requireSameType(
        RI->getBitsOperand()->getType(),
        SILType::getBuiltinWordType(F.getASTContext()),
        "ref_to_bridge_object must take a Builtin.Word bits operand");
    requireSameType(RI->getType(),
                    SILType::getBridgeObjectType(F.getASTContext()),
                    "ref_to_bridge_object must produce a BridgeObject");
  }
  
  void checkBridgeObjectToRefInst(BridgeObjectToRefInst *RI) {
    verifyLocalArchetype(RI, RI->getType().getASTType());
    requireSameType(RI->getOperand()->getType(),
                    SILType::getBridgeObjectType(F.getASTContext()),
                    "bridge_object_to_ref must take a BridgeObject");
    require(RI->getType().isObject(),
            "bridge_object_to_ref must produce a value");
    require(RI->getType().getASTType()->isBridgeableObjectType(),
            "bridge_object_to_ref must produce a heap object reference");
  }
  void checkBridgeObjectToWordInst(BridgeObjectToWordInst *RI) {
    requireSameType(RI->getOperand()->getType(),
                    SILType::getBridgeObjectType(F.getASTContext()),
                    "bridge_object_to_word must take a BridgeObject");
    require(RI->getType().isObject(),
            "bridge_object_to_word must produce a value");
    requireSameType(RI->getType(),
                    SILType::getBuiltinWordType(F.getASTContext()),
                    "bridge_object_to_word must produce a Word");
  }

  void checkConvertFunctionInst(ConvertFunctionInst *ICI) {
    auto opTI = requireObjectType(SILFunctionType, ICI->getOperand(),
                                  "convert_function operand");
    auto resTI = requireObjectType(SILFunctionType, ICI,
                                   "convert_function result");

    // convert_function is required to be an ABI-compatible conversion.
    requireABICompatibleFunctionTypes(
        opTI, resTI, "convert_function cannot change function ABI",
        *ICI->getFunction());
  }

  void checkConvertEscapeToNoEscapeInst(ConvertEscapeToNoEscapeInst *ICI) {
    auto opTI = requireObjectType(SILFunctionType, ICI->getOperand(),
                                  "convert_escape_to_noescape operand");
    auto resTI = ICI->getType().castTo<SILFunctionType>();

    // FIXME: Not yet, to be enabled when this is true.
    // require(resTI->isTrivial(F.getModule()),
    //         "convert_escape_to_noescape should produce a trivial result type");

    // convert_escape_to_noescape is required to be an ABI-compatible
    // conversion once escapability is the same on both sides.
    requireABICompatibleFunctionTypes(
        opTI, resTI->getWithExtInfo(resTI->getExtInfo().withNoEscape(false)),
        "convert_escape_to_noescape cannot change function ABI",
        *ICI->getFunction());

    // After mandatory passes convert_escape_to_noescape should not have the
    // '[not_guaranteed]' or '[escaped]' attributes.
    if (!SkipConvertEscapeToNoescapeAttributes &&
        F.getModule().getStage() != SILStage::Raw) {
      require(ICI->isLifetimeGuaranteed(),
              "convert_escape_to_noescape [not_guaranteed] not "
              "allowed after mandatory passes");
    }
  }

  void checkCondFailInst(CondFailInst *CFI) {
    requireSameType(CFI->getOperand()->getType(),
                    SILType::getBuiltinIntegerType(1, F.getASTContext()),
                    "cond_fail operand must be a Builtin.Int1");
  }

  void checkReturnInst(ReturnInst *RI) {
    LLVM_DEBUG(RI->print(llvm::dbgs()));

    SILType functionResultType =
        F.getLoweredType(F.mapTypeIntoContext(fnConv.getSILResultType(
                                                  F.getTypeExpansionContext()))
                             .getASTType())
            .getCategoryType(
                fnConv.getSILResultType(F.getTypeExpansionContext())
                    .getCategory());
    SILType instResultType = RI->getOperand()->getType();
    LLVM_DEBUG(llvm::dbgs() << "function return type: ";
               functionResultType.dump();
               llvm::dbgs() << "return inst type: ";
               instResultType.dump(););
    requireSameType(functionResultType, instResultType,
                    "return value type does not match return type of function");
  }

  void checkThrowInst(ThrowInst *TI) {
    LLVM_DEBUG(TI->print(llvm::dbgs()));

    require(fnConv.funcTy->hasErrorResult(),
            "throw in function that doesn't have an error result");

    SILType functionResultType =
        F.getLoweredType(F.mapTypeIntoContext(fnConv.getSILErrorType(
                                                  F.getTypeExpansionContext()))
                             .getASTType())
            .getCategoryType(fnConv.getSILErrorType(F.getTypeExpansionContext())
                                 .getCategory());
    SILType instResultType = TI->getOperand()->getType();
    LLVM_DEBUG(llvm::dbgs() << "function error result type: ";
               functionResultType.dump();
               llvm::dbgs() << "throw operand type: ";
               instResultType.dump(););
    requireSameType(
        functionResultType, instResultType,
        "throw operand type does not match error result type of function");
  }
  
  void checkUnwindInst(UnwindInst *UI) {
    require(F.getLoweredFunctionType()->isCoroutine(),
            "unwind in non-coroutine function");
  }

  void checkYieldInst(YieldInst *YI) {
    require(fnConv.funcTy->isCoroutine(),
            "yield in non-coroutine function");

    auto yieldedValues = YI->getYieldedValues();
    auto yieldInfos = fnConv.funcTy->getYields();
    require(yieldedValues.size() == yieldInfos.size(),
            "wrong number of yielded values for function");
    for (auto i : indices(yieldedValues)) {
      SILType yieldType = F.mapTypeIntoContext(
          fnConv.getSILType(yieldInfos[i], F.getTypeExpansionContext()));
      requireSameType(yieldedValues[i]->getType(), yieldType,
                      "yielded value does not match yield type of coroutine");
    }

    // We require the resume and unwind destinations to be unique in order
    // to prevent either edge from becoming critical.
    require(YI->getResumeBB()->getSinglePredecessorBlock(),
            "resume dest of 'yield' must be uniquely used");
    require(YI->getUnwindBB()->getSinglePredecessorBlock(),
            "unwind dest of 'yield' must be uniquely used");
  }

  void checkSelectEnumCases(SelectEnumOperation SEO) {
    EnumDecl *eDecl =
        SEO.getEnumOperand()->getType().getEnumOrBoundGenericEnum();
    require(eDecl, "select_enum operand must be an enum");

    // Find the set of enum elements for the type so we can verify
    // exhaustiveness.
    llvm::DenseSet<EnumElementDecl*> unswitchedElts;
    eDecl->getAllElements(unswitchedElts);

    // Verify the set of enum cases we dispatch on.
    for (unsigned i = 0, e = SEO.getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILValue result;
      std::tie(elt, result) = SEO.getCase(i);

      require(elt->getDeclContext() == eDecl,
              "select_enum dispatches on enum element that is not part of "
              "its type");
      require(unswitchedElts.count(elt),
              "select_enum dispatches on same enum element more than once");
      unswitchedElts.erase(elt);

      // The result value must match the type of the instruction.
      requireSameType(
          result->getType(), SEO->getType(),
          "select_enum case operand must match type of instruction");
    }

    // If the select is non-exhaustive, we require a default.
    bool isExhaustive =
        eDecl->isEffectivelyExhaustive(F.getModule().getSwiftModule(),
                                       F.getResilienceExpansion());
    require((isExhaustive && unswitchedElts.empty()) || SEO.hasDefault(),
            "nonexhaustive select_:enum must have a default destination");
    if (SEO.hasDefault()) {
      requireSameType(
          SEO.getDefaultResult()->getType(), SEO->getType(),
          "select_enum default operand must match type of instruction");
    }
  }

  void checkSelectEnumInst(SelectEnumInst *SEI) {
    require(SEI->getEnumOperand()->getType().isObject(),
            "select_enum operand must be an object");

    checkSelectEnumCases(SelectEnumOperation(SEI));
  }
  void checkSelectEnumAddrInst(SelectEnumAddrInst *SEI) {
    require(SEI->getEnumOperand()->getType().isAddress(),
            "select_enum_addr operand must be an address");

    checkSelectEnumCases(SelectEnumOperation(SEI));
  }

  void checkSwitchValueInst(SwitchValueInst *SVI) {
    // TODO: Type should be either integer or function
    auto Ty = SVI->getOperand()->getType();
    require(Ty.is<BuiltinIntegerType>() || Ty.is<SILFunctionType>(),
            "switch_value operand should be either of an integer "
            "or function type");

    auto ult = [](const SILValue &a, const SILValue &b) { 
      return a == b || a < b; 
    };

    std::set<SILValue, decltype(ult)> cases(ult);

    for (unsigned i = 0, e = SVI->getNumCases(); i < e; ++i) {
      SILValue value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SVI->getCase(i);

      requireSameType(
          value->getType(), Ty,
          "switch_value case value should have the same type as its operand");

      require(!cases.count(value),
              "multiple switch_value cases for same value");
      cases.insert(value);

      require(dest->args_empty(),
              "switch_value case destination cannot take arguments");
    }

    if (SVI->hasDefault())
      require(SVI->getDefaultBB()->args_empty(),
              "switch_value default destination cannot take arguments");
  }

  void checkSwitchEnumInst(SwitchEnumInst *switchEnum) {
    require(switchEnum->getOperand()->getType().isObject(),
            "switch_enum operand must be an object");

    SILType uTy = switchEnum->getOperand()->getType();
    EnumDecl *uDecl = uTy.getEnumOrBoundGenericEnum();
    require(uDecl, "switch_enum operand is not an enum");

    // Find the set of enum elements for the type so we can verify
    // exhaustiveness.
    llvm::DenseSet<EnumElementDecl*> unswitchedElts;

    auto checkSwitchCase = [&](EnumElementDecl *elt, SILBasicBlock *dest) {
      require(elt->getDeclContext() == uDecl,
              "switch_enum dispatches on enum element that is not part of "
              "its type");
      require(unswitchedElts.insert(elt).second,
              "switch_enum dispatches on same enum element more than once");

      if (!elt->hasAssociatedValues()) {
        require(dest->getArguments().empty(),
                "switch_enum destination for no-argument case must take no "
                "arguments");
        return;
      }
      // Check for a valid switch result type.
      if (dest->getArguments().size() == 1) {
        SILType eltArgTy = uTy.getEnumElementType(elt, F.getModule(),
                                                  F.getTypeExpansionContext());
        SILType bbArgTy = dest->getArguments()[0]->getType();
        if (F.getModule().getStage() != SILStage::Lowered) {
          // During the lowered stage, a function type might have different
          // signature
          //
          // We allow for move only wrapped enums to have trivial payloads that
          // are not move only wrapped. This occurs since we want to lower
          // trivial move only wrapped types earlier in the pipeline than
          // non-trivial types.
          if (bbArgTy.isTrivial(F)) {
            require(eltArgTy == bbArgTy.copyingMoveOnlyWrapper(eltArgTy),
                    "switch_enum destination bbarg must match case arg type");
          } else {
            require(eltArgTy == bbArgTy,
                    "switch_enum destination bbarg must match case arg type");
          }
        }
        require(!dest->getArguments()[0]->getType().isAddress(),
                "switch_enum destination bbarg type must not be an address");
      }
      if (!isSILOwnershipEnabled() || !F.hasOwnership()) {
        // In non-OSSA, the destBB can optionally ignore the payload.
        require(dest->getArguments().empty()
                    || dest->getArguments().size() == 1,
                "switch_enum destination for case w/ args must take 0 or 1 "
                "arguments");
        return;
      }
      checkForwardedTermResult(switchEnum, dest);
    };
    // Verify the set of enum cases we dispatch on.
    for (unsigned i = 0, e = switchEnum->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = switchEnum->getCase(i);
      checkSwitchCase(elt, dest);
    }
    // If the switch is non-exhaustive, we require a default.
    if (!switchEnum->hasDefault()) {
      bool isExhaustive = uDecl->isEffectivelyExhaustive(
          F.getModule().getSwiftModule(), F.getResilienceExpansion());
      require(isExhaustive
                  && (unswitchedElts.size() == uDecl->getNumElements()),
              "nonexhaustive switch_enum must have a default destination");
      return;
    }
    auto *defaultBB = switchEnum->getDefaultBB();
    if (!isSILOwnershipEnabled() || !F.hasOwnership()) {
      require(switchEnum->getDefaultBB()->args_empty(),
              "switch_enum default destination must take no arguments");
      return;
    }
    // When the switch has a unique default case, the OSSA result has the same
    // requirements as a matched result.
    if (NullablePtr<EnumElementDecl> uniqueCase =
            switchEnum->getUniqueCaseForDefault()) {
      checkSwitchCase(uniqueCase.get(), defaultBB);
      return;
    }
    // With no unique case, the switch_enum operand is simply forwarded.
    require(defaultBB->getNumArguments() == 1,
            "Switch enum default block should have one argument");
    requireSameType(
        defaultBB->getArgument(0)->getType(),
        switchEnum->getOperand()->getType(),
        "Switch enum default block should have one argument that is "
        "the same as the input type");
    require(defaultBB->getArgument(0)->getOwnershipKind()
                == switchEnum->getForwardingOwnershipKind(),
            "switch_enum non-trivial destination arg must have the same "
            "ownership as switch_enum's operand");
  }

  void checkSwitchEnumAddrInst(SwitchEnumAddrInst *SOI) {
    require(SOI->getOperand()->getType().isAddress(),
            "switch_enum_addr operand must be an address");

    SILType uTy = SOI->getOperand()->getType();
    EnumDecl *uDecl = uTy.getEnumOrBoundGenericEnum();
    require(uDecl, "switch_enum_addr operand must be an enum");

    // Find the set of enum elements for the type so we can verify
    // exhaustiveness.
    llvm::DenseSet<EnumElementDecl*> unswitchedElts;
    uDecl->getAllElements(unswitchedElts);

    // Verify the set of enum cases we dispatch on.
    for (unsigned i = 0, e = SOI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI->getCase(i);

      require(elt->getDeclContext() == uDecl,
              "switch_enum_addr dispatches on enum element that "
              "is not part of its type");
      require(unswitchedElts.count(elt),
              "switch_enum_addr dispatches on same enum element "
              "more than once");
      unswitchedElts.erase(elt);

      // The destination BB must not have BB arguments.
      require(dest->getArguments().empty(),
              "switch_enum_addr destination must take no BB args");
    }

    // If the switch is non-exhaustive, we require a default.
    bool isExhaustive =
        uDecl->isEffectivelyExhaustive(F.getModule().getSwiftModule(),
                                       F.getResilienceExpansion());
    require((isExhaustive && unswitchedElts.empty()) || SOI->hasDefault(),
            "nonexhaustive switch_enum_addr must have a default destination");

    if (SOI->hasDefault())
      require(SOI->getDefaultBB()->args_empty(),
              "switch_enum_addr default destination must take "
              "no arguments");
  }

  bool verifyBranchArgs(SILValue branchArg, SILArgument *bbArg) {
    // NOTE: IRGen currently does not support the following method_inst
    // variants as branch arguments.
    // Once this is supported, the check can be removed.
    require(!(isa<MethodInst>(branchArg) &&
              cast<MethodInst>(branchArg)->getMember().isForeign),
        "branch argument cannot be a witness_method or an objc method_inst");
    require(!(branchArg->getType().is<SILFunctionType>() &&
              branchArg->getType()
                      .castTo<SILFunctionType>()
                      ->getExtInfo()
                      .getRepresentation() ==
                  SILFunctionTypeRepresentation::ObjCMethod),
            "branch argument cannot be a objective-c method");
    return branchArg->getType() == bbArg->getType();
  }

  void checkBranchInst(BranchInst *BI) {
    require(BI->getArgs().size() == BI->getDestBB()->args_size(),
            "branch has wrong number of arguments for dest bb");
    require(std::equal(BI->getArgs().begin(), BI->getArgs().end(),
                       BI->getDestBB()->args_begin(),
                       [&](SILValue branchArg, SILArgument *bbArg) {
                         return verifyBranchArgs(branchArg, bbArg);
                       }),
            "branch argument types do not match arguments for dest bb");
  }

  void checkCondBranchInst(CondBranchInst *cbi) {
    // It is important that cond_br keeps an i1 type. ARC Sequence Opts assumes
    // that cond_br does not use reference counted values or decrement reference
    // counted values under the assumption that the instruction that computes
    // the i1 is the use/decrement that ARC cares about and that after that
    // instruction is evaluated, the scalar i1 has a different identity and the
    // object can be deallocated.
    requireSameType(cbi->getCondition()->getType(),
                    SILType::getBuiltinIntegerType(
                        1, cbi->getCondition()->getType().getASTContext()),
                    "condition of conditional branch must have Int1 type");

    require(cbi->getTrueArgs().size() == cbi->getTrueBB()->args_size(),
            "true branch has wrong number of arguments for dest bb");
    require(cbi->getTrueBB() != cbi->getFalseBB(), "identical destinations");
    require(std::equal(cbi->getTrueArgs().begin(), cbi->getTrueArgs().end(),
                       cbi->getTrueBB()->args_begin(),
                       [&](SILValue branchArg, SILArgument *bbArg) {
                         return verifyBranchArgs(branchArg, bbArg);
                       }),
            "true branch argument types do not match arguments for dest bb");

    require(cbi->getFalseArgs().size() == cbi->getFalseBB()->args_size(),
            "false branch has wrong number of arguments for dest bb");
    require(std::equal(cbi->getFalseArgs().begin(), cbi->getFalseArgs().end(),
                       cbi->getFalseBB()->args_begin(),
                       [&](SILValue branchArg, SILArgument *bbArg) {
                         return verifyBranchArgs(branchArg, bbArg);
                       }),
            "false branch argument types do not match arguments for dest bb");
    // When we are in ossa, cond_br can not have any arguments that are
    // non-trivial.
    if (!F.hasOwnership())
      return;

    require(llvm::all_of(cbi->getOperandValues(),
                         [&](SILValue v) -> bool {
                           return v->getType().isTrivial(*cbi->getFunction());
                         }),
            "cond_br must not have a non-trivial value in ossa.");
  }

  void checkDynamicMethodBranchInst(DynamicMethodBranchInst *DMBI) {
    SILType operandType = DMBI->getOperand()->getType();

    require(DMBI->getMember().getDecl()->isObjC(), "method must be @objc");
    if (!DMBI->getMember().getDecl()->isInstanceMember()) {
      require(operandType.is<MetatypeType>(),
              "operand must have metatype type");
      require(operandType.castTo<MetatypeType>()
                ->getInstanceType()->mayHaveSuperclass(),
              "operand must have metatype of class or class-bound type");
    }

    // Check that the branch argument is of the expected dynamic method type.
    require(DMBI->getHasMethodBB()->args_size() == 1,
            "true bb for dynamic_method_br must take an argument");

    auto bbArgTy = DMBI->getHasMethodBB()->args_begin()[0]->getType();
    require(verifyDynamicMethodType(cast<SILFunctionType>(bbArgTy.getASTType()),
                                    operandType, DMBI->getMember()),
            "bb argument for dynamic_method_br must be of the method's type");
  }

  void checkProjectBlockStorageInst(ProjectBlockStorageInst *PBSI) {
    require(PBSI->getOperand()->getType().isAddress(),
            "operand must be an address");
    auto storageTy = PBSI->getOperand()->getType().getAs<SILBlockStorageType>();
    require(storageTy, "operand must be a @block_storage type");
    
    require(PBSI->getType().isAddress(),
            "result must be an address");
    auto captureTy = PBSI->getType().getASTType();
    requireSameType(
        storageTy->getCaptureType(), captureTy,
        "result must be the capture type of the @block_storage type");
  }
  
  void checkInitBlockStorageHeaderInst(InitBlockStorageHeaderInst *IBSHI) {
    auto storage = IBSHI->getBlockStorage();
    require(storage->getType().isAddress(),
            "block storage operand must be an address");

    auto storageTy = storage->getType().getAs<SILBlockStorageType>();
    require(storageTy, "block storage operand must be a @block_storage type");

    auto captureTy = storageTy->getCaptureType();
    if (auto capturedFnTy = captureTy->getAs<SILFunctionType>()) {
      if (capturedFnTy->isNoEscape()) {
        // If the capture is a noescape function then it must be possible to
        // locally determine the value stored to initialize the storage for the
        // capture. This is required to diagnose static exclusivity violations
        // when a noescape closure is converted to a noescape block that
        // is then passed to a function.
        auto *storageProjection =
           storage->getSingleUserOfType<ProjectBlockStorageInst>();
        require(storageProjection,
                "block storage operand with noescape capture must have "
                "projection from block");

        auto *storeInst = storageProjection->getSingleUserOfType<StoreInst>();
        require(storeInst,
                "block storage operand with noescape capture must have "
                "store to projection");
      }
    }

    require(IBSHI->getInvokeFunction()->getType().isObject(),
            "invoke function operand must be a value");
    auto invokeTy
      = IBSHI->getInvokeFunction()->getType().getAs<SILFunctionType>();
    require(invokeTy, "invoke function operand must be a function");
    require(invokeTy->getRepresentation()
              == SILFunctionType::Representation::CFunctionPointer,
            "invoke function operand must be a c function");
    require(invokeTy->getParameters().size() >= 1,
            "invoke function must take at least one parameter");
    require(!invokeTy->getInvocationGenericSignature() ||
            invokeTy->getExtInfo().isPseudogeneric(),
            "invoke function must not take reified generic parameters");
    
    invokeTy = checkApplySubstitutions(IBSHI->getSubstitutions(),
                                    SILType::getPrimitiveObjectType(invokeTy));
    
    auto storageParam = invokeTy->getParameters()[0];
    require(storageParam.getConvention() ==
            ParameterConvention::Indirect_InoutAliasable,
            "invoke function must take block storage as @inout_aliasable "
            "parameter");
    requireSameType(
        storageParam.getArgumentType(F.getModule(), invokeTy,
                                     F.getTypeExpansionContext()),
        storageTy,
        "invoke function must take block storage type as first parameter");

    require(IBSHI->getType().isObject(), "result must be a value");
    auto blockTy = IBSHI->getType().getAs<SILFunctionType>();
    require(blockTy, "result must be a function");
    require(blockTy->getRepresentation() == SILFunctionType::Representation::Block,
            "result must be a cdecl block function");
    require(blockTy->getResults() == invokeTy->getResults(),
            "result must have same results as invoke function");

    require(blockTy->getParameters().size() + 1
              == invokeTy->getParameters().size(),
          "result must match all parameters of invoke function but the first");
    auto blockParams = blockTy->getParameters();
    auto invokeBlockParams = invokeTy->getParameters().slice(1);
    for (unsigned i : indices(blockParams)) {
      require(blockParams[i] == invokeBlockParams[i],
          "result must match all parameters of invoke function but the first");
    }
  }

  void checkHopToExecutorInst(HopToExecutorInst *HI) {
    auto executor = HI->getTargetExecutor();
    if (HI->getModule().getStage() == SILStage::Lowered) {
      requireOptionalExecutorType(executor,
                                  "hop_to_executor operand in lowered SIL");
    }
  }

  void checkExtractExecutorInst(ExtractExecutorInst *EEI) {
    if (EEI->getModule().getStage() == SILStage::Lowered) {
      require(false,
              "extract_executor instruction should have been lowered away");
    }
  }

  void checkObjCProtocolInst(ObjCProtocolInst *OPI) {
    require(OPI->getProtocol()->isObjC(),
            "objc_protocol must be applied to an @objc protocol");
    auto classTy = OPI->getType();
    require(classTy.isObject(), "objc_protocol must produce a value");
    auto classDecl = classTy.getClassOrBoundGenericClass();
    require(classDecl, "objc_protocol must produce a class instance");
    require(classDecl->getName() == F.getASTContext().Id_Protocol,
            "objc_protocol must produce an instance of ObjectiveC.Protocol class");
    require(classDecl->getModuleContext()->getName() == F.getASTContext().Id_ObjectiveC,
            "objc_protocol must produce an instance of ObjectiveC.Protocol class");
  }
  
  void checkObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *OMOI) {
    require(OMOI->getOperand()->getType().isObject(),
            "objc_metatype_to_object must take a value");
    auto fromMetaTy = OMOI->getOperand()->getType().getAs<MetatypeType>();
    require(fromMetaTy, "objc_metatype_to_object must take an @objc metatype value");
    require(fromMetaTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "objc_metatype_to_object must take an @objc metatype value");
    require(OMOI->getType().isObject(),
            "objc_metatype_to_object must produce a value");
    require(OMOI->getType().getASTType()->isAnyObject(),
            "objc_metatype_to_object must produce an AnyObject value");
  }

  void checkObjCExistentialMetatypeToObjectInst(
                                    ObjCExistentialMetatypeToObjectInst *OMOI) {
    require(OMOI->getOperand()->getType().isObject(),
            "objc_metatype_to_object must take a value");
    auto fromMetaTy = OMOI->getOperand()->getType()
      .getAs<ExistentialMetatypeType>();
    require(fromMetaTy, "objc_metatype_to_object must take an @objc existential metatype value");
    require(fromMetaTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "objc_metatype_to_object must take an @objc existential metatype value");
    require(OMOI->getType().isObject(),
            "objc_metatype_to_object must produce a value");
    require(OMOI->getType().getASTType()->isAnyObject(),
            "objc_metatype_to_object must produce an AnyObject value");
  }
  
  void checkKeyPathInst(KeyPathInst *KPI) {
    auto kpTy = KPI->getType();
    
    require(kpTy.isObject(), "keypath result must be an object type");
    
    auto kpBGT = kpTy.getAs<BoundGenericType>();
    require(kpBGT, "keypath result must be a generic type");
    require(kpBGT->isKeyPath() ||
            kpBGT->isWritableKeyPath() ||
            kpBGT->isReferenceWritableKeyPath(),
            "keypath result must be a key path type");
    
    auto baseTy = CanType(kpBGT->getGenericArgs()[0]);
    auto pattern = KPI->getPattern();
    SubstitutionMap patternSubs = KPI->getSubstitutions();
    requireSameType(
        F.getLoweredType(baseTy).getASTType(),
        F.getLoweredType(
            pattern->getRootType().subst(patternSubs)->getCanonicalType()).getASTType(),
        "keypath root type should match root type of keypath pattern");

    auto leafTy = CanType(kpBGT->getGenericArgs()[1]);
    requireSameType(
        F.getLoweredType(leafTy).getASTType(),
        F.getLoweredType(
             pattern->getValueType().subst(patternSubs)->getCanonicalType())
            .getASTType(),
        "keypath value type should match value type of keypath pattern");

    {
      for (auto &component : pattern->getComponents()) {
        bool hasIndices;
        switch (component.getKind()) {
        case KeyPathPatternComponent::Kind::GettableProperty:
        case KeyPathPatternComponent::Kind::SettableProperty:
          hasIndices = !component.getSubscriptIndices().empty();
          break;
        
        case KeyPathPatternComponent::Kind::StoredProperty:
        case KeyPathPatternComponent::Kind::OptionalChain:
        case KeyPathPatternComponent::Kind::OptionalWrap:
        case KeyPathPatternComponent::Kind::OptionalForce:
        case KeyPathPatternComponent::Kind::TupleElement:
          hasIndices = false;
          break;
        }
      
        verifyKeyPathComponent(F.getModule(), F.getTypeExpansionContext(),
          [&](bool reqt, StringRef message) { _require(reqt, message); },
          baseTy,
          leafTy,
          component,
          KPI->getPatternOperands(),
          KPI->getPattern()->getGenericSignature(),
          KPI->getSubstitutions(),
          /*property descriptor*/false,
          hasIndices);
      }
    }
    requireSameType(
        F.getLoweredType(CanType(baseTy)).getASTType(),
        F.getLoweredType(CanType(leafTy)).getASTType(),
        "final component should match leaf value type of key path type");
  }

  void checkIsEscapingClosureInst(IsEscapingClosureInst *IEC) {
    // The closure operand is allowed to be an optional closure.
    auto operandType = IEC->getOperand()->getType();
    if (operandType.getOptionalObjectType())
      operandType = operandType.getOptionalObjectType();

    auto fnType = operandType.getAs<SILFunctionType>();
    require(fnType && fnType->getExtInfo().hasContext() &&
                !fnType->isNoEscape() &&
                fnType->getExtInfo().getRepresentation() ==
                    SILFunctionTypeRepresentation::Thick,
            "is_escaping_closure must have a thick "
            "function operand");
    require(IEC->getVerificationType() == IsEscapingClosureInst::ObjCEscaping ||
                IEC->getVerificationType() ==
                    IsEscapingClosureInst::WithoutActuallyEscaping,
            "unknown verification type");
  }

  void checkDifferentiableFunctionInst(DifferentiableFunctionInst *dfi) {
    // FIXME(TF-1197): Re-enable verification after substituted SIL function
    // types.
    return;
#if 0
    auto origTy =
        dfi->getOriginalFunction()->getType().getAs<SILFunctionType>();
    require(origTy, "The original function must have a function type");
    require(!origTy->isDifferentiable(),
            "The original function must not be @differentiable");
    // Skip verification in lowered SIL: LoadableByAddress changes
    // parameter/result conventions.
    // TODO: Check that derivative function types match excluding
    // parameter/result conventions in lowered SIL.
    if (F.getModule().getStage() == SILStage::Lowered)
      return;
    if (dfi->hasDerivativeFunctions()) {
      auto jvp = dfi->getJVPFunction();
      auto jvpType = jvp->getType().getAs<SILFunctionType>();
      require(jvpType, "The JVP function must have a function type");
      require(!jvpType->isDifferentiable(),
              "The JVP function must not be @differentiable");
      auto expectedJVPType = origTy->getAutoDiffDerivativeFunctionType(
          dfi->getParameterIndices(), dfi->getResultIndices(),
          AutoDiffDerivativeFunctionKind::JVP, TC,
          LookUpConformanceInModule(M));
      requireSameType(SILType::getPrimitiveObjectType(jvpType),
                      SILType::getPrimitiveObjectType(expectedJVPType),
                      "JVP type does not match expected JVP type");
      auto vjp = dfi->getVJPFunction();
      auto vjpType = vjp->getType().getAs<SILFunctionType>();
      require(vjpType, "The VJP function must have a function type");
      require(!vjpType->isDifferentiable(),
              "The VJP function must not be @differentiable");
      auto expectedVJPType = origTy->getAutoDiffDerivativeFunctionType(
          dfi->getParameterIndices(), dfi->getResultIndices(),
          AutoDiffDerivativeFunctionKind::VJP, TC,
          LookUpConformanceInModule(M));
      requireSameType(SILType::getPrimitiveObjectType(vjpType),
                      SILType::getPrimitiveObjectType(expectedVJPType),
                      "VJP type does not match expected VJP type");
    }
#endif
  }

  void checkLinearFunctionInst(LinearFunctionInst *lfi) {
    auto origTy =
        lfi->getOriginalFunction()->getType().getAs<SILFunctionType>();
    require(origTy, "The original function must have a function type");
    require(!origTy->isDifferentiable(),
            "The original function must not be differentiable");
    // Skip lowered SIL: LoadableByAddress changes parameter/result conventions.
    // TODO: Check that transpose function type matches excluding
    // parameter/result conventions in lowered SIL.
    if (F.getModule().getStage() == SILStage::Lowered)
      return;
    if (lfi->hasTransposeFunction()) {
      auto transpose = lfi->getTransposeFunction();
      auto transposeType = transpose->getType().getAs<SILFunctionType>();
      require(transposeType,
              "The transpose function must have a function type");
      require(!transposeType->isDifferentiable(),
              "The transpose function must not be differentiable");
      auto expectedTransposeType = origTy->getAutoDiffTransposeFunctionType(
          lfi->getParameterIndices(), TC, LookUpConformanceInModule(M));
      // TODO: Consider tightening verification. This requires changes to
      // `SILFunctionType::getAutoDiffTransposeFunctionType`.
      requireSameType(
          SILType::getPrimitiveObjectType(
              transposeType->getUnsubstitutedType(F.getModule())),
          SILType::getPrimitiveObjectType(
              expectedTransposeType->getUnsubstitutedType(F.getModule())),
          "Transpose type does not match expected transpose type");
    }
  }

  void checkDifferentiableFunctionExtractInst(
      DifferentiableFunctionExtractInst *dfei) {
    auto fnTy = dfei->getOperand()->getType().getAs<SILFunctionType>();
    require(fnTy, "The function operand must have a function type");
    // TODO: Ban 'Normal' and 'Forward'.
    require(
        fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Reverse ||
        fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Normal ||
        fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Forward,
        "The function operand must be a '@differentiable(reverse)' function");
  }

  void checkLinearFunctionExtractInst(LinearFunctionExtractInst *lfei) {
    auto fnTy = lfei->getOperand()->getType().getAs<SILFunctionType>();
    require(fnTy, "The function operand must have a function type");
    require(fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Linear,
            "The function operand must be a '@differentiable(_linear)' "
            "function");
  }

  void checkDifferentiabilityWitnessFunctionInst(
      DifferentiabilityWitnessFunctionInst *dwfi) {
    auto witnessFnTy = dwfi->getType().castTo<SILFunctionType>();
    auto *witness = dwfi->getWitness();
    // `DifferentiabilityWitnessFunctionInst` constructor asserts that
    // `witness` is non-null.
    auto witnessKind = dwfi->getWitnessKind();
    // Return if not witnessing a derivative function.
    auto derivKind = witnessKind.getAsDerivativeFunctionKind();
    if (!derivKind)
      return;
    // Return if witness does not define the referenced derivative.
    auto *derivativeFn = witness->getDerivative(*derivKind);
    if (!derivativeFn)
      return;
    auto derivativeFnTy = derivativeFn->getLoweredFunctionType();
    requireSameType(SILType::getPrimitiveObjectType(witnessFnTy),
                    SILType::getPrimitiveObjectType(derivativeFnTy),
                    "Type of witness instruction does not match actual type of "
                    "witnessed function");
  }
  
  void checkGetAsyncContinuationInstBase(GetAsyncContinuationInstBase *GACI) {
    auto resultTy = GACI->getType();
    require(resultTy.is<BuiltinRawUnsafeContinuationType>(),
            "Instruction type must be a continuation type");
  }
  
  void checkGetAsyncContinuationInst(GetAsyncContinuationInst *GACI) {
    checkGetAsyncContinuationInstBase(GACI);
  }
  
  void checkGetAsyncContinuationAddrInst(GetAsyncContinuationAddrInst *GACI) {
    checkGetAsyncContinuationInstBase(GACI);

    requireSameType(GACI->getOperand()->getType(),
                    GACI->getLoweredResumeType().getAddressType(),
                    "Operand type must match continuation resume type");
  }
  
  void checkAwaitAsyncContinuationInst(AwaitAsyncContinuationInst *AACI) {
    // The operand must be a GetAsyncContinuation* instruction.
    auto cont = dyn_cast<GetAsyncContinuationInstBase>(AACI->getOperand());
    require(cont, "can only await the result of a get_async_continuation instruction");
    bool isAddressForm = isa<GetAsyncContinuationAddrInst>(cont);

    auto &C = cont->getType().getASTContext();
    
    // The shape of the successors depends on the continuation instruction being
    // awaited.
    require((bool)AACI->getErrorBB() == cont->throws(),
            "must have an error successor if and only if the continuation is throwing");
    if (cont->throws()) {
      require(AACI->getErrorBB()->getNumArguments() == 1,
              "error successor must take one argument");
      auto arg = AACI->getErrorBB()->getArgument(0);
      auto errorType = C.getErrorExistentialType();
      requireSameType(arg->getType(),
                      SILType::getPrimitiveObjectType(errorType),
              "error successor argument must have Error type");
      
      if (AACI->getFunction()->hasOwnership()) {
        require(arg->getOwnershipKind() == OwnershipKind::Owned,
                "error successor argument must be owned");
      }
    }
    if (isAddressForm) {
      require(AACI->getResumeBB()->getNumArguments() == 0,
              "resume successor must take no arguments for get_async_continuation_addr");
    } else {
      require(AACI->getResumeBB()->getNumArguments() == 1,
              "resume successor must take one argument for get_async_continuation");
      auto arg = AACI->getResumeBB()->getArgument(0);

      requireSameType(arg->getType(), cont->getLoweredResumeType(),
                      "resume successor must take an argument of the continuation resume type");
      if (AACI->getFunction()->hasOwnership()) {
        require(arg->getOwnershipKind() == OwnershipKind::Owned,
                "resume successor argument must be owned");
      }
    }
  }

  void verifySameShape(CanPackType left, CanPackType right) {
    verifySameShape(left, right, 0, right->getNumElements());
  }
  void verifySameShape(CanPackType left, CanPackType right,
                       unsigned rightBegin, unsigned rightEnd) {
    auto rightElements = right.getElementTypes();
    require(rightBegin <= rightEnd && rightEnd <= rightElements.size(),
            "slice out of range");
    _verifySameShape(left.getElementTypes(),
                     rightElements.slice(rightBegin, rightEnd - rightBegin));
  }
  void verifySameShape(CanPackType left, ArrayRef<CanType> right) {
    _verifySameShape(left.getElementTypes(), right);
  }
  template <class LeftArray, class RightArray>
  void _verifySameShape(LeftArray left, RightArray right) {
    require(left.size() == right.size(), "packs must agree in length");

    for (size_t i : indices(left)) {
      auto leftExpansion = dyn_cast<PackExpansionType>(left[i]);
      auto rightExpansion = dyn_cast<PackExpansionType>(right[i]);
      if (leftExpansion && rightExpansion) {
        require(leftExpansion.getCountType()->getReducedShape() ==
                rightExpansion.getCountType()->getReducedShape(),
                "packs must have same shape: corresponding expansion "
                "components must expand packs of same shape");
      } else {
        require(!leftExpansion && !rightExpansion,
                "packs must have same shape: must agree in whether "
                "corresponding components are expansions");
      }
    }
  }

  /// Given that we're indexing into the given pack, verify that the
  /// element type is a valid type for the element at the given index.
  void verifyPackElementType(CanSILPackType packType,
                             AnyPackIndexInst *packIndex,
                             SILType elementType) {
    require(elementType.isAddress() == packType->isElementAddress(),
            "pack element address-ness must match pack");

    verifyPackElementType(packType->getElementTypes(),
                          packIndex,
                          elementType.getASTType(),
                          /*SILType*/ true);
  }

  /// Verify that the element type is the right type for a particular
  /// index of a pack with the given components.  This implements the
  /// structural type matching for pack indexing algorithm described
  /// in the specification for the SIL pack indexing instructions.
  void verifyPackElementType(ArrayRef<CanType> indexedPack,
                             AnyPackIndexInst *packIndex,
                             CanType targetElementType,
                            bool typesAreSILTypes) {
    verifySameShape(packIndex->getIndexedPackType(), indexedPack);

    if (auto spi = dyn_cast<ScalarPackIndexInst>(packIndex)) {
      requireSameType(targetElementType,
                      indexedPack[spi->getComponentIndex()],
                      "scalar pack index must match exactly");
    } else if (auto ppi = dyn_cast<PackPackIndexInst>(packIndex)) {
      auto start = ppi->getComponentStartIndex();
      auto end = ppi->getComponentEndIndex();
      verifyPackElementType(indexedPack.slice(start, end - start),
                            ppi->getSliceIndexOperand(),
                            targetElementType,
                            typesAreSILTypes);
    } else {
      auto dpi = cast<DynamicPackIndexInst>(packIndex);
      verifyDynamicPackIndexStructuralEquality(indexedPack, dpi,
                                               targetElementType,
                                               typesAreSILTypes);
    }
  }

  /// Collect the opened element archetypes in the named type that
  /// are opened by an instruction using the given pack-indexing
  /// instruction.
  llvm::DenseMap<CanType, CanPackType>
  collectOpenedElementArchetypeBindings(CanType type,
                                        AnyPackIndexInst *indexedBy) {
    llvm::DenseMap<CanType, CanPackType> result;

    type.visit([&](CanType type) {
      auto opened = dyn_cast<ElementArchetypeType>(type);
      if (!opened) return;
      opened = opened.getRoot();

      // Don't repeat this work if the same archetype is named twice.
      if (result.count(opened)) return;

      // Ignore archetypes defined by open_pack_elements not based on the
      // same pack_index instruction.
      auto openingInst =
        F.getModule().getRootLocalArchetypeDef(opened,
                                               const_cast<SILFunction*>(&F));
      auto opi = dyn_cast<OpenPackElementInst>(openingInst);
      if (!opi || opi->getIndexOperand() != indexedBy) return;

      // Map each root opened element archetype to its pack substitution.
      // FIXME: remember conformances?
      auto openedEnv = opi->getOpenedGenericEnvironment();
      openedEnv->forEachPackElementBinding(
          [&](ElementArchetypeType *elementArchetype, PackType *substitution) {
        auto subPack = cast<PackType>(substitution->getCanonicalType());
        result.insert({elementArchetype->getCanonicalType(), subPack});
      });
    });

    return result;
  }

  /// Verify that the lowered element type is a valid type for a
  /// particular dynamic_pack_index into a pack operand with the given
  /// components.
  ///
  /// This implements part of the structural type matching algorithm
  /// for pack indexing:
  ///
  /// Let S be the set of opened pack element archetypes in the element
  /// type that were opened by open_pack_element instructions based on
  /// the same dynamic_pack_index instruction.  By construction,
  /// the pack substitutions given to open_pack_element for the opened
  /// type parameter packs must all have the same shape as the indexed
  /// pack type of the open_pack_element's index operand.  That index
  /// operand is the given dynamic_pack_index instruction, which is being
  /// used to index into a pack with the given pack components, so the
  /// components must have the same shape as the pack substitutions.
  /// The lowered element type is a valid type for this index if, for
  /// each component of this shape, there is a substitution which
  /// (optionally) replaces archetypes in S with the correponding
  /// component type (pattern types for expansions) of the pack
  /// substitution to get the corresponding component type (pattern
  /// type for expansions) of the pack operand.
  ///
  /// That is, suppose we have:
  ///   open_pack_element %index of <each P0, each P1 where (P0,P1):Any>
  ///                            at <Pack{A0, repeat each B0, C0},
  ///                                Pack{A1, repeat each B1, C1}>,
  ///                            shape $P0, uuid "01234"
  ///
  /// And suppose we're indexing into this pack (recalling that the
  /// pack shape rules require this to have the same shape as the pack
  /// substitutions):
  ///   $Pack{Ap, repeat each Bp, Cp},
  ///
  /// Finally, suppose that the expected element type of this index is E,
  /// a type expression in terms of @pack_element("01234") P0 and
  /// @pack_element("01234") P1.
  ///
  /// Then applying this substitution to E:
  ///   @pack_element("01234") P0   =>   A0
  ///   @pack_element("01234") P1   =>   A1
  /// should yield the type expression Ap, and so on for each component
  /// of the shape.
  void verifyDynamicPackIndexStructuralEquality(
                                          ArrayRef<CanType> indexedPack,
                                          DynamicPackIndexInst *dpi,
                                          CanType targetElementType,
                                          bool typesAreSILTypes) {
    // If there are no pack components, this code must be unreachable.
    if (indexedPack.empty()) return;

    // Collect the set S of opened pack archetypes based on the given
    // pack index instruction, mapping them to their pack substitution
    // types.
    auto allOpened =
      collectOpenedElementArchetypeBindings(targetElementType, dpi);

    // Expand each of the pack components.
    for (unsigned componentIndex : indices(indexedPack)) {
      CanType indexedElementType = indexedPack[componentIndex];
      CanType indexedShape;
      if (auto exp = dyn_cast<PackExpansionType>(indexedElementType)) {
        indexedShape = exp.getCountType();
        indexedElementType = exp.getPatternType();
      }

      // If we have an exact match without substitution, that's great.
      if (targetElementType == indexedElementType) continue;

      // If we don't have any substitutions, this must be a case where the
      // expansion is invariant to the archetypes.
      if (allOpened.empty()) {
        // This condition is always false.
        requireSameType(targetElementType, indexedElementType,
                        "no opened archetypes based on a matching pack index "
                        "instruction; element type must be invariant");
        continue;
      }

      // Otherwise, we expect lanewise substitution to turn the expected
      // element type into the lanewise component of the original pack.

      // Provide substitution functions that replace the pack archetypes
      // we found above with the corresponding lane of the pack substitution.
      auto substTypes = [&](SubstitutableType *type) -> Type {
        auto archetype = dyn_cast<ElementArchetypeType>(type);
        if (!archetype)
          return type;
        if (!archetype->isRoot())
          return Type();

        auto it = allOpened.find(type->getCanonicalType());
        assert(it != allOpened.end());

        auto pack = it->second;
        auto packElementType = pack.getElementType(componentIndex);
        if (auto exp = dyn_cast<PackExpansionType>(packElementType)) {
          assert(indexedShape && "pack substitution doesn't match in shape");
          packElementType = exp.getPatternType();
        } else {
          assert(!indexedShape && "pack substitution doesn't match in shape");
        }
        return packElementType;
      };
      auto substConformances = [&](CanType dependentType,
                                   Type conformingType,
                                   ProtocolDecl *protocol) -> ProtocolConformanceRef {
        // FIXME: This violates the spirit of this verifier check.
        return protocol->getParentModule()
            ->lookupConformance(conformingType, protocol);
      };

      // If the pack components and expected element types are SIL types,
      // we need to perform SIL substitution.
      if (typesAreSILTypes) {
        auto targetElementSILType =
          SILType::getPrimitiveObjectType(targetElementType);
        auto indexedElementSILType =
          SILType::getPrimitiveObjectType(indexedElementType);
        auto substTargetElementSILType =
          targetElementSILType.subst(F.getModule(),
                                     substTypes, substConformances);
        requireSameType(indexedElementSILType, substTargetElementSILType,
                        "lanewise-substituted pack element type didn't "
                        "match expected element type");
      } else {
        auto substTargetElementType =
          targetElementType.subst(substTypes, substConformances)
                           ->getCanonicalType();
        requireSameType(indexedElementType, substTargetElementType,
                        "lanewise-substituted pack element type didn't "
                        "match expected element type");
      }
    }
  }

  void checkPackPackIndexInst(PackPackIndexInst *i) {
    auto innerIndex = requireValueKind<AnyPackIndexInst>(i->getOperand(),
                                          "component pack index operand");
    if (!innerIndex) return;

    auto packType = i->getIndexedPackType();
    require(i->getComponentStartIndex() < packType->getNumElements(),
            "component index must be in bounds for indexed pack type");
    verifySameShape(innerIndex->getIndexedPackType(), packType,
                    i->getComponentStartIndex(), i->getComponentEndIndex());
  }

  void checkScalarPackIndexInst(ScalarPackIndexInst *i) {
    auto packType = i->getIndexedPackType();
    require(i->getComponentIndex() < packType->getNumElements(),
            "component index must be in bounds for indexed pack type");
    require(!isa<PackExpansionType>(
               packType.getElementType(i->getComponentIndex())),
            "component index must correspond to scalar component of "
            "indexed pack type");
  }

  void checkOpenPackElementInst(OpenPackElementInst *i) {
    requireObjectType(BuiltinPackIndexType, i->getOperand()->getType(),
                      "pack index operand");
    auto index = requireValueKind<AnyPackIndexInst>(i->getOperand(),
            "pack index operand must be one of the pack_index instructions");
    if (!index) return;

    verifySameShape(index->getIndexedPackType(), i->getOpenedShapeClass());
  }

  void checkPackElementGetInst(PackElementGetInst *i) {
    auto index = requireValueKind<AnyPackIndexInst>(i->getIndex(),
            "pack index operand must be one of the pack_index instructions");
    if (!index) return;

    verifyPackElementType(i->getPackType(), index, i->getElementType());
  }

  void checkPackElementSetInst(PackElementSetInst *i) {
    auto index = requireValueKind<AnyPackIndexInst>(i->getIndex(),
            "pack index operand must be one of the pack_index instructions");
    if (!index) return;

    verifyPackElementType(i->getPackType(), index, i->getElementType());
  }

  void checkTuplePackElementAddrInst(TuplePackElementAddrInst *i) {
    auto index = requireValueKind<AnyPackIndexInst>(i->getIndex(),
            "pack index operand must be one of the pack_index instructions");
    if (!index) return;

    // Remove the extra tuple element type structure.
    SmallVector<CanType, 8> tupleElements; {
      auto tupleType = requireAddressType(TupleType, i->getTuple()->getType(),
                                  "tuple operand of tuple_pack_element_addr");
      auto eltTypes = tupleType.getElementTypes();
      tupleElements.append(eltTypes.begin(), eltTypes.end());
    }

    require(i->getElementType().isAddress(),
            "result of tuple_pack_element_addr must be an address");

    verifyPackElementType(tupleElements, index,
                          i->getElementType().getASTType(),
                          /*types are SIL types*/ true);
  }

  // This verifies that the entry block of a SIL function doesn't have
  // any predecessors and also verifies the entry point arguments.
  void verifyEntryBlock(SILBasicBlock *entry) {
    require(entry->pred_empty(), "entry block cannot have predecessors");

    LLVM_DEBUG(
        llvm::dbgs() << "Argument types for entry point BB:\n";
        for (auto *arg
             : make_range(entry->args_begin(), entry->args_end()))
            arg->getType()
                .dump();
        llvm::dbgs() << "Input types for SIL function type ";
        F.getLoweredFunctionType()->print(llvm::dbgs()); llvm::dbgs() << ":\n";
        for (auto paramTy
             : fnConv.getParameterSILTypes(F.getTypeExpansionContext())) {
          paramTy.dump();
        });

    require(entry->args_size() == (fnConv.getNumIndirectSILResults()
                                   + fnConv.getNumParameters()),
            "entry point has wrong number of arguments");

    bool matched = true;
    auto argI = entry->args_begin();

    auto check = [&](const char *what, SILType ty) {
      auto mappedTy = F.mapTypeIntoContext(ty);
      SILArgument *bbarg = *argI;
      ++argI;
      if (bbarg->getType() != mappedTy &&
          bbarg->getType() != F.getLoweredType(mappedTy.getASTType())
                                  .getCategoryType(mappedTy.getCategory())) {
        llvm::errs() << what << " type mismatch!\n";
        llvm::errs() << "  argument: "; bbarg->dump();
        llvm::errs() << "  expected: "; mappedTy.dump();
        matched = false;
      }

      // If we do not have qualified ownership, do not check ownership.
      if (!F.hasOwnership()) {
        return;
      }

      auto ownershipkind = ValueOwnershipKind(
          F, mappedTy, fnConv.getSILArgumentConvention(bbarg->getIndex()));

      if (bbarg->getOwnershipKind() != ownershipkind) {
        llvm::errs() << what << " ownership kind mismatch!\n";
        llvm::errs() << "  argument: " << bbarg->getOwnershipKind() << '\n';
        llvm::errs() << "  expected: " << ownershipkind << '\n';
        matched = false;
      }
    };

    for (auto result : fnConv.getIndirectSILResults()) {
      assert(fnConv.isSILIndirect(result));
      check("indirect result",
            fnConv.getSILType(result, F.getTypeExpansionContext()));
    }
    for (auto param : F.getLoweredFunctionType()->getParameters()) {
      check("parameter", fnConv.getSILType(param, F.getTypeExpansionContext()));
    }

    require(matched, "entry point argument types do not match function type");

    // TBAA requirement for all address arguments.
    require(std::equal(entry->args_begin() + fnConv.getNumIndirectSILResults(),
                       entry->args_end(),
                       fnConv.funcTy->getParameters().begin(),
                       [&](SILArgument *bbarg, SILParameterInfo paramInfo) {
                         if (!bbarg->getType().isAddress())
                           return true;
                         switch (paramInfo.getConvention()) {
                         case ParameterConvention::Direct_Unowned:
                         case ParameterConvention::Direct_Guaranteed:
                         case ParameterConvention::Direct_Owned:
                           return false;
                         case ParameterConvention::Indirect_In:
                         case ParameterConvention::Indirect_Inout:
                         case ParameterConvention::Indirect_InoutAliasable:
                         case ParameterConvention::Indirect_In_Guaranteed:
                         case ParameterConvention::Pack_Owned:
                         case ParameterConvention::Pack_Guaranteed:
                         case ParameterConvention::Pack_Inout:
                           return true;
                         }
                       }),
            "entry point address argument must have an indirect calling "
            "convention");
  }

  void checkMoveValueInst(MoveValueInst *mvi) {
    require(mvi->getOperand()->getType().isObject(),
            "Operand value should be an object");
    require(mvi->getType() == mvi->getOperand()->getType(),
            "Result and operand must have the same type, today.");
  }

  // check that a drop_deinit can only ever be destroyed or destructured
  void checkDropDeinitUses(DropDeinitInst *ddi) {
    // Address-type drop_deinit has no special structural requirements. It just
    // sits there and blocks optimization on the allocation and downstream uses
    // of the address. If we want to optimize around address-type drop_deinit,
    // then we need a seperate verifier for its requirements.
    if (ddi->getType().isAddress())
      return;

    visitNonOwnershipUses(ddi, [&](Operand *use) {
      auto *user = use->getUser();
      require(isa<DestroyValueInst>(user)
              || isa<EndLifetimeInst>(user)
              || isa<DestructureStructInst>(user)
              || isa<SwitchEnumInst>(user),
              "A drop_deinit can only be destroyed or destructured");
      return true;
    });
  }

  void checkDropDeinitInst(DropDeinitInst *ddi) {
    require(F.hasOwnership(), "drop_deinit only allowed in OSSA");

    auto type = ddi->getType();
    require(type == ddi->getOperand()->getType(),
            "Result and operand must have the same type.");
    require(type.isMoveOnlyNominalType(),
            "drop_deinit only allowed for move-only types");
    require(type.getNominalOrBoundGenericNominal()
            ->getValueTypeDestructor(), "drop_deinit only allowed for "
            "struct/enum types that define a deinit");
    assert(!type.isTrivial(F) && "a type with a deinit is nontrivial");

    checkDropDeinitUses(ddi);
  }

  void checkMarkMustCheckInst(MarkMustCheckInst *i) {
    require(i->getModule().getStage() == SILStage::Raw,
            "Only valid in Raw SIL! Should have been eliminated by /some/ "
            "diagnostic pass");
    if (i->getType().isAddress())
      checkAddressWalkerCanVisitAllTransitiveUses(i);
  }

  void checkMarkUnresolvedReferenceBindingInst(
      MarkUnresolvedReferenceBindingInst *i) {
    require(i->getModule().getStage() == SILStage::Raw,
            "Only valid in Raw SIL! Should have been eliminated by /some/ "
            "diagnostic pass");
  }

  void checkMoveOnlyWrapperToCopyableValueInst(
      MoveOnlyWrapperToCopyableValueInst *cvt) {
    require(cvt->getOperand()->getType().isObject(),
            "Operand value should be an object");
    require(cvt->getOperand()->getType().isMoveOnlyWrapped(),
            "Operand should be move only wrapped");
    require(cvt->getType() ==
                cvt->getOperand()->getType().removingMoveOnlyWrapper(),
            "Result and operand must have the same type, today.");
  }

  void checkMoveOnlyWrapperToCopyableBoxInst(
      MoveOnlyWrapperToCopyableBoxInst *cvt) {
    require(cvt->getOperand()->getType().isObject(),
            "Operand value should be an object");
    require(cvt->getOperand()->getType().isBoxedMoveOnlyWrappedType(cvt->getFunction()),
            "Operand should be move only wrapped");
    require(cvt->getType() ==
            cvt->getOperand()->getType().removingMoveOnlyWrapperToBoxedType(cvt->getFunction()),
            "Result and operand must have the same type, today.");
  }

  void checkCopyableToMoveOnlyWrapperValueInst(
      CopyableToMoveOnlyWrapperValueInst *cvt) {
    require(cvt->getInitialKind() ==
                    CopyableToMoveOnlyWrapperValueInst::Owned ||
                !cvt->getOperand()->getType().isTrivial(*cvt->getFunction()),
            "To convert from a trivial value to a move only wrapper value use "
            "TrivialToGuaranteedMoveOnlyWrapperValueInst");
    require(cvt->getOperand()->getType().isObject(),
            "Operand value should be an object");
    require(cvt->getType().isMoveOnlyWrapped(), "Output should be move only");
    require(cvt->getType() ==
                cvt->getOperand()->getType().addingMoveOnlyWrapper(),
            "Result and operand must have the same type, today.");
  }

  void checkAllocPackMetadataInst(AllocPackMetadataInst *apmi) {
    require(apmi->getIntroducer()->mayRequirePackMetadata(),
            "Introduces instruction of kind which cannot emit on-stack pack "
            "metadata");
  }

  void checkDeallocPackMetadataInst(DeallocPackMetadataInst *dpmi) {
    auto *apmi = dpmi->getOperand()->getDefiningInstruction();
    require(apmi, "Must have instruction operand.");
    require(isa<AllocPackMetadataInst>(apmi),
            "Must have alloc_pack_metadata operand");
  }

  void checkMoveOnlyWrapperToCopyableAddrInst(
      MoveOnlyWrapperToCopyableAddrInst *cvt) {
    require(cvt->getType().isAddress(), "Output should be an address");
    require(cvt->getOperand()->getType().isMoveOnlyWrapped(),
            "Input should be move only");
    require(cvt->getType() ==
                cvt->getOperand()->getType().removingMoveOnlyWrapper(),
            "Result and operand must have the same type.");
  }

  void checkCopyableToMoveOnlyWrapperAddrInst(
      CopyableToMoveOnlyWrapperAddrInst *cvt) {
    require(cvt->getType().isAddress(), "Output should be an address");
    require(!cvt->getOperand()->getType().isMoveOnlyWrapped(),
            "Input should not be move only wrapped");
    require(cvt->getType() ==
                cvt->getOperand()->getType().addingMoveOnlyWrapper(),
            "Result and operand must have the same underlying type ignoring "
            "move only wrappedness.");
  }

  void verifyEpilogBlocks(SILFunction *F) {
    bool FoundReturnBlock = false;
    bool FoundThrowBlock = false;
    bool FoundUnwindBlock = false;
    for (auto &BB : *F) {
      if (isa<ReturnInst>(BB.getTerminator())) {
        require(!FoundReturnBlock,
                "more than one return block in function");
        FoundReturnBlock = true;
      } else if (isa<ThrowInst>(BB.getTerminator())) {
        require(!FoundThrowBlock,
                "more than one throw block in function");
        FoundThrowBlock = true;
      } else if (isa<UnwindInst>(BB.getTerminator())) {
        require(!FoundUnwindBlock,
                "more than one unwind block in function");
        FoundUnwindBlock = true;
      } else {
        assert(!BB.getTerminator()->isFunctionExiting());
      }
    }
  }

  bool isUnreachableAlongAllPathsStartingAt(
      SILBasicBlock *StartBlock, BasicBlockSet &Visited) {
    if (isa<UnreachableInst>(StartBlock->getTerminator()))
      return true;
    else if (isa<ReturnInst>(StartBlock->getTerminator()))
      return false;
    else if (isa<ThrowInst>(StartBlock->getTerminator()))
      return false;

    // Recursively check all successors.
    for (auto *SuccBB : StartBlock->getSuccessorBlocks())
      if (!Visited.insert(SuccBB))
        if (!isUnreachableAlongAllPathsStartingAt(SuccBB, Visited))
          return false;

    return true;
  }

  void verifySILFunctionType(CanSILFunctionType FTy) {
    // Make sure that if FTy's calling convention implies that it must have a
    // self parameter.
    require(!FTy->hasSelfParam() || !FTy->getParameters().empty(),
            "Functions with a calling convention with self parameter must "
            "have at least one argument for self.");
  }

  struct VerifyFlowSensitiveRulesDetails {
    enum CFGState {
      /// No special rules are in play.
      Normal,
      /// We've followed the resume edge of a yield in a yield_once coroutine.
      YieldOnceResume,
      /// We've followed the unwind edge of a yield.
      YieldUnwind
    };

    struct BBState {
      std::vector<SingleValueInstruction*> Stack;

      /// Contents: BeginAccessInst*, BeginApplyInst*.
      std::set<SILInstruction*> ActiveOps;

      CFGState CFG = Normal;
      
      GetAsyncContinuationInstBase *GotAsyncContinuation = nullptr;
    };
  };

  /// Verify the various control-flow-sensitive rules of SIL:
  ///
  /// - stack allocations and deallocations must obey a stack discipline
  /// - accesses must be uniquely ended
  /// - async continuations must be awaited before getting the continuation again, suspending
  ///  the task, or exiting the function
  /// - flow-sensitive states must be equivalent on all paths into a block
  void verifyFlowSensitiveRules(SILFunction *F) {
    // Do a traversal of the basic blocks.
    // Note that we intentionally don't verify these properties in blocks
    // that can't be reached from the entry block.
    llvm::DenseMap<SILBasicBlock*, VerifyFlowSensitiveRulesDetails::BBState> visitedBBs;
    SmallVector<SILBasicBlock*, 16> Worklist;
    visitedBBs.try_emplace(&*F->begin());
    Worklist.push_back(&*F->begin());
    while (!Worklist.empty()) {
      SILBasicBlock *BB = Worklist.pop_back_val();
      VerifyFlowSensitiveRulesDetails::BBState state = visitedBBs[BB];
      for (SILInstruction &i : *BB) {
        CurInstruction = &i;

        if (i.maySuspend()) {
          // Instructions that may suspend an async context must not happen
          // while the continuation is being accessed, with the exception of
          // the AwaitAsyncContinuationInst that completes suspending the task.
          if (auto aaci = dyn_cast<AwaitAsyncContinuationInst>(&i)) {
            require(state.GotAsyncContinuation == aaci->getOperand(),
                    "encountered await_async_continuation that doesn't match active gotten continuation");
            state.GotAsyncContinuation = nullptr;
          } else {
            require(!state.GotAsyncContinuation,
                    "cannot suspend async task while unawaited continuation is active");
          }
        }
          
        if (i.isAllocatingStack()) {
          state.Stack.push_back(cast<SingleValueInstruction>(&i));

        } else if (i.isDeallocatingStack()) {
          SILValue op = i.getOperand(0);
          require(!state.Stack.empty(),
                  "stack dealloc with empty stack");
          if (op != state.Stack.back()) {
            llvm::errs() << "Recent stack alloc: " << *state.Stack.back();
            llvm::errs() << "Matching stack alloc: " << *op;
            require(op == state.Stack.back(),
                    "stack dealloc does not match most recent stack alloc");
          }
          state.Stack.pop_back();

        } else if (isa<BeginAccessInst>(i) || isa<BeginApplyInst>(i) ||
                   isa<StoreBorrowInst>(i)) {
          bool notAlreadyPresent = state.ActiveOps.insert(&i).second;
          require(notAlreadyPresent,
                  "operation was not ended before re-beginning it");

        } else if (isa<EndAccessInst>(i) || isa<AbortApplyInst>(i) ||
                   isa<EndApplyInst>(i)) {
          if (auto beginOp = i.getOperand(0)->getDefiningInstruction()) {
            bool present = state.ActiveOps.erase(beginOp);
            require(present, "operation has already been ended");
          }
        } else if (auto *endBorrow = dyn_cast<EndBorrowInst>(&i)) {
          if (isa<StoreBorrowInst>(endBorrow->getOperand())) {
            if (auto beginOp = i.getOperand(0)->getDefiningInstruction()) {
              bool present = state.ActiveOps.erase(beginOp);
              require(present, "operation has already been ended");
            }
          }
        } else if (auto gaci = dyn_cast<GetAsyncContinuationInstBase>(&i)) {
          require(!state.GotAsyncContinuation,
                  "get_async_continuation while unawaited continuation is already active");
          state.GotAsyncContinuation = gaci;
        } else if (auto term = dyn_cast<TermInst>(&i)) {
          if (term->isFunctionExiting()) {
            require(state.Stack.empty(),
                    "return with stack allocs that haven't been deallocated");
            require(state.ActiveOps.empty(),
                    "return with operations still active");
            require(!state.GotAsyncContinuation,
                    "return with unawaited async continuation");

            if (isa<UnwindInst>(term)) {
              require(state.CFG == VerifyFlowSensitiveRulesDetails::YieldUnwind,
                      "encountered 'unwind' when not on unwind path");
            } else {
              require(state.CFG != VerifyFlowSensitiveRulesDetails::YieldUnwind,
                      "encountered 'return' or 'throw' when on unwind path");
              if (isa<ReturnInst>(term) &&
                  F->getLoweredFunctionType()->getCoroutineKind() ==
                    SILCoroutineKind::YieldOnce &&
                  F->getModule().getStage() != SILStage::Raw) {
                require(state.CFG == VerifyFlowSensitiveRulesDetails::YieldOnceResume,
                        "encountered 'return' before yielding a value in "
                        "yield_once coroutine");
              }
            }
          }
          
          if (isa<YieldInst>(term)) {
            require(state.CFG != VerifyFlowSensitiveRulesDetails::YieldOnceResume,
                    "encountered multiple 'yield's along single path");
            require(state.CFG == VerifyFlowSensitiveRulesDetails::Normal,
                    "encountered 'yield' on abnormal CFG path");
            require(!state.GotAsyncContinuation,
                    "encountered 'yield' while an unawaited continuation is active");
          }

          auto successors = term->getSuccessors();
          for (auto i : indices(successors)) {
            SILBasicBlock *succBB = successors[i].getBB();

            // Optimistically try to set our current state as the state
            // of the successor.  We can use a move on the final successor;
            // note that if the insertion fails, the move won't actually
            // happen, which is important because we'll still need it
            // to compare against the already-recorded state for the block.
            auto insertResult =
              i + 1 == successors.size()
                ? visitedBBs.try_emplace(succBB, std::move(state))
                : visitedBBs.try_emplace(succBB, state);

            // If the insertion was successful, add the successor to the
            // worklist and continue.
            if (insertResult.second) {
              Worklist.push_back(succBB);

              // If we're following a 'yield', update the CFG state:
              if (isa<YieldInst>(term)) {
                // Enforce that the unwind logic is segregated in all stages.
                if (i == 1) {
                  insertResult.first->second.CFG = VerifyFlowSensitiveRulesDetails::YieldUnwind;

                // We check the yield_once rule in the mandatory analyses,
                // so we can't assert it yet in the raw stage.
                } else if (F->getLoweredFunctionType()->getCoroutineKind()
                             == SILCoroutineKind::YieldOnce && 
                           F->getModule().getStage() != SILStage::Raw) {
                  insertResult.first->second.CFG = VerifyFlowSensitiveRulesDetails::YieldOnceResume;
                }
              }

              continue;
            }

            // This rule is checked elsewhere, but we'd want to assert it
            // here anyway.
            require(!isa<YieldInst>(term),
                    "successor of 'yield' should not be encountered twice");

            // Check that the stack height is consistent coming from all entry
            // points into this BB. We only care about consistency if there is
            // a possible return from this function along the path starting at
            // this successor bb.  (FIXME: Why? Infinite loops should still
            // preserve consistency...)
            auto isUnreachable = [&] {
              BasicBlockSet visited(F);
              return isUnreachableAlongAllPathsStartingAt(succBB, visited);
            };
            
            const auto &foundState = insertResult.first->second;
            require(state.Stack == foundState.Stack || isUnreachable(),
                    "inconsistent stack heights entering basic block");

            require(state.ActiveOps == foundState.ActiveOps || isUnreachable(),
                    "inconsistent active-operations sets entering basic block");
            require(state.CFG == foundState.CFG,
                    "inconsistent coroutine states entering basic block");
            require(state.GotAsyncContinuation == foundState.GotAsyncContinuation,
                    "inconsistent active async continuations entering basic block");
          }
        }
      }
    }
  }

  void verifyBranches(const SILFunction *F) {
    // Verify no critical edge.
    auto requireNonCriticalSucc = [this](const TermInst *termInst,
                                         const Twine &message) {
      // A critical edge has more than one outgoing edges from the source
      // block.
      auto succBlocks = termInst->getSuccessorBlocks();
      if (succBlocks.size() <= 1)
        return;

      for (const SILBasicBlock *destBB : succBlocks) {
        // And its destination block has more than one predecessor.
        _require(destBB->getSinglePredecessorBlock(), message);
      }
    };

    for (auto &bb : *F) {
      const TermInst *termInst = bb.getTerminator();
      CurInstruction = termInst;

      if (isSILOwnershipEnabled() && F->hasOwnership()) {
        requireNonCriticalSucc(termInst, "critical edges not allowed in OSSA");
      }
      // In Lowered SIL, they are allowed on conditional branches only.
      if (!AllowCriticalEdges && !isa<CondBranchInst>(termInst)) {
        requireNonCriticalSucc(termInst, "only cond_br critical edges allowed");
      }
    }
  }

  /// This pass verifies that there are no hole in debug scopes at -Onone.
  void verifyDebugScopeHoles(SILBasicBlock *BB) {
    if (!VerifyDIHoles)
      return;

    // These transforms don't set everything they move to implicit.
    if (M->getASTContext().LangOpts.Playground ||
        M->getASTContext().LangOpts.PCMacro)
      return;

    // This check only makes sense at -Onone. Optimizations,
    // e.g. inlining, can move scopes around.
    llvm::DenseSet<const SILDebugScope *> AlreadySeenScopes;
    if (BB->getParent()->getEffectiveOptimizationMode() !=
        OptimizationMode::NoOptimization)
      return;

    // Exit early if this BB is empty.
    if (BB->empty())
      return;

    const SILDebugScope *LastSeenScope = nullptr;
    SILInstruction *LastSeenScopeInst = nullptr;
    for (SILInstruction &SI : *BB) {
      if (SI.isMetaInstruction())
        continue;
      // FIXME: Profile counters for loop bodies may be emitted before the
      // instructions for the loop variable, but in a deeper scope.
      if (isa<IncrementProfilerCounterInst>(SI))
        continue;
      if (!SI.getLoc().hasValidLineNumber())
        continue;
      if (SI.getLoc().getKind() == SILLocation::CleanupKind)
        continue;
      // FIXME: These still leave holes in the scopes. We should make them
      // inherit the sourrounding scope in IRGenSIL.
      if (SI.getLoc().getKind() == SILLocation::MandatoryInlinedKind)
        continue;
      // FIXME: There are situations where the execution legitimately goes
      //        backwards, such as
      //
      //            while case let w: String? = Optional.some("b") {}
      //
      //        where the RHS of the assignment gets run first and then the
      //        result is copied into the LHS.
      if (!llvm::isa<BeginBorrowInst>(&SI) || !llvm::isa<CopyValueInst>(&SI))
        continue;

      // If we haven't seen this debug scope yet, update the
      // map and go on.
      auto *DS = SI.getDebugScope();
      assert(DS && "Each instruction should have a debug scope");

      // We don't support this verification on inlined call sites yet.
      if (DS->InlinedCallSite)
        continue;

      if (!AlreadySeenScopes.count(DS)) {
        AlreadySeenScopes.insert(DS);
        LastSeenScope = DS;
        LastSeenScopeInst = &SI;
        continue;
      }

      // Otherwise, we're allowed to re-enter a scope only if
      // the scope is an ancestor of the scope we're currently leaving.
      auto isAncestorScope = [](const SILDebugScope *Cur,
                                const SILDebugScope *Previous) {
        assert(Cur && "null current scope queried");
        assert(Previous && "null previous scope queried");
        const SILDebugScope *Tmp = Previous;
        while (Tmp) {
          auto Parent = Tmp->Parent;
          auto *ParentScope = Parent.dyn_cast<const SILDebugScope *>();
          if (ParentScope == Cur)
            return true;
          Tmp = ParentScope;
        }
        return false;
      };

      if (isAncestorScope(DS, LastSeenScope)) {
        LastSeenScope = DS;
        LastSeenScopeInst = &SI;
        continue;
      }
      if (DS != LastSeenScope) {
        llvm::errs() << "Broken instruction!\n"; 
        SI.dump();
#ifndef NDEBUG
        llvm::errs() << "in scope\n";
        DS->print(SI.getFunction()->getModule());
#endif
        llvm::errs() << "Previous, non-contiguous scope set by";
        LastSeenScopeInst->dump();
#ifndef NDEBUG
        llvm::errs() << "in scope\n";
        LastSeenScope->print(SI.getFunction()->getModule());
#endif
        llvm::errs() << "Please report a bug on bugs.swift.org\n";
        llvm::errs() <<
          "Pass -Xllvm -verify-di-holes=false to disable the verification\n";
        // Turn on debug info printing so that the log actually shows the bad
        // scopes.
        SILPrintDebugInfo.setValue(true);
        require(
            DS == LastSeenScope,
            "Basic block contains a non-contiguous lexical scope at -Onone");
      }
    }
  }

  void visitSILBasicBlock(SILBasicBlock *BB) {
    SILInstructionVisitor::visitSILBasicBlock(BB);
    verifyDebugScopeHoles(BB);
  }

  void visitBasicBlockArguments(SILBasicBlock *BB) {
    CurInstruction = nullptr;
    for (auto argI = BB->args_begin(), argEnd = BB->args_end(); argI != argEnd;
         ++argI)
      visitSILArgument(*argI);
  }

  void visitSILBasicBlocks(SILFunction *F) {
    // Visit all basic blocks in the RPOT order.
    // This ensures that any open_existential instructions, which
    // open archetypes, are seen before the uses of these archetypes.
    llvm::ReversePostOrderTraversal<SILFunction *> RPOT(F);
    BasicBlockSet VisitedBBs(F);
    for (auto Iter = RPOT.begin(), E = RPOT.end(); Iter != E; ++Iter) {
      auto *BB = *Iter;
      VisitedBBs.insert(BB);
      visitSILBasicBlock(BB);
    }

    // Visit all basic blocks that were not visited during the RPOT traversal,
    // e.g. unreachable basic blocks.
    for (auto &BB : *F) {
      if (VisitedBBs.contains(&BB))
        continue;
      visitSILBasicBlock(&BB);
    }

    verifyPredecessorSucessorStructure(F);
  }

  // Make sure that each of the successors/predecessors of a basic block
  // have this basic block in its predecessor/successor list.
  void verifyPredecessorSucessorStructure(SILFunction *f) {
    using PredSuccPair = std::pair<SILBasicBlock *, SILBasicBlock *>;
    llvm::DenseSet<PredSuccPair> foundSuccessors;
    llvm::DenseSet<PredSuccPair> foundPredecessors;

    for (auto &block : *f) {
      for (SILBasicBlock *succ : block.getSuccessorBlocks()) {
        foundSuccessors.insert({&block, succ});
      }
      for (SILBasicBlock *pred : block.getPredecessorBlocks()) {
        foundPredecessors.insert({pred, &block});
      }
    }
    for (PredSuccPair predSucc : foundSuccessors) {
      require(foundPredecessors.contains(predSucc),
              "block is not predecessor of its successor");
    }
    for (PredSuccPair predSucc : foundPredecessors) {
      require(foundSuccessors.contains(predSucc),
              "block is not successor of its predecessor");
    }
  }

  void visitSILFunction(SILFunction *F) {
    PrettyStackTraceSILFunction stackTrace("verifying", F);

    CanSILFunctionType FTy = F->getLoweredFunctionType();
    verifySILFunctionType(FTy);

    SILModule &mod = F->getModule();

    require(!F->isSerialized() || !mod.isSerialized() || mod.isParsedAsSerializedSIL(),
            "cannot have a serialized function after the module has been serialized");

    switch (F->getLinkage()) {
    case SILLinkage::Public:
    case SILLinkage::Shared:
      require(F->isDefinition() || F->hasForeignBody(),
              "public/shared function must have a body");
      break;
    case SILLinkage::PublicNonABI:
      require(F->isDefinition(),
              "alwaysEmitIntoClient function must have a body");
      require(F->isSerialized() || mod.isSerialized(),
              "alwaysEmitIntoClient function must be serialized");
      break;
    case SILLinkage::Hidden:
    case SILLinkage::Private:
      require(F->isDefinition() || F->hasForeignBody(),
              "internal/private function must have a body");
      require(!F->isSerialized(),
              "internal/private function cannot be serialized or serializable");
      break;
    case SILLinkage::PublicExternal:
      require(F->isExternalDeclaration() || F->isSerialized() ||
              mod.isSerialized(),
            "public-external function definition must be serialized");
      break;
    case SILLinkage::HiddenExternal:
      require(F->isExternalDeclaration(),
              "hidden-external function cannot have a body");
      break;
    }

    // Don't verify functions that were skipped. We are likely to see them in
    // FunctionBodySkipping::NonInlinableWithoutTypes mode.
    auto Ctx = F->getDeclContext();
    if (Ctx) {
      if (auto AFD = dyn_cast<AbstractFunctionDecl>(Ctx)) {
        if (AFD->isBodySkipped())
          return;
      }
    }

    if (F->isExternalDeclaration()) {
      if (F->hasForeignBody())
        return;

      require(F->isAvailableExternally(),
              "external declaration of internal SILFunction not allowed");
      // If F is an external declaration, there is nothing further to do,
      // return.
      return;
    }

    assert(!F->hasForeignBody());

    // Make sure that our SILFunction only has context generic params if our
    // SILFunctionType is non-polymorphic.
    if (F->getGenericEnvironment() &&
        !F->getGenericEnvironment()->getGenericSignature()
            ->areAllParamsConcrete()) {
      require(FTy->isPolymorphic(),
              "non-generic function definitions cannot have a "
              "generic environment");
    } else {
      require(!FTy->isPolymorphic(),
              "generic function definition must have a generic environment");
    }

    // Otherwise, verify the body of the function.
    verifyEntryBlock(F->getEntryBlock());
    verifyEpilogBlocks(F);
    verifyFlowSensitiveRules(F);
    verifyBranches(F);

    visitSILBasicBlocks(F);

    if (F->hasOwnership() && F->shouldVerifyOwnership() &&
        !mod.getASTContext().hadError()) {
      F->verifyMemoryLifetime(passManager);
    }
  }

  void verify(bool isCompleteOSSA) {
    if (!isCompleteOSSA || !F.getModule().getOptions().OSSACompleteLifetimes) {
      DEBlocks = std::make_unique<DeadEndBlocks>(const_cast<SILFunction *>(&F));
    }
    visitSILFunction(const_cast<SILFunction*>(&F));
  }
};
} // end anonymous namespace

#undef require
#undef requireObjectType

//===----------------------------------------------------------------------===//
//                     Out of Line Verifier Run Functions
//===----------------------------------------------------------------------===//

static bool verificationEnabled(const SILModule &M) {
  // If we are asked to never verify, return false early.
  if (M.getOptions().VerifyNone)
    return false;

  // Otherwise, if verify all is set, we always verify.
  if (M.getOptions().VerifyAll)
    return true;

#ifndef NDEBUG
  // Otherwise if we do have asserts enabled, always verify...
  return true;
#else
  // And if we don't never verify.
  return false;
#endif
}

/// verify - Run the SIL verifier to make sure that the SILFunction follows
/// invariants.
void SILFunction::verify(SILPassManager *passManager,
                         bool SingleFunction, bool isCompleteOSSA,
                         bool checkLinearLifetime) const {
  if (!verificationEnabled(getModule()))
    return;

  // Please put all checks in visitSILFunction in SILVerifier, not here. This
  // ensures that the pretty stack trace in the verifier is included with the
  // back trace when the verifier crashes.
  SILVerifier verifier(*this, passManager, SingleFunction, checkLinearLifetime);
  verifier.verify(isCompleteOSSA);
}

void SILFunction::verifyCriticalEdges() const {
  if (!verificationEnabled(getModule()))
    return;

  SILVerifier(*this, /*passManager=*/nullptr,
                     /*SingleFunction=*/true,
                     /*checkLinearLifetime=*/ false).verifyBranches(this);
}

/// Verify that a property descriptor follows invariants.
void SILProperty::verify(const SILModule &M) const {
  if (!verificationEnabled(M))
    return;

  auto *decl = getDecl();
  auto *dc = decl->getInnermostDeclContext();
  
  // TODO: base type for global/static descriptors
  auto sig = dc->getGenericSignatureOfContext();
  auto baseTy = dc->getInnermostTypeContext()->getSelfInterfaceType()
                  ->getReducedType(sig);
  auto leafTy = decl->getValueInterfaceType()->getReducedType(sig);
  SubstitutionMap subs;
  if (sig) {
    auto env = dc->getGenericEnvironmentOfContext();
    subs = env->getForwardingSubstitutionMap();
    baseTy = env->mapTypeIntoContext(baseTy)->getCanonicalType();
    leafTy = env->mapTypeIntoContext(leafTy)->getCanonicalType();
  }
  bool hasIndices = false;
  if (auto subscript = dyn_cast<SubscriptDecl>(decl)) {
    hasIndices = subscript->getIndices()->size() != 0;
  }

  auto canSig = sig.getCanonicalSignature();

  auto require = [&](bool reqt, StringRef message) {
      if (!reqt) {
        llvm::errs() << message << "\n";
        assert(false && "invoking standard assertion failure");
      }
    };

  if (auto &component = getComponent()) {
    auto typeExpansionContext =
        TypeExpansionContext::noOpaqueTypeArchetypesSubstitution(
            ResilienceExpansion::Maximal);
    verifyKeyPathComponent(const_cast<SILModule&>(M),
                           typeExpansionContext,
                           require,
                           baseTy,
                           leafTy,
                           *component,
                           {},
                           canSig,
                           subs,
                           /*property descriptor*/true,
                           hasIndices);
    // verifyKeyPathComponent updates baseTy to be the projected type of the
    // component, which should be the same as the type of the declared storage
    require(baseTy == leafTy,
            "component type of property descriptor should match type of storage");
  }
}

/// Verify that a vtable follows invariants.
void SILVTable::verify(const SILModule &M) const {
  if (!verificationEnabled(M))
    return;
  
  // Compare against the base class vtable if there is one.
  const SILVTable *superVTable = nullptr;
  auto superclass = getClass()->getSuperclassDecl();
  if (superclass) {
    for (auto &vt : M.getVTables()) {
      if (vt->getClass() == superclass) {
        superVTable = vt;
        break;
      }
    }
  }
  
  for (unsigned i : indices(getEntries())) {
    auto &entry = getEntries()[i];
    
    // Make sure the module's lookup cache is consistent.
    assert(entry == *getEntry(const_cast<SILModule &>(M), entry.getMethod())
           && "vtable entry is out of sync with method's vtable cache");
    
    // All vtable entries must be decls in a class context.
    assert(entry.getMethod().hasDecl() && "vtable entry is not a decl");
    auto baseInfo = M.Types.getConstantInfo(TypeExpansionContext::minimal(),
                                            entry.getMethod());
    ValueDecl *decl = entry.getMethod().getDecl();

    assert((!isa<AccessorDecl>(decl)
            || !cast<AccessorDecl>(decl)->isObservingAccessor())
           && "observing accessors shouldn't have vtable entries");

    // For ivar destroyers, the decl is the class itself.
    ClassDecl *theClass;
    if (entry.getMethod().kind == SILDeclRef::Kind::IVarDestroyer)
      theClass = dyn_cast<ClassDecl>(decl);
    else
      theClass = dyn_cast<ClassDecl>(decl->getDeclContext());

    assert(theClass && "vtable entry must refer to a class member");

    // The class context must be the vtable's class, or a superclass thereof.
    assert(theClass->isSuperclassOf(getClass()) &&
           "vtable entry must refer to a member of the vtable's class");

    // Foreign entry points shouldn't appear in vtables.
    assert(!entry.getMethod().isForeign && "vtable entry must not be foreign");

    // The vtable entry must be ABI-compatible with the overridden vtable slot.
    SmallString<32> baseName;
    {
      llvm::raw_svector_ostream os(baseName);
      entry.getMethod().print(os);
    }

    if (M.getStage() != SILStage::Lowered) {
      SILVerifier(*entry.getImplementation(), /*passManager=*/nullptr,
                                              /*SingleFunction=*/true,
                                              /*checkLinearLifetime=*/ false)
          .requireABICompatibleFunctionTypes(
              baseInfo.getSILType().castTo<SILFunctionType>(),
              entry.getImplementation()->getLoweredFunctionType(),
              "vtable entry for " + baseName + " must be ABI-compatible",
              *entry.getImplementation());
    }
    
    // Validate the entry against its superclass vtable.
    if (!superclass) {
      // Root methods should not have inherited or overridden entries.
      bool validKind;
      switch (entry.getKind()) {
      case Entry::Normal:
        validKind = true;
        break;
        
      case Entry::Inherited:
      case Entry::Override:
        validKind = false;
        break;
      }
      assert(validKind && "vtable entry in root class must not be inherited or override");
    } else if (superVTable) {
      // Validate the entry against the matching entry from the superclass
      // vtable.

      const Entry *superEntry = nullptr;
      for (auto &se : superVTable->getEntries()) {
        if (se.getMethod().getOverriddenVTableEntry() ==
            entry.getMethod().getOverriddenVTableEntry()) {
          superEntry = &se;
          break;
        }
      }

      switch (entry.getKind()) {
      case Entry::Normal:
        assert(!superEntry && "non-root vtable entry must be inherited or override");
        break;

      case Entry::Inherited:
        if (!superEntry)
          break;

        assert(entry.isNonOverridden() == superEntry->isNonOverridden()
               && "inherited vtable entry must share overridden-ness of superclass entry");
        break;
          
      case Entry::Override:
        assert(!entry.isNonOverridden()
               && "override entry can't claim to be nonoverridden");
        if (!superEntry)
          break;

        // The superclass entry must not prohibit overrides.
        assert(!superEntry->isNonOverridden()
               && "vtable entry overrides an entry that claims to have no overrides");
        // TODO: Check the root vtable entry for the method too.

        break;
      }
    }
  }
}

/// Verify that a witness table follows invariants.
void SILWitnessTable::verify(const SILModule &M) const {
  if (!verificationEnabled(M))
    return;

  if (isDeclaration())
    assert(getEntries().empty() &&
           "A witness table declaration should not have any entries.");

  for (const Entry &E : getEntries())
    if (E.getKind() == SILWitnessTable::WitnessKind::Method) {
      SILFunction *F = E.getMethodWitness().Witness;
      if (F) {
        // If a SILWitnessTable is going to be serialized, it must only
        // reference public or serializable functions.
        if (isSerialized()) {
          assert(F->hasValidLinkageForFragileRef() &&
                 "Fragile witness tables should not reference "
                 "less visible functions.");
        }

        assert(F->getLoweredFunctionType()->getRepresentation() ==
               SILFunctionTypeRepresentation::WitnessMethod &&
               "Witnesses must have witness_method representation.");
      }
    }
}

/// Verify that a default witness table follows invariants.
void SILDefaultWitnessTable::verify(const SILModule &M) const {
#ifndef NDEBUG
  for (const Entry &E : getEntries()) {
    // FIXME: associated type witnesses.
    if (!E.isValid() || E.getKind() != SILWitnessTable::Method)
      continue;

    SILFunction *F = E.getMethodWitness().Witness;
    if (!F)
      continue;

#if 0
    // FIXME: For now, all default witnesses are private.
    assert(F->hasValidLinkageForFragileRef() &&
           "Default witness tables should not reference "
           "less visible functions.");
#endif

    assert(F->getLoweredFunctionType()->getRepresentation() ==
           SILFunctionTypeRepresentation::WitnessMethod &&
           "Default witnesses must have witness_method representation.");
  }
#endif
}

/// Verify that a global variable follows invariants.
void SILGlobalVariable::verify() const {
  if (!verificationEnabled(getModule()))
    return;

  assert(getLoweredType().isObject()
         && "global variable cannot have address type");

  // Verify the static initializer.
  for (const SILInstruction &I : StaticInitializerBlock) {
    auto init = cast<SingleValueInstruction>(&I);
    if (init == &StaticInitializerBlock.back()) {
      assert(init->use_empty() && "Init value must not have another use");
    } else {
      assert(!init->use_empty() && "dead instruction in static initializer");
      assert(!isa<ObjectInst>(init) &&
             "object instruction is only allowed for final initial value");
    }
    assert(I.getParent() == &StaticInitializerBlock);
  }
}

void SILModule::verify(bool isCompleteOSSA, bool checkLinearLifetime) const {
  SILPassManager pm(const_cast<SILModule *>(this), /*isMandatory=*/false, /*IRMod=*/nullptr);
  verify(&pm, isCompleteOSSA, checkLinearLifetime);
}

/// Verify the module.
void SILModule::verify(SILPassManager *passManager,
                       bool isCompleteOSSA, bool checkLinearLifetime) const {
  if (!verificationEnabled(*this))
    return;

  checkForLeaks();

  // Uniquing set to catch symbol name collisions.
  llvm::DenseSet<StringRef> symbolNames;

  // Check all functions.
  for (const SILFunction &f : *this) {
    if (!symbolNames.insert(f.getName()).second) {
      llvm::errs() << "Symbol redefined: " << f.getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    f.verify(passManager, /*singleFunction*/ false, isCompleteOSSA, checkLinearLifetime);
  }

  // Check all globals.
  for (const SILGlobalVariable &g : getSILGlobals()) {
    if (!symbolNames.insert(g.getName()).second) {
      llvm::errs() << "Symbol redefined: " << g.getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    g.verify();
  }

  // Check all vtables and the vtable cache.
  llvm::DenseSet<ClassDecl*> vtableClasses;
  unsigned EntriesSZ = 0;
  for (const auto &vt : getVTables()) {
    if (!vtableClasses.insert(vt->getClass()).second) {
      llvm::errs() << "Vtable redefined: " << vt->getClass()->getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    vt->verify(*this);
    // Check if there is a cache entry for each vtable entry
    for (auto entry : vt->getEntries()) {
      if (VTableEntryCache.find({vt, entry.getMethod()}) ==
          VTableEntryCache.end()) {
        llvm::errs() << "Vtable entry for function: "
                     << entry.getImplementation()->getName()
                     << "not in cache!\n";
        assert(false && "triggering standard assertion failure routine");
      }
      ++EntriesSZ;
    }
  }
  assert(EntriesSZ == VTableEntryCache.size() &&
         "Cache size is not equal to true number of VTable entries");

  // Check all witness tables.
  LLVM_DEBUG(llvm::dbgs() <<"*** Checking witness tables for duplicates ***\n");
  llvm::DenseSet<RootProtocolConformance*> wtableConformances;
  for (const SILWitnessTable &wt : getWitnessTables()) {
    LLVM_DEBUG(llvm::dbgs() << "Witness Table:\n"; wt.dump());
    auto conformance = wt.getConformance();
    if (!wtableConformances.insert(conformance).second) {
      llvm::errs() << "Witness table redefined: ";
      conformance->printName(llvm::errs());
      assert(false && "triggering standard assertion failure routine");
    }
    wt.verify(*this);
  }

  // Check all default witness tables.
  LLVM_DEBUG(llvm::dbgs() << "*** Checking default witness tables for "
             "duplicates ***\n");
  llvm::DenseSet<const ProtocolDecl *> defaultWitnessTables;
  for (const SILDefaultWitnessTable &wt : getDefaultWitnessTables()) {
    LLVM_DEBUG(llvm::dbgs() << "Default Witness Table:\n"; wt.dump());
    if (!defaultWitnessTables.insert(wt.getProtocol()).second) {
      llvm::errs() << "Default witness table redefined: ";
      wt.dump();
      assert(false && "triggering standard assertion failure routine");
    }
    wt.verify(*this);
  }
  
  // Check property descriptors.
  LLVM_DEBUG(llvm::dbgs() << "*** Checking property descriptors ***\n");
  for (auto &prop : getPropertyList()) {
    prop.verify(*this);
  }
}

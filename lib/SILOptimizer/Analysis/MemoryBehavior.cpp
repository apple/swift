//===--- MemoryBehavior.cpp -----------------------------------------------===//
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

#define DEBUG_TYPE "sil-membehavior"

#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                       Memory Behavior Implementation
//===----------------------------------------------------------------------===//

namespace {

using MemBehavior = MemoryBehavior;

/// Visitor that determines the memory behavior of an instruction relative to a
/// specific SILValue (i.e. can the instruction cause the value to be read,
/// etc.).
///
/// TODO: Clarify what it means to return a MayHaveSideEffects result. Does this
/// mean that the instruction may release objects referenced by value 'V'?
/// Deallocate the an address contained in 'V'? Are any other code motion
/// barriers relevant here?
class MemoryBehaviorVisitor
    : public SILInstructionVisitor<MemoryBehaviorVisitor, MemBehavior> {

  AliasAnalysis *AA;

  /// The value we are attempting to discover memory behavior relative to.
  SILValue V;

  /// Cache either the address of the access corresponding to memory at 'V', or
  /// 'V' itself if it isn't recognized as part of an access. The cached value
  /// is always a valid SILValue.
  SILValue cachedValueAddress;

  llvm::Optional<bool> cachedIsLetValue;

  /// The SILType of the value.
  llvm::Optional<SILType> TypedAccessTy;

public:
  MemoryBehaviorVisitor(AliasAnalysis *AA, SILValue V)
      : AA(AA), V(V) {}

  SILType getValueTBAAType() {
    if (!TypedAccessTy)
      TypedAccessTy = computeTBAAType(V);
    return *TypedAccessTy;
  }

  /// If 'V' is an address projection within a formal access, return the
  /// canonical address of the formal access if possible without looking past
  /// any storage casts. Otherwise, a "best-effort" address
  ///
  /// If 'V' is an address, then the returned value is also an address.
  SILValue getValueAddress() {
    if (!cachedValueAddress) {
      cachedValueAddress =
          V->getType().isAddress() ? getTypedAccessAddress(V) : V;
    }
    return cachedValueAddress;
  }

  /// Return true if 'V's accessed address is that of a let variables.
  bool isLetValue() {
    if (!cachedIsLetValue) {
      cachedIsLetValue =
          V->getType().isAddress() && isLetAddress(getValueAddress());
    }
    return cachedIsLetValue.value();
  }

  // Return true is the given address (or pointer) may alias with 'V'.
  bool mayAlias(SILValue opAddress) {
    if (AA->isNoAlias(opAddress, V, computeTBAAType(opAddress),
                      getValueTBAAType())) {
      LLVM_DEBUG(llvm::dbgs()
                 << "No alias: access " << opAddress << " value " << V);
      return false;
    }
    LLVM_DEBUG(llvm::dbgs()
               << "May alias: access " << opAddress << " value " << V);
    return true;
  }

  MemBehavior visitValueBase(ValueBase *V) {
    llvm_unreachable("unimplemented");
  }

  MemBehavior visitSILInstruction(SILInstruction *Inst) {
    // If we do not have any more information, just use the general memory
    // behavior implementation.
    auto Behavior = Inst->getMemoryBehavior();

    // If this is a regular read-write access then return the computed memory
    // behavior.
    if (!isLetValue())
      return Behavior;

    // If this is a read-only access to 'let variable'. Other side effects, such
    // as releases of the object containing a 'let' property are still relevant.
    switch (Behavior) {
    case MemBehavior::MayReadWrite:       return MemBehavior::MayRead;
    case MemBehavior::MayWrite:           return MemBehavior::None;
    default: return Behavior;
    }
  }

  MemBehavior visitBeginAccessInst(BeginAccessInst *beginAccess) {
    if (!mayAlias(beginAccess->getSource()))
      return MemBehavior::None;

    // begin_access does not physically read or write memory. But we model it
    // as a memory read and/or write to prevent optimizations to move other
    // aliased loads/stores across begin_access into the access scope.
    switch (beginAccess->getAccessKind()) {
    case SILAccessKind::Deinit:
      // For the same reason we treat a ``load [take]`` or a ``destroy_addr``
      // as a memory write, we do that for a ``begin_access [deinit]`` as well.
      // See MemoryBehavior.
      return MemBehavior::MayReadWrite;
    case SILAccessKind::Read:
      return MemBehavior::MayRead;
    case SILAccessKind::Modify:
      if (isLetValue()) {
        assert(getAccessBase(beginAccess) != getValueAddress()
               && "let modification not allowed");
        return MemBehavior::None;
      }
      return MemBehavior::MayReadWrite;
    case SILAccessKind::Init:
      return MemBehavior::MayWrite;
    }
    llvm_unreachable("invalid access kind");
  }

  MemBehavior visitEndAccessInst(EndAccessInst *endAccess) {
    // end_access does not physically read or write memory. But, similar to
    // begin_access, we model it as a memory read and/or write to prevent
    // optimizations to move other aliased loads/stores across end_access into
    // the access scope.
    return visitBeginAccessInst(endAccess->getBeginAccess());
  }

  MemBehavior visitLoadInst(LoadInst *LI);
  MemBehavior visitStoreInst(StoreInst *SI);
  MemBehavior visitCopyAddrInst(CopyAddrInst *CAI);
  MemBehavior visitMarkUnresolvedMoveAddrInst(MarkUnresolvedMoveAddrInst *MAI);
  MemBehavior visitApplyInst(ApplyInst *AI);
  MemBehavior visitTryApplyInst(TryApplyInst *AI);
  MemBehavior visitBeginApplyInst(BeginApplyInst *AI);
  MemBehavior visitEndApplyInst(EndApplyInst *EAI);
  MemBehavior visitAbortApplyInst(AbortApplyInst *AAI);
  MemBehavior visitBuiltinInst(BuiltinInst *BI);
  MemBehavior visitStrongReleaseInst(StrongReleaseInst *BI);
  MemBehavior visitReleaseValueInst(ReleaseValueInst *BI);
  MemBehavior visitDestroyValueInst(DestroyValueInst *DVI);
  MemBehavior visitSetDeallocatingInst(SetDeallocatingInst *BI);
  MemBehavior visitBeginCOWMutationInst(BeginCOWMutationInst *BCMI);
  MemBehavior visitDebugValueInst(DebugValueInst *dv);
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  MemBehavior visit##Name##ReleaseInst(Name##ReleaseInst *BI);
#include "swift/AST/ReferenceStorage.def"

  // Instructions which are none if our SILValue does not alias one of its
  // arguments. If we cannot prove such a thing, return the relevant memory
  // behavior.
#define OPERANDALIAS_MEMBEHAVIOR_INST(Name)                                    \
  MemBehavior visit##Name(Name *I) {                                           \
    for (Operand & Op : I->getAllOperands()) {                                 \
      if (mayAlias(Op.get()))                                                  \
        return I->getMemoryBehavior();                                         \
    }                                                                          \
    return MemBehavior::None;                                                  \
  }

  OPERANDALIAS_MEMBEHAVIOR_INST(InjectEnumAddrInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(UncheckedTakeEnumDataAddrInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(InitExistentialAddrInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(DeinitExistentialAddrInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(DeallocStackInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(FixLifetimeInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(ClassifyBridgeObjectInst)
  OPERANDALIAS_MEMBEHAVIOR_INST(ValueToBridgeObjectInst)
#undef OPERANDALIAS_MEMBEHAVIOR_INST

  // Override simple behaviors where MayHaveSideEffects is too general and
  // encompasses other behavior that is not read/write/ref count decrement
  // behavior we care about.
#define SIMPLE_MEMBEHAVIOR_INST(Name, Behavior)                         \
  MemBehavior visit##Name(Name *I) { return MemBehavior::Behavior; }
  SIMPLE_MEMBEHAVIOR_INST(CondFailInst, None)
#undef SIMPLE_MEMBEHAVIOR_INST

  // Incrementing reference counts doesn't have an observable memory effect.
#define REFCOUNTINC_MEMBEHAVIOR_INST(Name)                                     \
  MemBehavior visit##Name(Name *I) {                                           \
    return MemBehavior::None;                                                \
  }
  REFCOUNTINC_MEMBEHAVIOR_INST(StrongRetainInst)
  REFCOUNTINC_MEMBEHAVIOR_INST(RetainValueInst)
  REFCOUNTINC_MEMBEHAVIOR_INST(CopyValueInst)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  REFCOUNTINC_MEMBEHAVIOR_INST(Name##RetainValueInst)                          \
  REFCOUNTINC_MEMBEHAVIOR_INST(StrongCopy##Name##ValueInst)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  REFCOUNTINC_MEMBEHAVIOR_INST(Name##RetainInst)                               \
  REFCOUNTINC_MEMBEHAVIOR_INST(StrongRetain##Name##Inst)                       \
  REFCOUNTINC_MEMBEHAVIOR_INST(StrongCopy##Name##ValueInst)
#include "swift/AST/ReferenceStorage.def"
#undef REFCOUNTINC_MEMBEHAVIOR_INST
};

} // end anonymous namespace

MemBehavior MemoryBehaviorVisitor::visitLoadInst(LoadInst *LI) {
  if (!mayAlias(LI->getOperand()))
    return MemBehavior::None;

  LLVM_DEBUG(llvm::dbgs() << "  Could not prove that load inst does not alias "
                             "pointer. ");

  if (LI->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
    LLVM_DEBUG(llvm::dbgs() << "Is a take so return MayReadWrite.\n");
    return MemBehavior::MayReadWrite;
  }

  LLVM_DEBUG(llvm::dbgs() << "Not a take so returning MayRead.\n");
  return MemBehavior::MayRead;
}

MemBehavior MemoryBehaviorVisitor::visitStoreInst(StoreInst *SI) {
  // No store besides the initialization of a "let"-variable
  // can have any effect on the value of this "let" variable.
  if (isLetValue() && (getAccessBase(SI->getDest()) != getValueAddress())) {
    return MemBehavior::None;
  }
  // If the store dest cannot alias the pointer in question and we are not
  // releasing anything due to an assign, then the specified value cannot be
  // modified by the store.
  if (!mayAlias(SI->getDest())) {
    if (SI->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
      // Consider side effects of the destructor
      return AA->getMemoryEffectOnEscapedAddress(V, SI);
    }

    return MemBehavior::None;
  }

  // Otherwise, a store just writes.
  LLVM_DEBUG(llvm::dbgs() << "  Could not prove store does not alias inst. "
                             "Returning default mem behavior.\n");
  return SI->getMemoryBehavior();
}

MemBehavior MemoryBehaviorVisitor::visitCopyAddrInst(CopyAddrInst *CAI) {
  // If it's an assign to the destination, a destructor might be called on the
  // old value. This can have any side effects.
  // We could also check if it's a trivial type (which cannot have any side
  // effect on destruction), but such copy_addr instructions are optimized to
  // load/stores anyway, so it's probably not worth it.
  if (!CAI->isInitializationOfDest())
    return MemBehavior::MayHaveSideEffects;

  bool mayWrite = mayAlias(CAI->getDest());
  bool mayRead = mayAlias(CAI->getSrc());
  
  if (mayRead) {
    if (mayWrite)
      return MemBehavior::MayReadWrite;

    // A take is modelled as a write. See MemoryBehavior::MayWrite.
    if (CAI->isTakeOfSrc())
      return MemBehavior::MayReadWrite;

    return MemBehavior::MayRead;
  }
  if (mayWrite)
    return MemBehavior::MayWrite;

  return MemBehavior::None;
}

MemBehavior MemoryBehaviorVisitor::visitMarkUnresolvedMoveAddrInst(
    MarkUnresolvedMoveAddrInst *MAI) {
  bool mayWrite = mayAlias(MAI->getDest());
  bool mayRead = mayAlias(MAI->getSrc());

  if (mayRead) {
    if (mayWrite)
      return MemBehavior::MayReadWrite;

    // mark_unresolved_move_addr doesn't semantically perform a take of src.
    return MemBehavior::MayRead;
  }

  if (mayWrite)
    return MemBehavior::MayWrite;

  return MemBehavior::None;
}

MemBehavior MemoryBehaviorVisitor::visitBuiltinInst(BuiltinInst *BI) {
  MemBehavior mb = BI->getMemoryBehavior();
  if (mb != MemBehavior::None) {
    return AA->getMemoryEffectOnEscapedAddress(V, BI);
  }
  return MemBehavior::None;
}

MemBehavior MemoryBehaviorVisitor::visitTryApplyInst(TryApplyInst *AI) {
  return AA->getMemoryEffectOnEscapedAddress(V, AI);
}

MemBehavior MemoryBehaviorVisitor::visitApplyInst(ApplyInst *AI) {
  return AA->getMemoryEffectOnEscapedAddress(V, AI);
}

MemBehavior MemoryBehaviorVisitor::visitBeginApplyInst(BeginApplyInst *AI) {
  return AA->getMemoryEffectOnEscapedAddress(V, AI);
}

MemBehavior MemoryBehaviorVisitor::visitEndApplyInst(EndApplyInst *EAI) {
  return AA->getMemoryEffectOnEscapedAddress(V, EAI->getBeginApply());
}

MemBehavior MemoryBehaviorVisitor::visitAbortApplyInst(AbortApplyInst *AAI) {
  return AA->getMemoryEffectOnEscapedAddress(V, AAI->getBeginApply());
}

MemBehavior
MemoryBehaviorVisitor::visitStrongReleaseInst(StrongReleaseInst *SI) {
  return AA->getMemoryEffectOnEscapedAddress(V, SI);
}

#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
MemBehavior \
MemoryBehaviorVisitor::visit##Name##ReleaseInst(Name##ReleaseInst *SI) { \
  return AA->getMemoryEffectOnEscapedAddress(V, SI); \
}
#include "swift/AST/ReferenceStorage.def"

MemBehavior MemoryBehaviorVisitor::visitReleaseValueInst(ReleaseValueInst *SI) {
  return AA->getMemoryEffectOnEscapedAddress(V, SI);
}

MemBehavior
MemoryBehaviorVisitor::visitDestroyValueInst(DestroyValueInst *DVI) {
  return AA->getMemoryEffectOnEscapedAddress(V, DVI);
}

MemBehavior MemoryBehaviorVisitor::visitSetDeallocatingInst(SetDeallocatingInst *SDI) {
  return MemBehavior::None;
}

MemBehavior MemoryBehaviorVisitor::
visitBeginCOWMutationInst(BeginCOWMutationInst *BCMI) {
  // begin_cow_mutation is defined to have side effects, because it has
  // dependencies with instructions which retain the buffer operand.
  // But it never interferes with any memory address.
  return MemBehavior::None;
}

MemBehavior MemoryBehaviorVisitor::
visitDebugValueInst(DebugValueInst *dv) {
  SILValue op = dv->getOperand();
  if (op->getType().isAddress() && mayAlias(op)) {
    return MemBehavior::MayRead;
  }
  return MemBehavior::None;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

MemBehavior
AliasAnalysis::computeMemoryBehavior(SILInstruction *Inst, SILValue V) {
  MemBehaviorCacheKey Key = {V, Inst};
  // Check if we've already computed this result.
  auto It = MemoryBehaviorCache.find(Key);
  if (It != MemoryBehaviorCache.end()) {
    return It->second;
  }

  // Calculate the aliasing result and store it in the cache.
  auto Result = computeMemoryBehaviorInner(Inst, V);
  MemoryBehaviorCache[Key] = Result;
  return Result;
}

/// If \p V is an address of an immutable memory, return the begin of the
/// scope where the memory can be considered to be immutable.
///
/// This is either a ``begin_access [read]`` in case V is the result of the
/// begin_access or a projection of it.
/// Or it is the begin of a borrow scope (begin_borrow, load_borrow, a
/// guaranteed function argument) of an immutable copy-on-write buffer.
/// For example:
///   %b = begin_borrow %array_buffer
///   %V = ref_element_addr [immutable] %b : $BufferType, #BufferType.someField
///
static SILValue getBeginScopeInst(SILValue V) {
  SILValue accessScope = getAccessScope(V);
  if (auto *access = dyn_cast<BeginAccessInst>(accessScope)) {
    if (access->getAccessKind() == SILAccessKind::Read &&
        access->getEnforcement() != SILAccessEnforcement::Unsafe)
      return access;
    return SILValue();
  }
  SILValue accessBase = getAccessBase(V);
  SILValue object;
  if (auto *elementAddr = dyn_cast<RefElementAddrInst>(accessBase)) {
    if (!elementAddr->isImmutable())
      return SILValue();
    object = elementAddr->getOperand();
  } else if (auto *tailAddr = dyn_cast<RefTailAddrInst>(accessBase)) {
    if (!tailAddr->isImmutable())
      return SILValue();
    object = tailAddr->getOperand();
  } else {
    return SILValue();
  }
  if (BorrowedValue borrowedObj = getSingleBorrowIntroducingValue(object)) {
    return borrowedObj.value;
  }
  if (!object->getFunction()->hasOwnership()) {
    // In non-OSSA, do a quick check if the object is a guaranteed function
    // argument.
    // Note that in OSSA, getSingleBorrowIntroducingValue will detect a
    // guaranteed argument.
    SILValue root = findOwnershipReferenceAggregate(object);
    if (auto *funcArg = dyn_cast<SILFunctionArgument>(root)) {
      if (funcArg->getArgumentConvention().isGuaranteedConvention())
        return funcArg;
    }
  }
  return SILValue();
}

/// Collect all instructions which are inside an immutable scope.
///
/// The \p beginScopeInst is either a ``begin_access [read]`` or the begin of a
/// borrow scope (begin_borrow, load_borrow) of an immutable copy-on-write
/// buffer.
void AliasAnalysis::computeImmutableScope(SingleValueInstruction *beginScopeInst) {
  BasicBlockSet visitedBlocks(beginScopeInst->getFunction());
  llvm::SmallVector<std::pair<SILInstruction *, SILBasicBlock *>, 16> workList;
  
  auto addEndScopeInst = [&](SILInstruction *endScope) {
    workList.push_back({endScope, endScope->getParent()});
    bool isNew = visitedBlocks.insert(endScope->getParent());
    (void)isNew;
    assert(isNew);
  };
  
  // First step: add all scope-ending instructions to the worklist.
  if (auto *beginAccess = dyn_cast<BeginAccessInst>(beginScopeInst)) {
    for (EndAccessInst *endAccess : beginAccess->getEndAccesses()) {
      addEndScopeInst(endAccess);
    }
  } else {
    visitTransitiveEndBorrows(beginScopeInst, addEndScopeInst);
  }

  // Second step: walk up the control flow until the beginScopeInst and add
  // all (potentially) memory writing instructions to instsInImmutableScopes.
  while (!workList.empty()) {
    auto instAndBlock = workList.pop_back_val();
    SILBasicBlock *block = instAndBlock.second;
    // If the worklist entry doesn't have an instruction, start at the end of
    // the block.
    auto iter = instAndBlock.first ? instAndBlock.first->getIterator()
                                   : block->end();
    // Walk up the instruction list - either to the begin of the block or until
    // we hit the beginScopeInst.
    while (true) {
      if (iter == block->begin()) {
        assert(block != block->getParent()->getEntryBlock() &&
               "didn't find the beginScopeInst when walking up the CFG");
        // Add all predecessor blocks to the worklist.
        for (SILBasicBlock *pred : block->getPredecessorBlocks()) {
          if (visitedBlocks.insert(pred))
            workList.push_back({nullptr, pred});
        }
        break;
      }
      --iter;
      SILInstruction *inst = &*iter;
      if (inst == beginScopeInst) {
        // When we are at the beginScopeInst we terminate the CFG walk.
        break;
      }
      if (inst->mayWriteToMemory()) {
        instsInImmutableScopes.insert({beginScopeInst, inst});
      }
    }
  }
}

/// Returns true if \p inst is in an immutable scope of V.
///
/// That means that even if we don't know anything about inst, we can be sure
/// that inst cannot write to V.
/// An immutable scope is for example a read-only begin_access/end_access scope.
/// Another example is a borrow scope of an immutable copy-on-write buffer.
bool AliasAnalysis::isInImmutableScope(SILInstruction *inst, SILValue V) {
  if (!V->getType().isAddress())
    return false;
    
  SILValue beginScope = getBeginScopeInst(V);
  if (!beginScope)
    return false;

  if (auto *funcArg = dyn_cast<SILFunctionArgument>(beginScope)) {
    // The immutable scope (= an guaranteed argument) spans over the whole
    // function. We don't need to do any scope computation in this case.
    assert(funcArg->getArgumentConvention().isGuaranteedConvention());
    return true;
  }

  auto *beginScopeInst = dyn_cast<SingleValueInstruction>(beginScope);
  if (!beginScopeInst)
    return false;

  // Recompute the scope if not done yet.
  if (immutableScopeComputed.insert(beginScopeInst).second) {
    computeImmutableScope(beginScopeInst);
  }
  return instsInImmutableScopes.contains({beginScopeInst, inst});
}

MemBehavior
AliasAnalysis::computeMemoryBehaviorInner(SILInstruction *Inst, SILValue V) {
  LLVM_DEBUG(llvm::dbgs() << "GET MEMORY BEHAVIOR FOR:\n    " << *Inst << "    "
                          << *V);
  MemBehavior result = MemoryBehaviorVisitor(this, V).visit(Inst);
  
  // If the "regular" alias analysis thinks that Inst may modify V, check if
  // Inst is in an immutable scope of V.
  if (result > MemBehavior::MayRead && isInImmutableScope(Inst, V)) {
    return (result == MemBehavior::MayWrite) ? MemBehavior::None
                                             : MemBehavior::MayRead;
  }
  return result;
}

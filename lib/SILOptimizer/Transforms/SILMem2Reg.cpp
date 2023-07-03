//===--- SILMem2Reg.cpp - Promotes AllocStacks to registers ---------------===//
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
// This pass promotes AllocStack instructions into virtual register
// references. It only handles load, store and deallocation
// instructions. The algorithm is based on:
//
//  Sreedhar and Gao. A linear time algorithm for placing phi-nodes. POPL '95.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-mem2reg"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/TaggedUnion.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/OSSALifetimeCompletion.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/ScopeOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <queue>

using namespace swift;
using namespace swift::siloptimizer;

STATISTIC(NumAllocStackFound,    "Number of AllocStack found");
STATISTIC(NumAllocStackCaptured, "Number of AllocStack captured");
STATISTIC(NumInstRemoved,        "Number of Instructions removed");

static bool lexicalLifetimeEnsured(AllocStackInst *asi);
static bool isGuaranteedLexicalValue(SILValue src);

namespace {

using DomTreeNode = llvm::DomTreeNodeBase<SILBasicBlock>;
using DomTreeLevelMap = llvm::DenseMap<DomTreeNode *, unsigned>;

/// A transient structure containing the values that are accessible in some
/// context: coming into a block, going out of the block, or within a block
/// (during promoteAllocationInBlock and removeSingleBlockAllocation).
///
/// At block boundaries, these are phi arguments or initializationPoints.  As we
/// iterate over a block, a way to keep track of the current (running) value
/// within a block.
class LiveValues {
public:
  struct Owned {
    SILValue stored = SILValue();
    SILValue move = SILValue();

    /// Create an instance of the minimum values required to replace a usage of
    /// an AllocStackInst.  It consists of only one value.
    ///
    /// Whether the one value occupies the stored or the move field depends on
    /// whether the alloc_stack is lexical.  If it is lexical, then usages of
    /// the asi will be replaced with usages of the move field; otherwise,
    /// those usages will be replaced with usages of the stored field.  The
    /// implementation constructs an instance to match those requirements.
    static Owned toReplace(AllocStackInst *asi, SILValue replacement) {
      if (lexicalLifetimeEnsured(asi))
        return {SILValue(), replacement};
      return {replacement, SILValue()};
    }

    /// The value with which usages of the provided AllocStackInst should be
    /// replaced.
    SILValue replacement(AllocStackInst *asi, SILInstruction *toReplace) {
      if (!lexicalLifetimeEnsured(asi)) {
        return stored;
      }
      // We should have created a move of the @owned stored value.
      assert(move);
      return move;
    }

    bool canEndLexicalLifetime() {
      // If running value originates from a load which was not preceded by a
      // store in the same basic block, then we don't have enough information
      // to end a lexical lifetime. In that case, the lifetime end will be
      // added later, when we have enough information, namely the live in
      // values, to end it.
      return move;
    }
  };
  struct Guaranteed {
    SILValue stored = SILValue();
    SILValue borrow = SILValue();

    /// Create an instance of the minimum values required to replace a usage of
    /// an AllocStackInst.  It consists of only one value.
    ///
    /// Whether the one value occupies the stored or the borrow field depends
    /// on whether the alloc_stack is lexical.  If it is lexical, then usages
    /// of \p asi will be replaced with usages of the borrow field; otherwise,
    /// those usages will be replaced with usages of the stored field.  The
    /// implementation constructs an instance to match those requirements.
    static Guaranteed toReplace(AllocStackInst *asi, SILValue replacement) {
      if (lexicalLifetimeEnsured(asi))
        return {SILValue(), replacement};
      return {replacement, SILValue()};
    }

    /// The value with which usages of the provided AllocStackInst should be
    /// replaced.
    SILValue replacement(AllocStackInst *asi, SILInstruction *toReplace) {
      if (!lexicalLifetimeEnsured(asi)) {
        return stored;
      }
      // For guaranteed lexical AllocStackInsts--i.e. those that are
      // store_borrow locations--we may have created a borrow if the stored
      // value is a non-lexical guaranteed value.
      assert(isGuaranteedLexicalValue(stored) || borrow);
      return borrow ? borrow : stored;
    }

    bool canEndLexicalLifetime() {
      // There are two different cases when we don't create a lexical lifetime
      // end for a guaranteed running value:
      //
      // If the source of the store_borrow is already lexical, then the running
      // value doesn't have a lexical lifetime of its own which could be ended.
      //
      // If running value originates from a load which was not preceded by a
      // store_borrow in the same basic block, then we don't have enough
      // information to end a lexical lifetime. In that case, the lifetime end
      // will be added later, when we have enough information, namely the live
      // in values, to end it.
      return borrow;
    }
  };

private:
  using Storage = TaggedUnion<Owned, Guaranteed>;
  Storage storage;

  LiveValues(Storage storage) : storage(storage) {}

  static LiveValues forGuaranteed(Guaranteed values) {
    return {Storage(values)};
  }

  static LiveValues forOwned(Owned values) { return {Storage(values)}; }

public:
  enum class Kind {
    Owned,
    Guaranteed,
  };

  Kind getKind() {
    if (storage.isa<Owned>()) {
      return Kind::Owned;
    }
    return Kind::Guaranteed;
  }

  bool isOwned() { return getKind() == Kind::Owned; }

  bool isGuaranteed() { return getKind() == Kind::Guaranteed; }

  static LiveValues forGuaranteed(SILValue stored, SILValue borrow) {
    return LiveValues::forGuaranteed({stored, borrow});
  }

  static LiveValues forOwned(SILValue stored, SILValue move) {
    return LiveValues::forOwned({stored, move});
  }

  static LiveValues forValues(SILValue stored, SILValue lexical) {
    if (stored->getOwnershipKind() == OwnershipKind::Guaranteed) {
      return LiveValues::forGuaranteed({stored, lexical});
    }
    return LiveValues::forOwned({stored, lexical});
  }

  static LiveValues toReplace(AllocStackInst *asi, SILValue replacement) {
    if (replacement->getOwnershipKind() == OwnershipKind::Guaranteed) {
      return LiveValues::forGuaranteed(Guaranteed::toReplace(asi, replacement));
    }
    return LiveValues::forOwned(Owned::toReplace(asi, replacement));
  }

  Owned getOwned() { return storage.get<Owned>(); }

  Guaranteed getGuaranteed() { return storage.get<Guaranteed>(); }

  SILValue replacement(AllocStackInst *asi, SILInstruction *toReplace) {
    if (auto *owned = storage.dyn_cast<Owned>()) {
      return owned->replacement(asi, toReplace);
    }
    auto &guaranteed = storage.get<Guaranteed>();
    return guaranteed.replacement(asi, toReplace);
  }

  SILValue getStored() {
    if (auto *owned = storage.dyn_cast<Owned>()) {
      return owned->stored;
    }
    auto &guaranteed = storage.get<Guaranteed>();
    return guaranteed.stored;
  }

  bool canEndLexicalLifetime() {
    if (auto *owned = storage.dyn_cast<Owned>()) {
      return owned->canEndLexicalLifetime();
    }
    auto &guaranteed = storage.get<Guaranteed>();
    return guaranteed.canEndLexicalLifetime();
  }
};

/// A transient structure used only by promoteAllocationInBlock and
/// removeSingleBlockAllocation.
///
/// A pair of a CFG-position-relative value T and a boolean indicating whether
/// the alloc_stack's storage is valid at the position where that value exists.
template <typename T>
struct StorageStateTracking {
  /// The value which exists at some CFG position.
  T value;
  /// Whether the stack storage is initialized at that position.
  bool isStorageValid;
};

} // anonymous namespace

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

/// Make the specified instruction cease to be a user of its operands and add it
/// to the list of instructions to delete.
///
/// This both (1) removes the specified instruction from the list of users of
/// its operands, avoiding disrupting logic that examines those users and (2)
/// keeps the specified instruction in place, allowing it to be used for
/// insertion until instructionsToDelete is culled.
static void
prepareForDeletion(SILInstruction *inst,
                   SmallVectorImpl<SILInstruction *> &instructionsToDelete) {
  for (auto &operand : inst->getAllOperands()) {
    operand.set(SILUndef::get(operand.get()->getType(), *inst->getFunction()));
  }
  instructionsToDelete.push_back(inst);
}

static void
replaceDestroy(DestroyAddrInst *dai, SILValue newValue, SILBuilderContext &ctx,
               InstructionDeleter &deleter,
               SmallVectorImpl<SILInstruction *> &instructionsToDelete) {
  SILFunction *f = dai->getFunction();
  auto ty = dai->getOperand()->getType();

  assert(ty.isLoadable(*f) && "Unexpected promotion of address-only type!");

  assert(newValue ||
         (ty.is<TupleType>() && ty.getAs<TupleType>()->getNumElements() == 0));

  SILBuilderWithScope builder(dai, ctx);

  auto &typeLowering = f->getTypeLowering(ty);

  bool expand = shouldExpand(dai->getModule(),
                             dai->getOperand()->getType().getObjectType());
  using TypeExpansionKind = Lowering::TypeLowering::TypeExpansionKind;
  auto expansionKind = expand ? TypeExpansionKind::MostDerivedDescendents
                              : TypeExpansionKind::None;
  typeLowering.emitLoweredDestroyValue(builder, dai->getLoc(), newValue,
                                       expansionKind);

  prepareForDeletion(dai, instructionsToDelete);
}

/// Whether the specified debug_value's operand names the address at the
/// indicated alloc_stack.
///
/// If it's a guaranteed alloc_stack (i.e. a store_borrow location), that
/// includes the values produced by any store_borrows whose destinations are the
/// alloc_stack since those values amount to aliases for the alloc_stack's
/// storage.
static bool isDebugValueOfAllocStack(DebugValueInst *dvi, AllocStackInst *asi) {
  auto value = dvi->getOperand();
  if (value == asi)
    return true;
  auto *sbi = dyn_cast<StoreBorrowInst>(value);
  if (!sbi)
    return false;
  return sbi->getDest() == asi;
}

/// Promote a DebugValue w/ address value to a DebugValue of non-address value.
static void promoteDebugValueAddr(DebugValueInst *dvai, SILValue value,
                                  SILBuilderContext &ctx,
                                  InstructionDeleter &deleter) {
  assert(dvai->getOperand()->getType().isLoadable(*dvai->getFunction()) &&
         "Unexpected promotion of address-only type!");
  assert(value && "Expected valid value");

  // Avoid inserting the same debug_value twice.
  //
  // We remove the di expression when comparing since:
  //
  // 1. dvai is on will always have the deref diexpr since it is on addresses.
  //
  // 2. We are only trying to delete debug_var that are on values... values will
  //    never have an op_deref meaning that the comparison will always fail and
  //    not serve out purpose here.
  auto dvaiWithoutDIExpr = dvai->getVarInfo()->withoutDIExpr();
  for (auto *use : value->getUses()) {
    if (auto *dvi = dyn_cast<DebugValueInst>(use->getUser())) {
      if (!dvi->hasAddrVal() && *dvi->getVarInfo() == dvaiWithoutDIExpr) {
        deleter.forceDelete(dvai);
        return;
      }
    }
  }

  // Drop op_deref if dvai is actually a debug_value instruction
  auto varInfo = *dvai->getVarInfo();
  if (isa<DebugValueInst>(dvai)) {
    auto &diExpr = varInfo.DIExpr;
    if (diExpr)
      diExpr.eraseElement(diExpr.element_begin());
  }

  SILBuilderWithScope b(dvai, ctx);
  b.createDebugValue(dvai->getLoc(), value, std::move(varInfo));
  deleter.forceDelete(dvai);
}

/// Returns true if \p I is a load which loads from \p ASI.
static bool isLoadFromStack(SILInstruction *i, AllocStackInst *asi) {
  if (!isa<LoadInst>(i) && !isa<LoadBorrowInst>(i))
    return false;

  if (auto *lbi = dyn_cast<LoadBorrowInst>(i)) {
    if (BorrowedValue(lbi).hasReborrow())
      return false;
  }

  // Skip struct and tuple address projections.
  ValueBase *op = i->getOperand(0);
  while (op != asi) {
    if (!isa<UncheckedAddrCastInst>(op) && !isa<StructElementAddrInst>(op) &&
        !isa<TupleElementAddrInst>(op) && !isa<StoreBorrowInst>(op))
      return false;
    if (auto *sbi = dyn_cast<StoreBorrowInst>(op)) {
      op = sbi->getDest();
      continue;
    }
    op = cast<SingleValueInstruction>(op)->getOperand(0);
  }
  return true;
}

/// Collects all load instructions which (transitively) use \p i as address.
static void collectLoads(SILInstruction *i,
                         SmallVectorImpl<SILInstruction *> &foundLoads) {
  if (isa<LoadInst>(i) || isa<LoadBorrowInst>(i)) {
    foundLoads.push_back(i);
    return;
  }
  if (!isa<UncheckedAddrCastInst>(i) && !isa<StructElementAddrInst>(i) &&
      !isa<TupleElementAddrInst>(i))
    return;

  // Recursively search for other loads in the instruction's uses.
  for (auto *use : cast<SingleValueInstruction>(i)->getUses()) {
    collectLoads(use->getUser(), foundLoads);
  }
}

/// Returns true if \p I is an address of a LoadInst, skipping struct and
/// tuple address projections. Sets \p singleBlock to null if the load (or
/// it's address is not in \p singleBlock.
/// This function looks for these patterns:
/// 1. (load %ASI)
/// 2. (load (struct_element_addr/tuple_element_addr/unchecked_addr_cast %ASI))
static bool isAddressForLoad(SILInstruction *load, SILBasicBlock *&singleBlock,
                             bool &involvesUntakableProjection) {
  if (auto *li = dyn_cast<LoadInst>(load)) {
    // SILMem2Reg is disabled when we find a load [take] of an untakable
    // projection.  See below for further discussion.
    if (involvesUntakableProjection &&
        li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
      return false;
    }
    return true;
  }

  if (isa<LoadBorrowInst>(load)) {
    if (involvesUntakableProjection) {
      return false;
    }
    return true;
  }

  if (!isa<UncheckedAddrCastInst>(load) && !isa<StructElementAddrInst>(load) &&
      !isa<TupleElementAddrInst>(load))
    return false;

  // None of the projections are lowered to owned values:
  //
  // struct_element_addr and tuple_element_addr instructions are lowered to
  // struct_extract and tuple_extract instructions respectively.  These both
  // have guaranteed ownership (since they forward ownership and can only be
  // used on a guaranteed value).
  //
  // unchecked_addr_cast instructions are lowered to unchecked_bitwise_cast
  // instructions.  These have unowned ownership.
  //
  // So in no case can a load [take] be lowered into the new projected value
  // (some sequence of struct_extract, tuple_extract, and
  // unchecked_bitwise_cast instructions) taking over ownership of the original
  // value.  Without additional changes.
  //
  // For example, for a sequence of element_addr projections could be
  // transformed into a sequence of destructure instructions, followed by a
  // sequence of structure instructions where all the original values are
  // kept in place but the taken value is "knocked out" and replaced with
  // undef.  The running value would then be set to the newly structed
  // "knockout" value.
  //
  // Alternatively, a new copy of the running value could be created and a new
  // set of destroys placed after its last uses.
  involvesUntakableProjection = true;

  // Recursively search for other (non-)loads in the instruction's uses.
  auto *svi = cast<SingleValueInstruction>(load);
  for (auto *use : svi->getUses()) {
    SILInstruction *user = use->getUser();
    if (user->getParent() != singleBlock)
      singleBlock = nullptr;

    if (!isAddressForLoad(user, singleBlock, involvesUntakableProjection))
      return false;
  }
  return true;
}

/// Returns true if \p I is a dead struct_element_addr or tuple_element_addr.
static bool isDeadAddrProjection(SILInstruction *inst) {
  if (!isa<UncheckedAddrCastInst>(inst) && !isa<StructElementAddrInst>(inst) &&
      !isa<TupleElementAddrInst>(inst))
    return false;

  // Recursively search for uses which are dead themselves.
  for (auto UI : cast<SingleValueInstruction>(inst)->getUses()) {
    SILInstruction *II = UI->getUser();
    if (!isDeadAddrProjection(II))
      return false;
  }
  return true;
}

/// Returns true if this \p def is captured.
/// Sets \p inSingleBlock to true if all uses of \p def are in a single block.
static bool isCaptured(SILValue def, bool *inSingleBlock) {
  SILBasicBlock *singleBlock = def->getParentBlock();

  // For all users of the def
  for (auto *use : def->getUses()) {
    SILInstruction *user = use->getUser();

    if (user->getParent() != singleBlock)
      singleBlock = nullptr;

    // Loads are okay.
    bool involvesUntakableProjection = false;
    if (isAddressForLoad(user, singleBlock, involvesUntakableProjection))
      continue;

    // We can store into an AllocStack (but not the pointer).
    if (auto *si = dyn_cast<StoreInst>(user))
      if (si->getDest() == def)
        continue;

    if (auto *sbi = dyn_cast<StoreBorrowInst>(user)) {
      if (sbi->getDest() == def) {
        if (isCaptured(sbi, inSingleBlock)) {
          return true;
        }
        continue;
      }
    }

    // Deallocation is also okay, as are DebugValue w/ address value. We will
    // promote the latter into normal DebugValue.
    if (isa<DeallocStackInst>(user) || DebugValueInst::hasAddrVal(user))
      continue;

    if (isa<EndBorrowInst>(user))
      continue;

    // Destroys of loadable types can be rewritten as releases, so
    // they are fine.
    if (auto *dai = dyn_cast<DestroyAddrInst>(user))
      if (dai->getOperand()->getType().isLoadable(*dai->getFunction()))
        continue;

    // Other instructions are assumed to capture the AllocStack.
    LLVM_DEBUG(llvm::dbgs() << "*** AllocStack is captured by: " << *user);
    return true;
  }

  // None of the users capture the AllocStack.
  *inSingleBlock = (singleBlock != nullptr);
  return false;
}

/// Returns true if the \p def is only stored into.
static bool isWriteOnlyAllocation(SILValue def) {
  assert(isa<AllocStackInst>(def) || isa<StoreBorrowInst>(def));

  // For all users of the def:
  for (auto *use : def->getUses()) {
    SILInstruction *user = use->getUser();

    // It is okay to store into the AllocStack.
    if (auto *si = dyn_cast<StoreInst>(user))
      if (!isa<AllocStackInst>(si->getSrc()))
        continue;

    if (auto *sbi = dyn_cast<StoreBorrowInst>(user)) {
      // Since all uses of the alloc_stack will be via store_borrow, check if
      // there are any non-writes from the store_borrow location.
      if (!isWriteOnlyAllocation(sbi)) {
        return false;
      }
      continue;
    }

    // Deallocation is also okay.
    if (isa<DeallocStackInst>(user))
      continue;

    if (isa<EndBorrowInst>(user))
      continue;

    // If we haven't already promoted the AllocStack, we may see
    // DebugValue uses.
    if (DebugValueInst::hasAddrVal(user))
      continue;

    if (isDeadAddrProjection(user))
      continue;

    // Can't do anything else with it.
    LLVM_DEBUG(llvm::dbgs() << "*** AllocStack has non-write use: " << *user);
    return false;
  }

  return true;
}

static void
replaceLoad(SILInstruction *inst, SILValue newValue, AllocStackInst *asi,
            SILBuilderContext &ctx, InstructionDeleter &deleter,
            SmallVectorImpl<SILInstruction *> &instructionsToDelete) {
  assert(isa<LoadInst>(inst) || isa<LoadBorrowInst>(inst));
  ProjectionPath projections(newValue->getType());
  SILValue op = inst->getOperand(0);
  SILBuilderWithScope builder(inst, ctx);
  SILOptScope scope;

  while (op != asi) {
    assert(isa<UncheckedAddrCastInst>(op) || isa<StructElementAddrInst>(op) ||
           isa<TupleElementAddrInst>(op) ||
           isa<StoreBorrowInst>(op) &&
               "found instruction that should have been skipped in "
               "isLoadFromStack");
    if (auto *sbi = dyn_cast<StoreBorrowInst>(op)) {
      op = sbi->getDest();
      continue;
    }
    auto *projInst = cast<SingleValueInstruction>(op);
    projections.push_back(Projection(projInst));
    op = projInst->getOperand(0);
  }

  for (const auto &proj : llvm::reverse(projections)) {
    assert(proj.getKind() == ProjectionKind::BitwiseCast ||
           proj.getKind() == ProjectionKind::Struct ||
           proj.getKind() == ProjectionKind::Tuple);

    // struct_extract and tuple_extract expect guaranteed operand ownership
    // non-trivial RunningVal is owned. Insert borrow operation to convert them
    // to guaranteed!
    if (proj.getKind() == ProjectionKind::Struct ||
        proj.getKind() == ProjectionKind::Tuple) {
      if (auto opVal = scope.borrowValue(inst, newValue)) {
        assert(*opVal != newValue &&
               "Valid value should be different from input value");
        newValue = *opVal;
      }
    }
    newValue =
        proj.createObjectProjection(builder, inst->getLoc(), newValue).get();
  }

  op = inst->getOperand(0);

  if (auto *lbi = dyn_cast<LoadBorrowInst>(inst)) {
    if (lexicalLifetimeEnsured(asi) &&
        newValue->getOwnershipKind() == OwnershipKind::Guaranteed) {
      SmallVector<SILInstruction *, 4> endBorrows;
      for (auto *ebi : lbi->getUsersOfType<EndBorrowInst>()) {
        endBorrows.push_back(ebi);
      }
      for (auto *ebi : endBorrows) {
        prepareForDeletion(ebi, instructionsToDelete);
      }
      lbi->replaceAllUsesWith(newValue);
    } else {
      auto *borrow = SILBuilderWithScope(lbi, ctx).createBeginBorrow(
          lbi->getLoc(), newValue, asi->isLexical());
      lbi->replaceAllUsesWith(borrow);
    }
  } else {
    auto *li = cast<LoadInst>(inst);
    // Replace users of the loaded value with `newValue`
    // If we have a load [copy], replace the users with copy_value of `newValue`
    if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
      li->replaceAllUsesWith(builder.createCopyValue(li->getLoc(), newValue));
    } else {
      li->replaceAllUsesWith(newValue);
    }
  }

  // Pop the scope so that we emit cleanups.
  std::move(scope).popAtEndOfScope(&*builder.getInsertionPoint());

  // Delete the load
  prepareForDeletion(inst, instructionsToDelete);

  while (op != asi && op->use_empty()) {
    assert(isa<UncheckedAddrCastInst>(op) || isa<StructElementAddrInst>(op) ||
           isa<TupleElementAddrInst>(op) || isa<StoreBorrowInst>(op));
    if (auto *sbi = dyn_cast<StoreBorrowInst>(op)) {
      SILValue next = sbi->getDest();
      deleter.forceDelete(sbi);
      op = next;
      continue;
    }
    auto *inst = cast<SingleValueInstruction>(op);
    SILValue next = inst->getOperand(0);
    deleter.forceDelete(inst);
    op = next;
  }
}

/// Instantiate the specified empty type by recursively tupling and structing
/// the empty types aggregated together at each level.
static SILValue createValueForEmptyType(SILType ty,
                                        SILInstruction *insertionPoint,
                                        SILBuilderContext &ctx) {
  auto *function = insertionPoint->getFunction();
  assert(ty.isEmpty(*function));
  if (auto tupleTy = ty.getAs<TupleType>()) {
    SmallVector<SILValue, 4> elements;
    for (unsigned idx : range(tupleTy->getNumElements())) {
      SILType elementTy = ty.getTupleElementType(idx);
      auto element = createValueForEmptyType(elementTy, insertionPoint, ctx);
      elements.push_back(element);
    }
    SILBuilderWithScope builder(insertionPoint, ctx);
    return builder.createTuple(insertionPoint->getLoc(), ty, elements);
  } else if (auto *decl = ty.getStructOrBoundGenericStruct()) {
    TypeExpansionContext tec = *function;
    auto &module = function->getModule();
    if (decl->isResilient(tec.getContext()->getParentModule(),
                          tec.getResilienceExpansion())) {
      llvm::errs() << "Attempting to create value for illegal empty type:\n";
      ty.print(llvm::errs());
      llvm::report_fatal_error("illegal empty type: resilient struct");
    }
    SmallVector<SILValue, 4> elements;
    for (auto *field : decl->getStoredProperties()) {
      auto elementTy = ty.getFieldType(field, module, tec);
      auto element = createValueForEmptyType(elementTy, insertionPoint, ctx);
      elements.push_back(element);
    }
    SILBuilderWithScope builder(insertionPoint, ctx);
    return builder.createStruct(insertionPoint->getLoc(), ty, elements);
  }
  llvm::errs() << "Attempting to create value for illegal empty type:\n";
  ty.print(llvm::errs());
  llvm::report_fatal_error("illegal empty type: neither tuple nor struct.");
}

/// Whether lexical lifetimes should be added for the values stored into the
/// alloc_stack.
static bool lexicalLifetimeEnsured(AllocStackInst *asi) {
  return asi->getFunction()->hasOwnership() &&
         asi->getFunction()
                 ->getModule()
                 .getASTContext()
                 .SILOpts.LexicalLifetimes == LexicalLifetimesOption::On &&
         asi->isLexical() &&
         !asi->getElementType().isTrivial(*asi->getFunction());
}

static bool isGuaranteedLexicalValue(SILValue src) {
  return src->getOwnershipKind() == OwnershipKind::Guaranteed &&
         src->isLexical();
}

/// Returns true if we have enough information to end the lifetime.
static bool canEndLexicalLifetime(LiveValues values) {
  return values.canEndLexicalLifetime();
}

static SILValue getLexicalValueForStore(SILInstruction *inst,
                                        AllocStackInst *asi) {
  assert(isa<StoreInst>(inst) || isa<StoreBorrowInst>(inst));

  SILValue stored = inst->getOperand(CopyLikeInstruction::Src);
  LLVM_DEBUG(llvm::dbgs() << "*** Found Store def " << stored);

  if (!lexicalLifetimeEnsured(asi)) {
    return SILValue();
  }
  if (isa<StoreBorrowInst>(inst)) {
    if (isGuaranteedLexicalValue(stored)) {
      return SILValue();
    }
    auto borrow = cast<BeginBorrowInst>(inst->getNextInstruction());
    return borrow;
  }
  auto move = cast<MoveValueInst>(inst->getNextInstruction());
  return move;
}

/// Begin a lexical borrow scope for the value stored into the provided
/// StoreInst after that instruction.
///
/// The beginning of the scope looks like
///
///     %lifetime = move_value [lexical] %original
///
/// Because the value was consumed by the original store instruction, it can
/// be rewritten to be consumed by a lexical move_value.
static StorageStateTracking<LiveValues>
beginOwnedLexicalLifetimeAfterStore(AllocStackInst *asi, StoreInst *inst) {
  assert(lexicalLifetimeEnsured(asi));
  SILValue stored = inst->getOperand(CopyLikeInstruction::Src);
  SILLocation loc = RegularLocation::getAutoGeneratedLocation(inst->getLoc());

  MoveValueInst *mvi = nullptr;
  SILBuilderWithScope::insertAfter(inst, [&](SILBuilder &builder) {
    mvi = builder.createMoveValue(loc, stored, /*isLexical*/ true);
  });
  StorageStateTracking<LiveValues> vals = {LiveValues::forOwned(stored, mvi),
                                           /*isStorageValid=*/true};
  return vals;
}

/// Begin a lexical borrow scope for the value stored via the provided
/// StoreBorrowInst after that instruction.  Only do so if the stored value is
/// non-lexical.
static StorageStateTracking<LiveValues>
beginGuaranteedLexicalLifetimeAfterStore(AllocStackInst *asi,
                                         StoreBorrowInst *inst) {
  assert(lexicalLifetimeEnsured(asi));
  SILValue stored = inst->getOperand(CopyLikeInstruction::Src);
  SILLocation loc = RegularLocation::getAutoGeneratedLocation(inst->getLoc());

  if (isGuaranteedLexicalValue(stored)) {
    return {LiveValues::forGuaranteed(stored, {}), /*isStorageValid*/ true};
  }
  auto *borrow = SILBuilderWithScope(inst->getNextInstruction())
                     .createBeginBorrow(loc, stored, /*isLexical*/ true);
  return {LiveValues::forGuaranteed(stored, borrow), /*isStorageValid*/ true};
}

/// End the lexical borrow scope for an @owned stored value described by the
/// provided LiveValues struct before the specified instruction.
///
/// The end of the scope looks like
///
///     destroy_value %lifetime
///
/// This instruction corresponds to the following instructions that begin a
/// lexical borrow scope:
///
///     %lifetime = move_value [lexical] %original
///
/// However, no intervention is required to explicitly end the lifetime because
/// it will already have been ended naturally by destroy_addrs (or equivalent)
/// of the alloc_stack.
static void endOwnedLexicalLifetimeBeforeInst(AllocStackInst *asi,
                                              SILInstruction *beforeInstruction,
                                              SILBuilderContext &ctx,
                                              LiveValues::Owned values) {
  assert(lexicalLifetimeEnsured(asi));
  assert(beforeInstruction);
}

/// End the lexical borrow scope for an @guaranteed stored value described by
/// the provided LiveValues struct before the specified instruction.
static void endGuaranteedLexicalLifetimeBeforeInst(
    AllocStackInst *asi, SILInstruction *beforeInstruction,
    SILBuilderContext &ctx, LiveValues::Guaranteed values) {
  assert(lexicalLifetimeEnsured(asi));
  assert(beforeInstruction);
  assert(values.borrow);

  SILBuilderWithScope builder(beforeInstruction);
  builder.createEndBorrow(RegularLocation::getAutoGeneratedLocation(),
                          values.borrow);
}

//===----------------------------------------------------------------------===//
//                     Single Stack Allocation Promotion
//===----------------------------------------------------------------------===//

namespace {

/// Promotes a single AllocStackInst into registers..
class StackAllocationPromoter {
  using BlockSetVector = BasicBlockSetVector;
  using BlockToInstMap = llvm::DenseMap<SILBasicBlock *, SILInstruction *>;

  // Use a priority queue keyed on dominator tree level so that inserted nodes
  // are handled from the bottom of the dom tree upwards.
  using DomTreeNodePair = std::pair<DomTreeNode *, unsigned>;
  using NodePriorityQueue =
      std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                          llvm::less_second>;

  /// The AllocStackInst that we are handling.
  AllocStackInst *asi;

  /// The unique deallocation instruction. This value could be NULL if there are
  /// multiple deallocations.
  DeallocStackInst *dsi;

  /// Dominator info.
  DominanceInfo *domInfo;

  /// Map from dominator tree node to tree level.
  DomTreeLevelMap &domTreeLevels;

  /// The SIL builder used when creating new instructions during register
  /// promotion.
  SILBuilderContext &ctx;

  InstructionDeleter &deleter;

  /// Instructions that could not be deleted immediately with forceDelete until
  /// StackAllocationPromoter finishes its run.
  ///
  /// There are two reasons why an instruction might not be deleted:
  /// (1) new instructions are inserted before or after it
  /// (2) it ensures that an instruction remains used, preventing it from being
  ///     deleted
  SmallVectorImpl<SILInstruction *> &instructionsToDelete;

  /// The last instruction in each block that initializes the storage that is
  /// not succeeded by an instruction that deinitializes it.
  ///
  /// The live-out values for every block can be derived from these.
  ///
  /// This is either a StoreInst or a StoreBorrowInst.
  ///
  /// If the alloc_stack is non-lexical, the only live-out value is the source
  /// operand of the instruction.
  ///
  /// If the alloc_stack is lexical but the stored value is already lexical, no
  /// additional lexical lifetime is necessary and as an optimization can be
  /// omitted.  In that case, the only live-out value is the source operand of
  /// the instruction.  This optimization has been implemented for guaranteed
  /// alloc_stacks.
  ///
  /// If the alloc_stack is lexical and the stored value is not already lexical,
  /// a lexical lifetime must be introduced that matches the duration in which
  /// the value remains in the alloc_stack:
  /// - For owned alloc_stacks, a move_value [lexical] of the stored value is
  /// created.  That move_value is the instruction after the store, and it is
  /// the other running value.
  /// - For guaranteed alloc_stacks, a begin_borrow [lexical] of the
  /// store_borrow'd value is created.  That begin_borrow is the instruction
  /// after the store_borrow, and it is the other running value.
  BlockToInstMap initializationPoints;

  /// The first instruction in each block that deinitializes the storage that is
  /// not preceded by an instruction that initializes it.
  ///
  /// That includes:
  ///     store
  ///     destroy_addr
  ///     load [take]
  /// Or
  ///     end_borrow
  /// Ending lexical lifetimes before these instructions is one way that the
  /// cross-block lexical lifetimes of initializationPoints can be ended in
  /// StackAllocationPromoter::endLexicalLifetime.
  BlockToInstMap deinitializationPoints;

public:
  /// C'tor.
  StackAllocationPromoter(
      AllocStackInst *inputASI, DominanceInfo *inputDomInfo,
      DomTreeLevelMap &inputDomTreeLevels, SILBuilderContext &inputCtx,
      InstructionDeleter &deleter,
      SmallVectorImpl<SILInstruction *> &instructionsToDelete)
      : asi(inputASI), dsi(nullptr), domInfo(inputDomInfo),
        domTreeLevels(inputDomTreeLevels), ctx(inputCtx), deleter(deleter),
        instructionsToDelete(instructionsToDelete) {
    // Scan the users in search of a deallocation instruction.
    for (auto *use : asi->getUses()) {
      if (auto *foundDealloc = dyn_cast<DeallocStackInst>(use->getUser())) {
        // Don't record multiple dealloc instructions.
        if (dsi) {
          dsi = nullptr;
          break;
        }
        // Record the deallocation instruction.
        dsi = foundDealloc;
      }
    }
  }

  /// Promote the Allocation.
  void run();

private:
  /// Promote AllocStacks into SSA.
  void promoteAllocationToPhi(BlockSetVector &livePhiBlocks);

  /// Replace the dummy nodes with new block arguments.
  void addBlockArguments(BlockSetVector &phiBlocks);

  /// Check if \p phi is a proactively added phi by SILMem2Reg
  bool isProactivePhi(SILPhiArgument *phi, const BlockSetVector &phiBlocks);

  /// Check if \p proactivePhi is live.
  bool isNecessaryProactivePhi(SILPhiArgument *proactivePhi,
                               const BlockSetVector &phiBlocks);

  /// Given a \p proactivePhi that is live, backward propagate liveness to
  /// other proactivePhis.
  void propagateLiveness(SILPhiArgument *proactivePhi,
                         const BlockSetVector &phiBlocks,
                         SmallPtrSetImpl<SILPhiArgument *> &livePhis);

  /// End the lexical borrow scope that is introduced for lexical alloc_stack
  /// instructions.
  void endLexicalLifetime(BlockSetVector &phiBlocks);

  /// Fix all of the branch instructions and the uses to use
  /// the AllocStack definitions (which include stores and Phis).
  void fixBranchesAndUses(BlockSetVector &blocks, BlockSetVector &liveBlocks);

  /// update the branch instructions with the new Phi argument.
  /// The blocks in \p PhiBlocks are blocks that define a value, \p Dest is
  /// the branch destination, and \p Pred is the predecessors who's branch we
  /// modify.
  void fixPhiPredBlock(BlockSetVector &phiBlocks, SILBasicBlock *dest,
                       SILBasicBlock *pred);

  /// Get the values for this AllocStack variable that are flowing out of
  /// StartBB.
  llvm::Optional<LiveValues> getLiveOutValues(BlockSetVector &phiBlocks,
                                              SILBasicBlock *startBlock);

  /// Get the values for this AllocStack variable that are flowing out of
  /// StartBB or undef if there are none.
  LiveValues getEffectiveLiveOutValues(BlockSetVector &phiBlocks,
                                       SILBasicBlock *startBlock);

  /// Get the values for this AllocStack variable that are flowing into block.
  llvm::Optional<LiveValues> getLiveInValues(BlockSetVector &phiBlocks,
                                             SILBasicBlock *block);

  /// Get the values for this AllocStack variable that are flowing into block or
  /// undef if there are none.
  LiveValues getEffectiveLiveInValues(BlockSetVector &phiBlocks,
                                      SILBasicBlock *block);

  /// Prune AllocStacks usage in the function. Scan the function
  /// and remove in-block usage of the AllocStack. Leave only the first
  /// load and the last store.
  void pruneAllocStackUsage();

  /// Promote all of the AllocStacks in a single basic block in one
  /// linear scan. This function deletes all of the loads and stores except
  /// for the first load and the last store.
  /// \returns the last StoreInst found, whose storage was not subsequently
  ///          deinitialized
  SILInstruction *promoteAllocationInBlock(SILBasicBlock *block);
};

} // end of namespace

SILInstruction *StackAllocationPromoter::promoteAllocationInBlock(
    SILBasicBlock *blockPromotingWithin) {
  LLVM_DEBUG(llvm::dbgs() << "*** Promoting ASI in block: " << *asi);

  // RunningVal is the current value in the stack location.
  // We don't know the value of the alloca until we find the first store.
  //
  // States:
  // - None: no values have been encountered within this block
  // - Some + !isStorageValid: a value was encountered but is no longer stored--
  //                           it has been destroy_addr'd, etc
  // - Some + isStorageValid: a value was encountered and is currently stored
  llvm::Optional<StorageStateTracking<LiveValues>> runningVals;
  // The most recent StoreInst or StoreBorrowInst that encountered while
  // iterating over the block.  The final value will be returned to the caller
  // which will use it to determine the live-out value of the block.
  SILInstruction *lastStoreInst = nullptr;

  // For all instructions in the block.
  for (auto bbi = blockPromotingWithin->begin(),
            bbe = blockPromotingWithin->end();
       bbi != bbe;) {
    SILInstruction *inst = &*bbi;
    ++bbi;

    if (isLoadFromStack(inst, asi)) {
      assert(!runningVals || runningVals->isStorageValid);
      auto *li = dyn_cast<LoadInst>(inst);
      if (li && li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        if (lexicalLifetimeEnsured(asi)) {
          // End the lexical lifetime at a load [take].  The storage is no
          // longer keeping the value alive.
          if (runningVals && canEndLexicalLifetime(runningVals->value)) {
            // End it right now if we have enough information.
            endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/li,
                                              ctx,
                                              runningVals->value.getOwned());
          } else {
            // If we don't have enough information, end it endLexicalLifetime.
            assert(!deinitializationPoints[blockPromotingWithin]);
            deinitializationPoints[blockPromotingWithin] = li;
          }
        }
        if (runningVals)
          runningVals->isStorageValid = false;
      }

      if (runningVals) {
        // If we are loading from the AllocStackInst and we already know the
        // content of the Alloca then use it.
        LLVM_DEBUG(llvm::dbgs() << "*** Promoting load: " << *inst);
        replaceLoad(inst, runningVals->value.replacement(asi, inst), asi, ctx,
                    deleter, instructionsToDelete);
        ++NumInstRemoved;
      } else if (li && li->getOperand() == asi &&
                 li->getOwnershipQualifier() != LoadOwnershipQualifier::Copy) {
        // If we don't know the content of the AllocStack then the loaded
        // value *is* the new value;
        // Don't use result of load [copy] as a RunningVal, it necessitates
        // additional logic for cleanup of consuming instructions of the result.
        // StackAllocationPromoter::fixBranchesAndUses will later handle it.
        LLVM_DEBUG(llvm::dbgs() << "*** First load: " << *li);
        runningVals = {LiveValues::toReplace(asi, /*replacement=*/li),
                       /*isStorageValid=*/true};
      }
      continue;
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (auto *si = dyn_cast<StoreInst>(inst)) {
      if (si->getDest() != asi)
        continue;

      // If we see a store [assign], always convert it to a store [init]. This
      // simplifies further processing.
      if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
        if (runningVals) {
          assert(runningVals->isStorageValid);
          SILBuilderWithScope(si, ctx).createDestroyValue(
              si->getLoc(), runningVals->value.replacement(asi, si));
        } else {
          SILBuilderWithScope localBuilder(si, ctx);
          auto *newLoad = localBuilder.createLoad(si->getLoc(), asi,
                                                  LoadOwnershipQualifier::Take);
          localBuilder.createDestroyValue(si->getLoc(), newLoad);
          if (lexicalLifetimeEnsured(asi)) {
            assert(!deinitializationPoints[blockPromotingWithin]);
            deinitializationPoints[blockPromotingWithin] = si;
          }
        }
        si->setOwnershipQualifier(StoreOwnershipQualifier::Init);
      }

      // If we met a store before this one, delete it.
      if (lastStoreInst) {
        assert(cast<StoreInst>(lastStoreInst)->getOwnershipQualifier() !=
                   StoreOwnershipQualifier::Assign &&
               "store [assign] to the stack location should have been "
               "transformed to a store [init]");
        LLVM_DEBUG(llvm::dbgs()
                   << "*** Removing redundant store: " << lastStoreInst);
        ++NumInstRemoved;
        prepareForDeletion(lastStoreInst, instructionsToDelete);
      }

      auto oldRunningVals = runningVals;
      // The stored value is the new running value.
      runningVals = {LiveValues::toReplace(asi, /*replacement=*/si->getSrc()),
                     /*isStorageValid=*/true};
      // The current store is now the lastStoreInst (until we see
      // another).
      lastStoreInst = si;
      if (lexicalLifetimeEnsured(asi)) {
        if (oldRunningVals && oldRunningVals->isStorageValid &&
            canEndLexicalLifetime(oldRunningVals->value)) {
          endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/si, ctx,
                                            oldRunningVals->value.getOwned());
        }
        runningVals = beginOwnedLexicalLifetimeAfterStore(asi, si);
      }
      continue;
    }

    if (auto *sbi = dyn_cast<StoreBorrowInst>(inst)) {
      if (sbi->getDest() != asi)
        continue;

      // If we met a store before this one, delete it.
      if (lastStoreInst) {
        LLVM_DEBUG(llvm::dbgs()
                   << "*** Removing redundant store: " << lastStoreInst);
        ++NumInstRemoved;
        prepareForDeletion(lastStoreInst, instructionsToDelete);
      }

      // The stored value is the new running value.
      runningVals = {LiveValues::toReplace(asi, sbi->getSrc()),
                     /*isStorageValid=*/true};
      // The current store is now the lastStoreInst.
      lastStoreInst = sbi;
      if (lexicalLifetimeEnsured(asi)) {
        runningVals = beginGuaranteedLexicalLifetimeAfterStore(asi, sbi);
      }
      continue;
    }

    // End the lexical lifetime of the store_borrow source.
    if (auto *ebi = dyn_cast<EndBorrowInst>(inst)) {
      if (!lexicalLifetimeEnsured(asi)) {
        continue;
      }
      auto *sbi = dyn_cast<StoreBorrowInst>(ebi->getOperand());
      if (!sbi) {
        continue;
      }
      if (sbi->getDest() != asi) {
        continue;
      }
      assert(!deinitializationPoints[blockPromotingWithin]);
      deinitializationPoints[blockPromotingWithin] = inst;
      if (!runningVals.has_value()) {
        continue;
      }
      if (!runningVals->value.isGuaranteed()) {
        continue;
      }
      if (sbi->getSrc() != runningVals->value.getGuaranteed().stored) {
        continue;
      }
      // Mark storage as invalid and mark end_borrow as a deinit point.
      runningVals->isStorageValid = false;
      if (!canEndLexicalLifetime(runningVals->value)) {
        continue;
      }
      endGuaranteedLexicalLifetimeBeforeInst(
          asi, ebi->getNextInstruction(), ctx,
          runningVals->value.getGuaranteed());
      continue;
    }

    // Replace debug_value w/ address value with debug_value of
    // the promoted value.
    // if we have a valid value to use at this point. Otherwise we'll
    // promote this when we deal with hooking up phis.
    if (auto *dvi = DebugValueInst::hasAddrVal(inst)) {
      if (isDebugValueOfAllocStack(dvi, asi) && runningVals)
        promoteDebugValueAddr(dvi, runningVals->value.replacement(asi, dvi),
                              ctx, deleter);
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(inst)) {
      if (dai->getOperand() != asi) {
        continue;
      }
      if (runningVals) {
        replaceDestroy(dai, runningVals->value.replacement(asi, dai), ctx,
                       deleter, instructionsToDelete);
        if (lexicalLifetimeEnsured(asi)) {
          endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/dai, ctx,
                                            runningVals->value.getOwned());
        }
        runningVals->isStorageValid = false;
      } else {
        assert(!deinitializationPoints[blockPromotingWithin]);
        deinitializationPoints[blockPromotingWithin] = dai;
      }
      continue;
    }

    // Stop on deallocation.
    if (auto *dsi = dyn_cast<DeallocStackInst>(inst)) {
      if (dsi->getOperand() == asi)
        break;
    }
  }

  if (lastStoreInst && runningVals->isStorageValid) {
    assert((isa<StoreBorrowInst>(lastStoreInst) ||
            (cast<StoreInst>(lastStoreInst)->getOwnershipQualifier() !=
             StoreOwnershipQualifier::Assign)) &&
           "store [assign] to the stack location should have been "
           "transformed to a store [init]");
    LLVM_DEBUG(llvm::dbgs()
               << "*** Finished promotion. Last store: " << lastStoreInst);
    return lastStoreInst;
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Finished promotion with no stores.\n");
  return nullptr;
}

void StackAllocationPromoter::addBlockArguments(BlockSetVector &phiBlocks) {
  LLVM_DEBUG(llvm::dbgs() << "*** Adding new block arguments.\n");

  for (auto *block : phiBlocks) {
    // The stored value or its lexical move.
    block->createPhiArgument(asi->getElementType(), OwnershipKind::Owned);
  }
}

llvm::Optional<LiveValues>
StackAllocationPromoter::getLiveOutValues(BlockSetVector &phiBlocks,
                                          SILBasicBlock *startBlock) {
  LLVM_DEBUG(llvm::dbgs() << "*** Searching for a value definition.\n");
  // Walk the Dom tree in search of a defining value:
  for (DomTreeNode *domNode = domInfo->getNode(startBlock); domNode;
       domNode = domNode->getIDom()) {
    SILBasicBlock *domBlock = domNode->getBlock();

    // If there is a store (that must come after the phi), use its value.
    BlockToInstMap::iterator it = initializationPoints.find(domBlock);
    if (it != initializationPoints.end()) {
      auto *inst = it->second;
      auto stored = inst->getOperand(CopyLikeInstruction::Src);
      auto lexical = getLexicalValueForStore(inst, asi);
      return LiveValues::forValues(stored, lexical);
    }

    // If there is a Phi definition in this block:
    if (phiBlocks.contains(domBlock)) {
      // Return the dummy instruction that represents the new value that we will
      // add to the basic block.
      SILValue argument =
          domBlock->getArgument(domBlock->getNumArguments() - 1);
      LLVM_DEBUG(llvm::dbgs() << "*** Found a dummy Phi def " << *argument);
      auto values = LiveValues::toReplace(asi, argument);
      return values;
    }

    // Move to the next dominating block.
    LLVM_DEBUG(llvm::dbgs() << "*** Walking up the iDOM.\n");
  }
  LLVM_DEBUG(llvm::dbgs() << "*** Could not find a Def. Using Undef.\n");
  return llvm::None;
}

LiveValues
StackAllocationPromoter::getEffectiveLiveOutValues(BlockSetVector &phiBlocks,
                                                   SILBasicBlock *startBlock) {
  if (auto values = getLiveOutValues(phiBlocks, startBlock)) {
    return *values;
  }
  auto *undef = SILUndef::get(asi->getElementType(), *asi->getFunction());
  return LiveValues::forOwned(undef, undef);
}

llvm::Optional<LiveValues>
StackAllocationPromoter::getLiveInValues(BlockSetVector &phiBlocks,
                                         SILBasicBlock *block) {
  // First, check if there is a Phi value in the current block. We know that
  // our loads happen before stores, so we need to first check for Phi nodes
  // in the first block, but stores first in all other stores in the idom
  // chain.
  if (phiBlocks.contains(block)) {
    LLVM_DEBUG(llvm::dbgs() << "*** Found a local Phi definition.\n");
    SILValue argument = block->getArgument(block->getNumArguments() - 1);
    auto values = LiveValues::toReplace(asi, argument);
    return values;
  }

  if (block->pred_empty() || !domInfo->getNode(block))
    return llvm::None;

  // No phi for this value in this block means that the value flowing
  // out of the immediate dominator reaches here.
  DomTreeNode *iDom = domInfo->getNode(block)->getIDom();
  assert(iDom &&
         "Attempt to get live-in value for alloc_stack in entry block!");

  return getLiveOutValues(phiBlocks, iDom->getBlock());
}

LiveValues
StackAllocationPromoter::getEffectiveLiveInValues(BlockSetVector &phiBlocks,
                                                  SILBasicBlock *block) {
  if (auto values = getLiveInValues(phiBlocks, block)) {
    return *values;
  }
  auto *undef = SILUndef::get(asi->getElementType(), *asi->getFunction());
  // TODO: Add another kind of LiveValues for undef.
  return LiveValues::forOwned(undef, undef);
}

void StackAllocationPromoter::fixPhiPredBlock(BlockSetVector &phiBlocks,
                                              SILBasicBlock *destBlock,
                                              SILBasicBlock *predBlock) {
  TermInst *ti = predBlock->getTerminator();
  LLVM_DEBUG(llvm::dbgs() << "*** Fixing the terminator " << *ti << ".\n");

  LiveValues values = getEffectiveLiveOutValues(phiBlocks, predBlock);

  LLVM_DEBUG(llvm::dbgs() << "*** Found the definition: "
                          << values.getStored());

  SmallVector<SILValue> vals;
  vals.push_back(values.replacement(asi, nullptr));

  addArgumentsToBranch(vals, destBlock, ti);
  deleter.forceDelete(ti);
}

bool StackAllocationPromoter::isProactivePhi(SILPhiArgument *phi,
                                             const BlockSetVector &phiBlocks) {
  auto *phiBlock = phi->getParentBlock();
  return phiBlocks.contains(phiBlock) &&
         phi == phiBlock->getArgument(phiBlock->getNumArguments() - 1);
}

bool StackAllocationPromoter::isNecessaryProactivePhi(
    SILPhiArgument *proactivePhi, const BlockSetVector &phiBlocks) {
  assert(isProactivePhi(proactivePhi, phiBlocks));
  for (auto *use : proactivePhi->getUses()) {
    auto *branch = dyn_cast<BranchInst>(use->getUser());
    // A non-branch use is a necessary use
    if (!branch)
      return true;
    auto *destBB = branch->getDestBB();
    auto opNum = use->getOperandNumber();
    // A phi has a necessary use if it is used as a branch op for a
    // non-proactive phi
    if (!phiBlocks.contains(destBB) || (opNum != branch->getNumArgs() - 1))
      return true;
  }
  return false;
}

void StackAllocationPromoter::propagateLiveness(
    SILPhiArgument *proactivePhi, const BlockSetVector &phiBlocks,
    SmallPtrSetImpl<SILPhiArgument *> &livePhis) {
  assert(isProactivePhi(proactivePhi, phiBlocks));
  if (livePhis.contains(proactivePhi))
    return;
  // If liveness has not been propagated, go over the incoming operands and mark
  // any operand values that are proactivePhis as live
  livePhis.insert(proactivePhi);
  SmallVector<SILValue> incomingPhiVals;
  proactivePhi->getIncomingPhiValues(incomingPhiVals);
  for (auto &inVal : incomingPhiVals) {
    auto *inPhi = dyn_cast<SILPhiArgument>(inVal);
    if (!inPhi)
      continue;
    if (!isProactivePhi(inPhi, phiBlocks))
      continue;
    propagateLiveness(inPhi, phiBlocks, livePhis);
  }
}

void StackAllocationPromoter::fixBranchesAndUses(BlockSetVector &phiBlocks,
                                                 BlockSetVector &phiBlocksOut) {
  // First update uses of the value.
  SmallVector<SILInstruction *, 4> collectedLoads;
  // Collect all alloc_stack uses.
  SmallVector<Operand *, 4> uses(asi->getUses());

  // Collect uses of store_borrows to alloc_stack.
  for (unsigned i = 0; i < uses.size(); i++) {
    auto *use = uses[i];
    if (auto *sbi = dyn_cast<StoreBorrowInst>(use->getUser())) {
      for (auto *sbuse : sbi->getUses()) {
        uses.push_back(sbuse);
      }
    }
  }

  for (auto ui = uses.begin(), ue = uses.end(); ui != ue;) {
    auto *user = (*ui)->getUser();
    ++ui;
    bool removedUser = false;

    collectedLoads.clear();
    collectLoads(user, collectedLoads);
    for (auto *li : collectedLoads) {
      // If this block has no predecessors then nothing dominates it and
      // the instruction is unreachable. If the instruction we're
      // examining is a value, replace it with undef. Either way, delete
      // the instruction and move on.
      SILBasicBlock *loadBlock = li->getParent();
      auto def = getEffectiveLiveInValues(phiBlocks, loadBlock);

      LLVM_DEBUG(llvm::dbgs() << "*** Replacing " << *li << " with Def "
                              << def.replacement(asi, li));

      // Replace the load with the definition that we found.
      replaceLoad(li, def.replacement(asi, li), asi, ctx, deleter,
                  instructionsToDelete);
      removedUser = true;
      ++NumInstRemoved;
    }

    if (removedUser)
      continue;

    // If this block has no predecessors then nothing dominates it and
    // the instruction is unreachable. Delete the instruction and move
    // on.
    SILBasicBlock *userBlock = user->getParent();

    if (auto *dvi = DebugValueInst::hasAddrVal(user)) {
      // Replace debug_value w/ address-type value with
      // a new debug_value w/ promoted value.
      auto def = getEffectiveLiveInValues(phiBlocks, userBlock);
      promoteDebugValueAddr(dvi, def.replacement(asi, dvi), ctx, deleter);
      ++NumInstRemoved;
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(user)) {
      auto def = getEffectiveLiveInValues(phiBlocks, userBlock);
      replaceDestroy(dai, def.replacement(asi, dai), ctx, deleter,
                     instructionsToDelete);
      continue;
    }
  }

  // Now that all of the uses are fixed we can fix the branches that point
  // to the blocks with the added arguments.
  // For each Block with a new Phi argument:
  for (auto *block : phiBlocks) {
    // Fix all predecessors.
    for (auto pbbi = block->getPredecessorBlocks().begin(),
              pbbe = block->getPredecessorBlocks().end();
         pbbi != pbbe;) {
      auto *predBlock = *pbbi;
      ++pbbi;
      assert(predBlock && "Invalid block!");
      fixPhiPredBlock(phiBlocks, block, predBlock);
    }
  }

  // Fix ownership of proactively created non-trivial phis
  if (asi->getFunction()->hasOwnership() &&
      !asi->getType().isTrivial(*asi->getFunction())) {
    SmallPtrSet<SILPhiArgument *, 4> livePhis;

    for (auto *block : phiBlocks) {
      auto *proactivePhi = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      // First, check if the proactively added phi is necessary by looking at
      // it's immediate uses.
      if (isNecessaryProactivePhi(proactivePhi, phiBlocks)) {
        // Backward propagate liveness to other dependent proactively added phis
        propagateLiveness(proactivePhi, phiBlocks, livePhis);
      }
    }
    // Go over all proactively added phis, and delete those that were not marked
    // live above.
    auto eraseLastPhiFromBlock = [](SILBasicBlock *block) {
      auto *phi = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      phi->replaceAllUsesWithUndef();
      erasePhiArgument(block, block->getNumArguments() - 1,
                       /*cleanupDeadPhiOp*/ false);
    };
    for (auto *block : phiBlocks) {
      auto *proactivePhi = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      if (!livePhis.contains(proactivePhi)) {
        eraseLastPhiFromBlock(block);
      } else {
        phiBlocksOut.insert(block);
      }
    }
  } else {
    for (auto *block : phiBlocks)
      phiBlocksOut.insert(block);
  }
}

/// End the lexical lifetimes that were introduced for storage to the
/// alloc_stack and have not already been ended.
///
/// Walk forward from the out-edge of each of the blocks which began but did not
/// end a borrow scope.  The scope must be ended if any of the following three
/// conditions hold:
///
/// Normally, we are relying on the invariant that the storage's
/// deinitializations must jointly postdominate its initializations.  That fact
/// allows us to simply end scopes when memory is deinitialized.  There is only
/// one simple check to do:
///
/// (1) A block deinitializes the storage before initializing it.
///
///     These blocks and the relevant instruction within them are tracked by the
///     deinitializationPoints map.
///
/// If this were all we needed to do, we could just iterate over that map.
///
/// The above invariant does not help us with unreachable terminators, however.
/// Because it is valid to have the alloc_stack be initialized when exiting a
/// function via an unreachable, we can't rely on the memory having been
/// deinitialized.  But we still need to ensure that borrow scopes are ended and
/// values are destroyed before getting to an unreachable.
///
/// (2.a) A block has as its terminator an UnreachableInst.
///
/// (2.b) A block's single successor does not have live-in values.
///
///       This can only happen if the successor is a CFG merge and all paths
///       from here lead to unreachable.
void StackAllocationPromoter::endLexicalLifetime(BlockSetVector &phiBlocks) {
  if (!lexicalLifetimeEnsured(asi))
    return;

  // We need to separately consider and visit incoming unopened borrow scopes
  // and outgoing unclosed borrow scopes.  The reason is that a walk should stop
  // on any path where it encounters an incoming unopened borrow scope but that
  // should _NOT_ count as a visit of outgoing unclosed borrow scopes.
  //
  // Without this distinction, a case like the following wouldn't be visited
  // properly:
  //
  //     bb1:
  //       %addr = alloc_stack
  //       store %value to [init] %addr
  //       br bb2
  //     bb2:
  //       %value_2 = load [take] %addr
  //       store %value_2 to [init] %addr
  //       br bb3
  //     bb3:
  //       destroy_addr %addr
  //       dealloc_stack %addr
  //       %r = tuple ()
  //       return %r
  //
  // Both bb1 and bb2 have cross-block initialization points.  Suppose that we
  // visited bb1 first.  We would see that it didn't have an incoming unopened
  // borrow scope (already, we can tell something is amiss that we're
  // considering this) and then add bb2 to the worklist--except it's already
  // there.  Next we would visit bb2.  We would see that it had an incoming
  // unopened borrow scope so we would close it.  And then we'd be done.  In
  // particular, we'd leave the scope that opens in bb2 unclosed.
  //
  // The root cause here is that it's important to stop walking when we hit a
  // scope close.  Otherwise, we could keep walking down to blocks which don't
  // have live-in or live-out values.
  //
  // Visiting the incoming and outgoing edges works as follows in the above
  // example:  The worklist is initialized with {(bb1, ::Out), (bb2, ::Out)}.
  // When visiting (bb1, ::Out), we see that bb1 is neither unreachable nor
  // has exactly one successor without live-in values.  So we add (bb2, ::In) to
  // the worklist.  Next, we visit (bb2, ::Out).  We see that it _also_ doesn't
  // have an unreachable terminator or a unique successor without live-in
  // values, so we add (bb3, ::In).  Next, we visit (bb2, ::In).  We see that
  // it _does_ have an incoming unopened borrow scope, so we close it and stop.
  // Finally, we visit (bb3, ::Out).  We see that it too has an incoming
  // unopened borrow scope so we close it and stop.
  enum class AvailableValuesKind : uint8_t { In, Out };

  using ScopeEndPosition =
      llvm::PointerIntPair<SILBasicBlock *, 1, AvailableValuesKind>;

  GraphNodeWorklist<ScopeEndPosition, 16> worklist;
  for (auto pair : initializationPoints) {
    worklist.insert({pair.getFirst(), AvailableValuesKind::Out});
  }
  while (!worklist.empty()) {
    auto position = worklist.pop();
    auto *bb = position.getPointer();
    switch (position.getInt()) {
    case AvailableValuesKind::In: {
      if (auto *inst = deinitializationPoints[bb]) {
        auto values = getLiveInValues(phiBlocks, bb);
        if (isa<EndBorrowInst>(inst)) {
          // Not all store_borrows will have a begin_borrow [lexical] that needs
          // to be ended. If the source is already lexical, we don't create it.
          if (!canEndLexicalLifetime(*values)) {
            continue;
          }
          endGuaranteedLexicalLifetimeBeforeInst(
              asi, /*beforeInstruction=*/inst, ctx, values->getGuaranteed());
          continue;
        }
        endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/inst, ctx,
                                          values->getOwned());
        continue;
      }
      worklist.insert({bb, AvailableValuesKind::Out});
      break;
    }
    case AvailableValuesKind::Out: {
      bool terminatesInUnreachable = isa<UnreachableInst>(bb->getTerminator());
      auto uniqueSuccessorLacksLiveInValues = [&]() -> bool {
        return bb->getSingleSuccessorBlock() &&
               !getLiveInValues(phiBlocks, bb->getSingleSuccessorBlock());
      };
      if (terminatesInUnreachable || uniqueSuccessorLacksLiveInValues()) {
        auto values = getLiveOutValues(phiBlocks, bb);
        if (values->isGuaranteed()) {
          if (!canEndLexicalLifetime(*values)) {
            continue;
          }
          endGuaranteedLexicalLifetimeBeforeInst(
              asi, /*beforeInstruction=*/bb->getTerminator(), ctx,
              values->getGuaranteed());
          continue;
        }
        endOwnedLexicalLifetimeBeforeInst(
            asi, /*beforeInstruction=*/bb->getTerminator(), ctx,
            values->getOwned());
        continue;
      }
      for (auto *successor : bb->getSuccessorBlocks()) {
        worklist.insert({successor, AvailableValuesKind::In});
      }
      break;
    }
    }
  }
}

void StackAllocationPromoter::pruneAllocStackUsage() {
  LLVM_DEBUG(llvm::dbgs() << "*** Pruning : " << *asi);
  BlockSetVector functionBlocks(asi->getFunction());

  // Insert all of the blocks that asi is live in.
  for (auto *use : asi->getUses())
    functionBlocks.insert(use->getUser()->getParent());

  for (auto *sbi : asi->getUsersOfType<StoreBorrowInst>()) {
    for (auto *use : sbi->getUses()) {
      functionBlocks.insert(use->getUser()->getParent());
    }
  }

  for (auto block : functionBlocks)
    if (auto si = promoteAllocationInBlock(block)) {
      // There was a final store/store_borrow instruction which was not
      // followed by an instruction that deinitializes the memory.  Record it
      // as a cross-block initialization point.
      initializationPoints[block] = si;
    }

  LLVM_DEBUG(llvm::dbgs() << "*** Finished pruning : " << *asi);
}

void StackAllocationPromoter::promoteAllocationToPhi(
    BlockSetVector &livePhiBlocks) {
  LLVM_DEBUG(llvm::dbgs() << "*** Placing Phis for : " << *asi);

  // A list of blocks that will require new Phi values.
  BlockSetVector phiBlocks(asi->getFunction());

  // The "piggy-bank" data-structure that we use for processing the dom-tree
  // bottom-up.
  NodePriorityQueue priorityQueue;

  // Collect all of the stores into the AllocStack. We know that at this point
  // we have at most one store per block.
  for (auto *use : asi->getUses()) {
    SILInstruction *user = use->getUser();
    // We need to place Phis for this block.
    if (isa<StoreInst>(user) || isa<StoreBorrowInst>(user)) {
      // If the block is in the dom tree (dominated by the entry block).
      if (auto *node = domInfo->getNode(user->getParent()))
        priorityQueue.push(std::make_pair(node, domTreeLevels[node]));
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Found: " << priorityQueue.size()
                          << " Defs\n");

  // A list of nodes for which we already calculated the dominator frontier.
  llvm::SmallPtrSet<DomTreeNode *, 32> visited;

  SmallVector<DomTreeNode *, 32> worklist;

  // Scan all of the definitions in the function bottom-up using the priority
  // queue.
  while (!priorityQueue.empty()) {
    DomTreeNodePair rootPair = priorityQueue.top();
    priorityQueue.pop();
    DomTreeNode *root = rootPair.first;
    unsigned rootLevel = rootPair.second;

    // Walk all dom tree children of Root, inspecting their successors. Only
    // J-edges, whose target level is at most Root's level are added to the
    // dominance frontier.
    worklist.clear();
    worklist.push_back(root);

    while (!worklist.empty()) {
      DomTreeNode *node = worklist.pop_back_val();
      SILBasicBlock *nodeBlock = node->getBlock();

      // For all successors of the node:
      for (auto &nodeBlockSuccs : nodeBlock->getSuccessors()) {
        auto *successorNode = domInfo->getNode(nodeBlockSuccs);

        // Skip D-edges (edges that are dom-tree edges).
        if (successorNode->getIDom() == node)
          continue;

        // Ignore J-edges that point to nodes that are not smaller or equal
        // to the root level.
        unsigned succLevel = domTreeLevels[successorNode];
        if (succLevel > rootLevel)
          continue;

        // Ignore visited nodes.
        if (!visited.insert(successorNode).second)
          continue;

        // If the new PHInode is not dominated by the allocation then it's dead.
        if (!domInfo->dominates(asi->getParent(), successorNode->getBlock()))
          continue;

        // If the new PHInode is properly dominated by the deallocation then it
        // is obviously a dead PHInode, so we don't need to insert it.
        if (dsi && domInfo->properlyDominates(dsi->getParent(),
                                              successorNode->getBlock()))
          continue;

        // The successor node is a new PHINode. If this is a new PHI node
        // then it may require additional definitions, so add it to the PQ.
        if (phiBlocks.insert(nodeBlockSuccs))
          priorityQueue.push(std::make_pair(successorNode, succLevel));
      }

      // Add the children in the dom-tree to the worklist.
      for (auto *child : node->children())
        if (!visited.count(child))
          worklist.push_back(child);
    }
  }

  // At this point we calculated the locations of all of the new Phi values.
  // Next, add the Phi values and promote all of the loads and stores into the
  // new locations.

  // Replace the dummy values with new block arguments.
  addBlockArguments(phiBlocks);

  // Hook up the Phi nodes, loads, and debug_value_addr with incoming values.
  fixBranchesAndUses(phiBlocks, livePhiBlocks);

  endLexicalLifetime(livePhiBlocks);

  LLVM_DEBUG(llvm::dbgs() << "*** Finished placing Phis ***\n");
}

void StackAllocationPromoter::run() {
  auto *function = asi->getFunction();

  // Reduce the number of load/stores in the function to minimum.
  // After this phase we are left with up to one load and store
  // per block and the last store is recorded.
  pruneAllocStackUsage();

  // The blocks which still have new phis after fixBranchesAndUses runs.  These
  // are not necessarily the same as phiBlocks because fixBranchesAndUses
  // removes superfluous proactive phis.
  BlockSetVector livePhiBlocks(asi->getFunction());

  // Replace AllocStacks with Phi-nodes.
  promoteAllocationToPhi(livePhiBlocks);

  // Make sure that all of the allocations were promoted into registers.
  assert(isWriteOnlyAllocation(asi) && "Non-write uses left behind");

  SmallVector<SILValue> valuesToComplete;

  // Enum types may have incomplete lifetimes in address form, when promoted to
  // value form after mem2reg, they will end up with incomplete ossa lifetimes.
  // Use the lifetime completion utility to complete such lifetimes.
  // First, collect the stored values to complete.
  if (asi->getType().isOrHasEnum()) {
    for (auto *block : livePhiBlocks) {
      SILPhiArgument *argument = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      assert(argument->isPhi());
      valuesToComplete.push_back(argument);
    }
    for (auto it : initializationPoints) {
      auto *si = it.second;
      auto stored = si->getOperand(CopyLikeInstruction::Src);
      valuesToComplete.push_back(stored);
      if (auto lexical = getLexicalValueForStore(si, asi)) {
        valuesToComplete.push_back(lexical);
      }
    }
  }

  // ... and erase the allocation.
  deleter.forceDeleteWithUsers(asi);

  // Now, complete lifetimes!
  OSSALifetimeCompletion completion(function, domInfo);

  // We may have incomplete lifetimes for enum locations on trivial paths.
  // After promoting them, complete lifetime here.
  for (auto it : valuesToComplete) {
    // Set forceBoundaryCompletion as true so that we complete at boundary for
    // lexical values as well.
    completion.completeOSSALifetime(it, /* forceBoundaryCompletion */ true);
  }
}

//===----------------------------------------------------------------------===//
//                      General Memory To Registers Impl
//===----------------------------------------------------------------------===//

namespace {

/// Promote memory to registers
class MemoryToRegisters {
  /// Lazily initialized map from DomTreeNode to DomTreeLevel.
  ///
  /// DomTreeLevelMap is a DenseMap implying that if we initialize it, we always
  /// will initialize a heap object with 64 objects. Thus by using an optional,
  /// computing this lazily, we only do this if we actually need to do so.
  llvm::Optional<DomTreeLevelMap> domTreeLevels;

  /// The function that we are optimizing.
  SILFunction &f;

  /// Dominators.
  DominanceInfo *domInfo;

  /// The builder context used when creating new instructions during register
  /// promotion.
  SILBuilderContext ctx;

  InstructionDeleter deleter;
  SmallVector<SILInstruction *, 32> instructionsToDelete;

  /// Returns the dom tree levels for the current function. Computes these
  /// lazily.
  DomTreeLevelMap &getDomTreeLevels() {
    // If we already computed our levels, just return it.
    if (auto &levels = domTreeLevels) {
      return *levels;
    }

    // Otherwise, emplace the map and compute it.
    domTreeLevels.emplace();
    auto &levels = *domTreeLevels;
    SmallVector<DomTreeNode *, 32> worklist;
    DomTreeNode *rootNode = domInfo->getRootNode();
    levels[rootNode] = 0;
    worklist.push_back(rootNode);
    while (!worklist.empty()) {
      DomTreeNode *domNode = worklist.pop_back_val();
      unsigned childLevel = levels[domNode] + 1;
      for (auto *childNode : domNode->children()) {
        levels[childNode] = childLevel;
        worklist.push_back(childNode);
      }
    }
    return *domTreeLevels;
  }

  /// Promote all of the AllocStacks in a single basic block in one
  /// linear scan. Note: This function deletes all of the users of the
  /// AllocStackInst, including the DeallocStackInst but it does not remove the
  /// AllocStackInst itself!
  void removeSingleBlockAllocation(AllocStackInst *asi);

  /// Attempt to promote the specified stack allocation, returning true if so
  /// or false if not.  On success, all uses of the AllocStackInst have been
  /// removed, but the ASI itself is still in the program.
  bool promoteSingleAllocation(AllocStackInst *asi);

public:
  /// C'tor
  MemoryToRegisters(SILFunction &inputFunc, DominanceInfo *inputDomInfo)
      : f(inputFunc), domInfo(inputDomInfo), ctx(inputFunc.getModule()) {}

  /// Promote memory to registers. Return True on change.
  bool run();
};

} // end anonymous namespace

void MemoryToRegisters::removeSingleBlockAllocation(AllocStackInst *asi) {
  LLVM_DEBUG(llvm::dbgs() << "*** Promoting in-block: " << *asi);

  SILBasicBlock *parentBlock = asi->getParent();
  // The default value of the AllocStack is NULL because we don't have
  // uninitialized variables in Swift.
  llvm::Optional<StorageStateTracking<LiveValues>> runningVals;

  // For all instructions in the block.
  for (auto bbi = parentBlock->begin(), bbe = parentBlock->end(); bbi != bbe;) {
    SILInstruction *inst = &*bbi;
    ++bbi;

    // Remove instructions that we are loading from. Replace the loaded value
    // with our running value.
    if (isLoadFromStack(inst, asi)) {
      if (!runningVals) {
        // Loading from uninitialized memory is only acceptable if the type is
        // empty--an aggregate of types without storage.
        runningVals = {
            LiveValues::toReplace(asi,
                                  /*replacement=*/createValueForEmptyType(
                                      asi->getElementType(), inst, ctx)),
            /*isStorageValid=*/true};
      }
      assert(runningVals && runningVals->isStorageValid);
      auto *loadInst = dyn_cast<LoadInst>(inst);
      if (loadInst &&
          loadInst->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        if (lexicalLifetimeEnsured(asi)) {
          // End the lexical lifetime at a load [take].  The storage is no
          // longer keeping the value alive.
          endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/inst,
                                            ctx, runningVals->value.getOwned());
        }
        runningVals->isStorageValid = false;
      }
      replaceLoad(inst, runningVals->value.replacement(asi, inst), asi, ctx,
                  deleter, instructionsToDelete);
      ++NumInstRemoved;
      continue;
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (auto *si = dyn_cast<StoreInst>(inst)) {
      if (si->getDest() != asi) {
        continue;
      }
      if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
        assert(runningVals && runningVals->isStorageValid);
        SILBuilderWithScope(si, ctx).createDestroyValue(
            si->getLoc(), runningVals->value.replacement(asi, si));
      }
      auto oldRunningVals = runningVals;
      runningVals = {LiveValues::toReplace(asi, /*replacement=*/si->getSrc()),
                     /*isStorageValid=*/true};
      if (lexicalLifetimeEnsured(asi)) {
        if (oldRunningVals && oldRunningVals->isStorageValid) {
          endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/si, ctx,
                                            oldRunningVals->value.getOwned());
        }
        runningVals = beginOwnedLexicalLifetimeAfterStore(asi, si);
      }
      deleter.forceDelete(si);
      ++NumInstRemoved;
      continue;
    }

    if (auto *sbi = dyn_cast<StoreBorrowInst>(inst)) {
      if (sbi->getDest() != asi) {
        continue;
      }
      runningVals = {LiveValues::toReplace(asi, /*replacement=*/sbi->getSrc()),
                     /*isStorageValid=*/true};
      if (lexicalLifetimeEnsured(asi)) {
        runningVals = beginGuaranteedLexicalLifetimeAfterStore(asi, sbi);
      }
      continue;
    }

    if (auto *ebi = dyn_cast<EndBorrowInst>(inst)) {
      auto *sbi = dyn_cast<StoreBorrowInst>(ebi->getOperand());
      if (!sbi) {
        continue;
      }
      if (sbi->getDest() != asi) {
        continue;
      }
      if (!runningVals.has_value()) {
        continue;
      }
      if (!runningVals->value.isGuaranteed()) {
        continue;
      }
      if (sbi->getSrc() != runningVals->value.getGuaranteed().stored) {
        continue;
      }
      runningVals->isStorageValid = false;
      if (!canEndLexicalLifetime(runningVals->value)) {
        continue;
      }
      endGuaranteedLexicalLifetimeBeforeInst(
          asi, ebi->getNextInstruction(), ctx,
          runningVals->value.getGuaranteed());
      continue;
    }

    // Replace debug_value w/ address value with debug_value of
    // the promoted value.
    if (auto *dvi = DebugValueInst::hasAddrVal(inst)) {
      if (isDebugValueOfAllocStack(dvi, asi)) {
        if (runningVals) {
          promoteDebugValueAddr(dvi, runningVals->value.replacement(asi, dvi),
                                ctx, deleter);
        } else {
          // Drop debug_value of uninitialized void values.
          assert(asi->getElementType().isVoid() &&
                 "Expected initialization of non-void type!");
          deleter.forceDelete(dvi);
        }
      }
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(inst)) {
      if (dai->getOperand() == asi) {
        assert(runningVals && runningVals->isStorageValid);
        replaceDestroy(dai, runningVals->value.replacement(asi, dai), ctx,
                       deleter, instructionsToDelete);
        if (lexicalLifetimeEnsured(asi)) {
          endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/dai, ctx,
                                            runningVals->value.getOwned());
        }
        runningVals->isStorageValid = false;
      }
      continue;
    }

    // Remove deallocation.
    if (auto *dsi = dyn_cast<DeallocStackInst>(inst)) {
      if (dsi->getOperand() == asi) {
        deleter.forceDelete(dsi);
        NumInstRemoved++;
        // No need to continue scanning after deallocation.
        break;
      }
    }

    // Remove dead address instructions that may be uses of the allocation.
    auto *addrInst = dyn_cast<SingleValueInstruction>(inst);
    while (addrInst && addrInst->use_empty() &&
           (isa<StructElementAddrInst>(addrInst) ||
            isa<TupleElementAddrInst>(addrInst) ||
            isa<UncheckedAddrCastInst>(addrInst))) {
      SILValue op = addrInst->getOperand(0);
      deleter.forceDelete(addrInst);
      ++NumInstRemoved;
      addrInst = dyn_cast<SingleValueInstruction>(op);
    }
  }

  if (lexicalLifetimeEnsured(asi) && runningVals &&
      runningVals->isStorageValid &&
      runningVals->value.getStored()->getOwnershipKind().isCompatibleWith(
          OwnershipKind::Owned)) {
    // There is still valid storage after visiting all instructions in this
    // block which are the only instructions involving this alloc_stack.
    // This can only happen if all paths from this block end in unreachable.
    //
    // We need to end the lexical lifetime at the last possible location, at the
    // boundary blocks which are the predecessors of dominance frontier
    // dominated by the alloc_stack.
    SmallVector<SILBasicBlock *, 4> boundary;
    computeDominatedBoundaryBlocks(asi->getParent(), domInfo, boundary);
    for (auto *block : boundary) {
      auto *terminator = block->getTerminator();
      endOwnedLexicalLifetimeBeforeInst(asi, /*beforeInstruction=*/terminator,
                                        ctx, runningVals->value.getOwned());
    }
  }
}

/// Attempt to promote the specified stack allocation, returning true if so
/// or false if not.  On success, this returns true and usually drops all of the
/// uses of the AllocStackInst, but never deletes the ASI itself.  Callers
/// should check to see if the ASI is dead after this and remove it if so.
bool MemoryToRegisters::promoteSingleAllocation(AllocStackInst *alloc) {
  LLVM_DEBUG(llvm::dbgs() << "*** Memory to register looking at: " << *alloc);
  ++NumAllocStackFound;

  // In OSSA, don't do Mem2Reg on non-trivial alloc_stack with dynamic_lifetime.
  if (alloc->hasDynamicLifetime() && f.hasOwnership() &&
      !alloc->getType().isTrivial(f)) {
    return false;
  }

  // Don't handle captured AllocStacks.
  bool inSingleBlock = false;
  if (isCaptured(alloc, &inSingleBlock)) {
    ++NumAllocStackCaptured;
    return false;
  }

  // Remove write-only AllocStacks.
  if (isWriteOnlyAllocation(alloc) && !lexicalLifetimeEnsured(alloc)) {
    LLVM_DEBUG(llvm::dbgs() << "*** Deleting store-only AllocStack: "<< *alloc);
    deleter.forceDeleteWithUsers(alloc);
    return true;
  }

  // For AllocStacks that are only used within a single basic blocks, use
  // the linear sweep to remove the AllocStack.
  if (inSingleBlock) {
    removeSingleBlockAllocation(alloc);

    LLVM_DEBUG(llvm::dbgs() << "*** Deleting single block AllocStackInst: "
                            << *alloc);
    deleter.forceDeleteWithUsers(alloc);
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Need to insert BB arguments for " << *alloc);

  // Promote this allocation, lazily computing dom tree levels for this function
  // if we have not done so yet.
  auto &domTreeLevels = getDomTreeLevels();
  StackAllocationPromoter(alloc, domInfo, domTreeLevels, ctx, deleter,
                          instructionsToDelete)
      .run();
  return true;
}

bool MemoryToRegisters::run() {
  bool madeChange = false;

  if (f.getModule().getOptions().VerifyAll)
    f.verifyCriticalEdges();

  for (auto &block : f) {
    // Don't waste time optimizing unreachable blocks.
    if (!domInfo->isReachableFromEntry(&block)) {
      continue;
    }
    for (SILInstruction &inst : block.reverseDeletableInstructions()) {
      auto *asi = dyn_cast<AllocStackInst>(&inst);
      if (!asi)
        continue;

      if (promoteSingleAllocation(asi)) {
        for (auto *inst : instructionsToDelete) {
          deleter.forceDelete(inst);
        }
        instructionsToDelete.clear();
        ++NumInstRemoved;
        madeChange = true;
      }
    }
  }
  return madeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class SILMem2Reg : public SILFunctionTransform {
  void run() override {
    SILFunction *f = getFunction();

    LLVM_DEBUG(llvm::dbgs()
               << "** Mem2Reg on function: " << f->getName() << " **\n");

    auto *da = PM->getAnalysis<DominanceAnalysis>();

    bool madeChange = MemoryToRegisters(*f, da->get(f)).run();
    if (madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createMem2Reg() {
  return new SILMem2Reg();
}

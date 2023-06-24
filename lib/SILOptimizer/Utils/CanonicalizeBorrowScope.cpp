//===--- CanonicalizeBorrowScope.cpp - Canonicalize OSSA borrow scopes ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// TODO: Enable -canonical-ossa-rewrite-borrows by default (see
/// CopyPropagation).
///
/// TODO: Add support for load_borrows, and reborrows by using persistentCopies.
///
/// TODO: Hoist struct/tuple with multiple operands out of borrow scopes if that
/// ever happens. Consider modeling them as reborrows instead.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "copy-propagation"

#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                           MARK: Local utilities
//===----------------------------------------------------------------------===//

static bool hasValueOwnership(SILValue value) {
  return value->getOwnershipKind() == OwnershipKind::Guaranteed ||
         value->getOwnershipKind() == OwnershipKind::Owned;
}

static SingleValueInstruction *asCopyOrMove(SILValue v) {
  if (auto *copy = dyn_cast<CopyValueInst>(v))
    return copy;
  if (auto *move = dyn_cast<MoveValueInst>(v))
    return move;
  return nullptr;
}

/// Delete a chain of unused copies and moves leading to \p v.
static void deleteCopyAndMoveChain(SILValue v, InstructionDeleter &deleter) {
  while (auto *inst = asCopyOrMove(v)) {
    if (!onlyHaveDebugUses(inst))
      break;

    v = inst->getOperand(CopyLikeInstruction::Src);
    LLVM_DEBUG(llvm::dbgs() << "  Deleting " << *inst);
    ++NumCopiesAndMovesEliminated;
    deleter.forceDelete(inst);
  }
}

//===----------------------------------------------------------------------===//
//                        MARK: Rewrite borrow scopes
//===----------------------------------------------------------------------===//

/// Does this instruction forward ownership, and can it be hoisted out of a
/// borrow scope or sunk to its uses?
///
/// Currently requires the first operand to forward ownership.
///
/// Handles:
///   OwnershipForwardingConversionInst (all kinds of ref casts)
///   OwnershipForwardingMultipleValueInstruction
///     (DestructureStruct, DestructureTuple)
///   FirstArgOwnershipForwardingSingleValueInst
///     (Object, Enum, UncheckedEnumData, Open/InitExistentialRef,
///      MarkDependence)
///
/// TODO:
///   Enum, InitExistential, MarkDependence
///   Struct, Tuple
///   SelectEnum, SwitchEnum, CheckCastBranch
bool CanonicalizeBorrowScope::isRewritableOSSAForward(SILInstruction *inst) {
  if (!inst->isTriviallyDuplicatable())
    return false;

  // TODO: check other operands for dominance and even attempt to hoist them.
  if (inst->getNumOperands() != 1)
    return false;

  if (isa<OwnershipForwardingSingleValueInstruction>(inst) ||
      isa<OwnershipForwardingMultipleValueInstruction>(inst)) {
    Operand *forwardedOper = &inst->getOperandRef(0);
    // Trivial conversions do not need to be hoisted out of a borrow scope.
    auto operOwnership = forwardedOper->getOperandOwnership();
    if (operOwnership == OperandOwnership::TrivialUse)
      return false;
    // Don't mess with unowned conversions. They need to be copied immediately.
    if (operOwnership != OperandOwnership::GuaranteedForwarding &&
        operOwnership != OperandOwnership::ForwardingConsume) {
      return false;
    }
    assert(operOwnership == OperandOwnership::GuaranteedForwarding ||
           operOwnership == OperandOwnership::ForwardingConsume);

    // Filter instructions that belong to a Forwarding*ValueInst mixin but
    // cannot be converted to forward owned value (struct_extract).
    if (!canOpcodeForwardOwnedValues(forwardedOper))
      return false;

    return true;
  }
  return false;
}

/// Return the root of a borrowed extended lifetime for \p def or invalid.
///
/// \p def may be any guaranteed value.
SILValue CanonicalizeBorrowScope::getCanonicalBorrowedDef(SILValue def) {
  while (true) {
    if (def->getOwnershipKind() != OwnershipKind::Guaranteed)
      break;

    if (auto borrowedVal = BorrowedValue(def)) {
      // Any def's that aren't filtered out here must be handled by
      // computeBorrowLiveness.
      switch (borrowedVal.kind) {
      case BorrowedValueKind::Invalid:
        llvm_unreachable("Using invalid case?!");
      case BorrowedValueKind::SILFunctionArgument:
      case BorrowedValueKind::BeginBorrow:
        return def;

      case BorrowedValueKind::LoadBorrow:
      case BorrowedValueKind::Phi:
        break;
      }
    }
    // Look through hoistable guaranteed forwarding instructions on the
    // use-def chain.
    SILInstruction *defInst = def->getDefiningInstruction();
    if (!defInst)
      break;

    if (!CanonicalizeBorrowScope::isRewritableOSSAForward(defInst))
      break;

    def = defInst->getOperand(0);
  }
  return SILValue();
}

bool CanonicalizeBorrowScope::computeBorrowLiveness() {
  switch (borrowedValue.kind) {
  case BorrowedValueKind::Invalid:
    llvm_unreachable("Used invalid");
  case BorrowedValueKind::SILFunctionArgument:
    // For efficiency, function arguments skip liveness.
    return true;
  case BorrowedValueKind::LoadBorrow:
  case BorrowedValueKind::Phi:
    // TODO: Canonicalize load_borrow scope and phi once consolidateBorrowScope
    // can handle persistentCopies.
    return false;
  case BorrowedValueKind::BeginBorrow:
    break;
  }
  // Note that there is no need to look through any reborrows. The reborrowed
  // value is considered a separate lifetime for canonicalization. Any copies of
  // the reborrowed value will not be rewritten when canonicalizing the current
  // borrow scope because they are "hidden" behind the reborrow.
  borrowedValue.visitLocalScopeEndingUses([this](Operand *use) {
    liveness->updateForUse(use->getUser(), /*lifetimeEnding*/ true);
    return true;
  });
  return true;
}

/// Return the source of a use within a borrow scope. This is the use-def
/// equivalent to the logic in visitBorrowScopeUses that recurses through
/// copies. The use-def and def-use logic must be consistent.
SILValue CanonicalizeBorrowScope::findDefInBorrowScope(SILValue value) {
  while (auto *inst = asCopyOrMove(value)) {
    auto *copy = dyn_cast<CopyValueInst>(inst);
    if (copy && isPersistentCopy(copy))
      return copy;

    value = inst->getOperand(0);
  }
  return value;
}

/// Visit all extended uses within the borrow scope, looking through copies and
/// moves. Call visitUse for uses which could potentially be outside the borrow
/// scope. Call visitForwardingUse for hoistable forwarding operations which
/// could potentially be inside the borrow scope.
///
/// The visitor may or may not be able to determine which uses are outside the
/// scope, but it can filter uses that are definitely within the scope. For
/// example, guaranteed uses and uses in live-out blocks must both be within
/// the scope.
///
/// This def-use traversal is similar to findExtendedTransitiveGuaranteedUses(),
/// however, to cover the canonical lifetime, it looks through copies. It also
/// considers uses within the introduced borrow scope itself (instead of simply
/// visiting the scope-ending uses). It does not, however, look into nested
/// borrow scopes uses, since nested scopes are canonicalized independently.
///
/// \p innerValue is either the initial begin_borrow, or a forwarding operation
/// within the borrow scope.
///
/// Note: This must always return true when innerValue is a function argument.
template <typename Visitor>
bool CanonicalizeBorrowScope::visitBorrowScopeUses(SILValue innerValue,
                                                   Visitor &visitor) {
  // defUseWorklist is used recursively here.
  // This avoids revisiting uses in case we ever recurse through
  // both structs and destructures.
  unsigned defUseStart = defUseWorklist.size();
  defUseWorklist.insert(innerValue);
  while (defUseStart < defUseWorklist.size()) {
    SILValue value = defUseWorklist.pop();
    // Gather the uses before updating any of them.
    // 'value' may be deleted in this loop after rewriting its last use.
    // 'use' may become invalid after processing its user.

    SmallVector<Operand *, 4> uses(value->getUses());
    for (auto *use : uses) {
      auto *user = use->getUser();
      // Incidental uses, such as debug_value may be deleted before they can be
      // processed. Their user will now be nullptr. This means that value
      // is dead, so just bail.
      if (!user)
        break;

      // Recurse through copies and moves.
      if (auto *copy = dyn_cast<CopyValueInst>(user)) {
        if (!isPersistentCopy(copy)) {
          defUseWorklist.insert(copy);
          continue;
        }
      } else if (auto *move = dyn_cast<MoveValueInst>(user)) {
        if (!move->isLexical() || innerValue->isLexical()) {
          defUseWorklist.insert(move);
          continue;
        }
      }
      // Note: debug_value uses are handled like normal uses here. If they are
      // inner uses, they remain. If they are outer uses, then they will be
      // stripped if needed when the outer copy or persistent copy is
      // canonicalized.
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      case OperandOwnership::InteriorPointer:
      case OperandOwnership::EndBorrow:
      case OperandOwnership::Reborrow:
        // Ignore uses that must be within the borrow scope.
        // Rewriting does not look through reborrowed values--it considers them
        // part of a separate lifetime.
        break;

      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        // Pointer escapes are only allowed if they use the guaranteed value,
        // which means that the escaped value must be confined to the current
        // borrow scope. visitBorrowScopeUses must never return false when
        // borrowedValue is a SILFunctionArgument.
        if (use->get()->getOwnershipKind() != OwnershipKind::Guaranteed &&
            !isa<SILFunctionArgument>(borrowedValue.value)) {
          return false;
        }
        if (!visitor.visitUse(use)) {
          assert(!isa<SILFunctionArgument>(borrowedValue.value));
          return false;
        }
        break;

      case OperandOwnership::GuaranteedForwarding:
      case OperandOwnership::ForwardingConsume:
        if (CanonicalizeBorrowScope::isRewritableOSSAForward(user)) {
          if (!visitor.visitForwardingUse(use)) {
            assert(!isa<SILFunctionArgument>(borrowedValue.value));
            return false;
          }
          break;
        }
        LLVM_FALLTHROUGH;

      case OperandOwnership::Borrow:
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
      case OperandOwnership::DestroyingConsume:
        if (!visitor.visitUse(use)) {
          assert(!isa<SILFunctionArgument>(borrowedValue.value));
          return false;
        }
        break;
      } // end switch OperandOwnership
    }
  } // end def-use traversal
  return true;
}

namespace {

using OuterUsers = CanonicalizeBorrowScope::OuterUsers;

/// Find all potential outer uses within a borrow scope.
///
/// Implements visitBorrowScopeUses<Visitor>
class FindBorrowScopeUses {
  CanonicalizeBorrowScope &scope;

  /// useInsts are the potentially outer use instructions. This set is built up
  /// recursively. It is pared down to only the outer uses outside this class by
  /// filterOuterBorrowUseInsts.
  OuterUsers useInsts;

public:
  FindBorrowScopeUses(CanonicalizeBorrowScope &scope) : scope(scope) {}

  Optional<OuterUsers> findUses() && {
    scope.beginVisitBorrowScopeUses();
    if (!scope.visitBorrowScopeUses(scope.getBorrowedValue().value, *this))
      return None;

    return std::move(useInsts);
  }

  bool visitUse(Operand *use) {
    // A guaranteed use can never be outside this borrow scope
    if (use->get()->getOwnershipKind() == OwnershipKind::Guaranteed)
      return true;
    
    auto *user = use->getUser();
    if (!isUserInLiveOutBlock(user)) {
      useInsts.insert(user);
    }
    if (auto borrowingOper = BorrowingOperand(use)) {
      // For borrows, record the scope-ending instructions to outer use
      // points. Note: The logic in filterOuterBorrowUseInsts that checks
      // whether a borrow scope is an outer use must visit the same set of uses.
      //
      // FIXME: visitExtendedScopeEndingUses can't return false here once dead
      // borrows are disallowed.
      if (!borrowingOper.visitExtendedScopeEndingUses([&](Operand *endBorrow) {
        auto *endInst = endBorrow->getUser();
        if (!isUserInLiveOutBlock(endInst)) {
          useInsts.insert(endInst);
        }
        return true;
      })) {
        useInsts.insert(user);
      }
    }
    return true;
  }

  // Recurse through forwards.
  bool visitForwardingUse(Operand *use) {
    auto *user = use->getUser();
    if (!isUserInLiveOutBlock(user)) {
      useInsts.insert(user);
    }
    for (auto result : user->getResults()) {
      if (hasValueOwnership(result)) {
        if (!scope.visitBorrowScopeUses(result, *this))
          return false;
      }
    }
    return true;
  }

protected:
  bool isUserInLiveOutBlock(SILInstruction *user) {
    return (scope.getLiveness().getBlockLiveness(user->getParent())
            == PrunedLiveBlocks::LiveOut);
    return false;
  };
};

} // namespace

/// Erase users from \p outerUseInsts that are not within the borrow scope.
void CanonicalizeBorrowScope::filterOuterBorrowUseInsts(
    OuterUsers &outerUseInsts) {
  auto *beginBorrow = cast<BeginBorrowInst>(borrowedValue.value);
  SmallVector<SILInstruction *, 4> scopeEndingInsts;
  BorrowedValue(beginBorrow).getLocalScopeEndingInstructions(scopeEndingInsts);
  blockWorklist.clear();
  // Remove outer uses that occur before the end of the borrow scope by
  // reverse iterating from the end_borrow.
  auto scanBlock = [&](SILBasicBlock *bb, SILBasicBlock::iterator endIter) {
    auto beginIter = bb->begin();
    if (bb == beginBorrow->getParent()) {
      beginIter = std::next(beginBorrow->getIterator());
    } else {
      blockWorklist.insert(bb);
    }
    for (auto instIter = endIter; instIter != beginIter;) {
      --instIter;
      outerUseInsts.erase(&*instIter);
    }
  };
  for (auto *scopeEnd : scopeEndingInsts) {
    scanBlock(scopeEnd->getParent(), std::next(scopeEnd->getIterator()));
  }
  // This worklist is also a visited set, so we never pop the entries.
  while (auto *bb = blockWorklist.pop()) {
    for (auto *predBB : bb->getPredecessorBlocks()) {
      scanBlock(predBB, predBB->end());
    }
  }
}

namespace {

/// Remove redundant copies/destroys within a borrow scope.
///
/// The visitor callbacks must always return true since this rewrites in-place.
class RewriteInnerBorrowUses {
  CanonicalizeBorrowScope &scope;

public:
  RewriteInnerBorrowUses(CanonicalizeBorrowScope &scope): scope(scope) {}

  CanonicalizeBorrowScope &getScope() const { return scope; }

  // Implements visitBorrowScopeUses<Visitor>
  bool visitUse(Operand *use) {
    auto *user = use->getUser();
    SILValue value = use->get();
    // destroys are never needed within a borrow scope.
    if (isa<DestroyValueInst>(user)) {
      scope.getDeleter().forceDelete(user);
      deleteCopyAndMoveChain(value, scope.getDeleter());
      return true;
    }
    SILValue def = scope.findDefInBorrowScope(value);
    if (use->isConsuming()) {
      // All in-scope consuming uses need a unique copy in the same block.
      auto *copy = dyn_cast<CopyValueInst>(use->get());
      if (copy && copy->hasOneUse() && copy->getParent() == user->getParent()) {
        value = copy->getOperand();
        copy->setOperand(def);
      } else {
        use->set(def);
        copyLiveUse(use, scope.getCallbacks());
      }
      deleteCopyAndMoveChain(value, scope.getDeleter());
      return true;
    }
    // Non-consuming use.
    use->set(def);
    deleteCopyAndMoveChain(value, scope.getDeleter());
    return true;
  }

  // Recurse through forwards.
  //
  // Implements visitBorrowScopeUses<Visitor>
  bool visitForwardingUse(Operand *use) {
    auto *user = use->getUser();
    assert(CanonicalizeBorrowScope::isRewritableOSSAForward(user));

    for (auto result : user->getResults()) {
      if (!hasValueOwnership(result)) {
        continue;
      }
      scope.visitBorrowScopeUses(result, *this);
    }
    // Update this operand bypassing any copies.
    SILValue value = use->get();
    use->set(scope.findDefInBorrowScope(value));
    ForwardingOperand(use).setForwardingOwnershipKind(
        OwnershipKind::Guaranteed);
    deleteCopyAndMoveChain(value, scope.getDeleter());
    return true;
  }
};

/// Generate copies outside this borrow scope, hoist forwarding operations,
/// rewrite the operands of uses outside the scope, and record the outer uses
/// that are consumes.
///
/// Each hoisted forwarding operation instantiates a new instance of this
/// visitor. They are rewritten separately.
///
/// Implements visitBorrowScopeUses<Visitor>
///
/// The visitor callbacks must always return true since this rewrites in-place.
class RewriteOuterBorrowUses {
  CanonicalizeBorrowScope &scope;

  RewriteInnerBorrowUses &innerRewriter;

  const OuterUsers &outerUseInsts;

  /// Map values inside the borrow scope to cloned-and-copied values outside the
  /// borrow scope.
  using InnerToOuterMap = llvm::SmallDenseMap<SILValue, SILValue, 8>;
  InnerToOuterMap &innerToOuterMap;

  // Outer uses specific to the current incomingValue, including hoisted
  // forwarding instructions, which are not part of outerUseInsts.
  SmallVector<Operand *, 4> consumingUses;
  SmallPtrSet<SILInstruction *, 4> unclaimedConsumingUsers;

  RewriteOuterBorrowUses(RewriteInnerBorrowUses &innerRewriter,
                         const OuterUsers &outerUseInsts,
                         InnerToOuterMap &innerToOuterMap)
    : scope(innerRewriter.getScope()), innerRewriter(innerRewriter),
      outerUseInsts(outerUseInsts), innerToOuterMap(innerToOuterMap) {}

public:
  static void rewrite(BeginBorrowInst *beginBorrow,
                      CanonicalizeBorrowScope &scope,
                      const OuterUsers &outerUseInsts) {

    SILBuilderWithScope builder(beginBorrow);
    auto loc = RegularLocation::getAutoGeneratedLocation(beginBorrow->getLoc());
    auto *outerCopy =
        createOuterCopy(beginBorrow->getOperand(), builder, loc, scope);

    llvm::SmallDenseMap<SILValue, SILValue, 8> innerToOuterMap;
    innerToOuterMap[beginBorrow] = outerCopy;

    RewriteInnerBorrowUses innerRewriter(scope);
    RewriteOuterBorrowUses rewriter(innerRewriter, outerUseInsts,
                                    innerToOuterMap);
    scope.beginVisitBorrowScopeUses();
    auto outerVal = rewriter.recursivelyRewriteOuterUses(beginBorrow);
    assert(outerVal == outerCopy);
  }

  // Implements visitBorrowScopeUses<Visitor>
  bool visitUse(Operand *use) {
    auto *user = use->getUser();
    if (outerUseInsts.count(user)) {
      rewriteOuterUse(use);
      return true;
    }
    // If this use begins a borrow scope, check if any of the scope ending
    // instructions are outside the current scope (this can happen if any copy
    // has occurred on the def-use chain within the current scope).
    if (auto borrowingOper = BorrowingOperand(use)) {
      if (!borrowingOper.visitExtendedScopeEndingUses(
            [&](Operand *endBorrow) {
              return !outerUseInsts.count(endBorrow->getUser());
            })) {
        rewriteOuterUse(use);
        return true;
      }
    }
    innerRewriter.visitUse(use);
    return true;
  }

  // Recurse through forwards.
  //
  // Implements visitBorrowScopeUses<Visitor>
  bool visitForwardingUse(Operand *use) {
    auto *user = use->getUser();
    assert(CanonicalizeBorrowScope::isRewritableOSSAForward(user));
    if (outerUseInsts.count(user)) {
      rewriteOuterUse(use);
      return true;
    }

    // Process transitive users and add this forwarding operation to
    // outerUseInsts if any outer uses were found.
    SILInstruction *outerClone = nullptr;
    for (auto result : user->getResults()) {
      if (!hasValueOwnership(result)) {
        continue;
      }
      RewriteOuterBorrowUses rewriter(innerRewriter, outerUseInsts,
                                      innerToOuterMap);
      auto outerVal = rewriter.recursivelyRewriteOuterUses(result);
      if (outerVal)
        outerClone = outerVal->getDefiningInstruction();
    }
    // Record the forwarded operand of this hoisted instruction as an outer use.
    if (outerClone) {
      recordOuterUse(&outerClone->getOperandRef(0));
    }
    // If it's not already dead, update this operand bypassing any copies.
    SILValue innerValue = use->get();
    if (scope.getDeleter().deleteIfDead(user, /*fixLifetime=*/false)) {
      LLVM_DEBUG(llvm::dbgs() << "  Deleted " << *user);
    } else {
      use->set(scope.findDefInBorrowScope(use->get()));
      ForwardingOperand(use).setForwardingOwnershipKind(
          OwnershipKind::Guaranteed);
    }
    deleteCopyAndMoveChain(innerValue, scope.getDeleter());
    return true;
  }

protected:
  // Create a copy for outer uses of the borrow scope introduced by
  // currentDef. This copy should only be used by outer uses in the same block
  // as the borrow scope.
  //
  // To use an existing outer copy, we could find its earliest consume. But the
  // new copy will immediately canonicalized and a canonical begin_borrow scope
  // have no outer uses of its first block.
  static CopyValueInst *createOuterCopy(SILValue incomingValue,
                                        SILBuilder &builder, SILLocation loc,
                                        CanonicalizeBorrowScope &scope) {
    auto *copy = builder.createCopyValue(loc, incomingValue);
    scope.getCallbacks().createdNewInst(copy);
    scope.recordOuterCopy(copy);

    ++NumCopiesGenerated;
    LLVM_DEBUG(llvm::dbgs() << "  Outer copy " << *copy);

    return copy;
  }

  SILValue recursivelyRewriteOuterUses(SILValue innerValue) {
    bool succeed = scope.visitBorrowScopeUses(innerValue, *this);
    assert(succeed && "should be filtered by FindBorrowScopeUses");

    auto iter = innerToOuterMap.find(innerValue);
    if (iter == innerToOuterMap.end()) {
      return SILValue();
    }
    SILValue outerValue = iter->second;
    cleanupOuterValue(outerValue);
    return outerValue;
  }

  void recordOuterUse(Operand *use) {
    if (use->isLifetimeEnding()) {
      consumingUses.push_back(use);
      unclaimedConsumingUsers.insert(use->getUser());
    }
  };

  void rewriteOuterUse(Operand *use) {
    LLVM_DEBUG(llvm::dbgs() << "  Use of outer copy " << *use->getUser());

    SILValue innerValue = use->get();
    SILValue outerValue = createOuterValues(innerValue);

    use->set(outerValue);

    deleteCopyAndMoveChain(innerValue, scope.getDeleter());

    recordOuterUse(use);
  };

  SILValue createOuterValues(SILValue innerValue);

  void cleanupOuterValue(SILValue outerValue);
};

} // namespace

// Incrementally create a def-use chain outside the current borrow scope for
// all instructions between currentDef and \p innerValue, where \p innerValue
// has at least one use outside the borrow scope.
SILValue RewriteOuterBorrowUses::createOuterValues(SILValue innerValue) {
  // Poke through the copies just like rewriteOuterBorrowUsesAndFindConsumes
  // does on the def-use side.
  while (auto *copy = dyn_cast<CopyValueInst>(innerValue)) {
    innerValue = copy->getOperand();
  }
  auto iter = innerToOuterMap.find(innerValue);
  if (iter != innerToOuterMap.end()) {
    return iter->second;
  }
  auto *innerInst = innerValue->getDefiningInstruction();
  assert(CanonicalizeBorrowScope::isRewritableOSSAForward(innerInst));
  SILValue incomingInnerVal = innerInst->getOperand(0);

  auto incomingOuterVal = createOuterValues(incomingInnerVal);

  auto *insertPt = innerValue->getDefiningInsertionPoint();
  auto *clone = innerInst->clone(insertPt);
  scope.getCallbacks().createdNewInst(clone);
  Operand *use = &clone->getOperandRef(0);
  use->set(incomingOuterVal);
  ForwardingOperand(use).setForwardingOwnershipKind(OwnershipKind::Owned);

  LLVM_DEBUG(llvm::dbgs() << "  Hoisted forward " << *clone);

  for (unsigned idx = 0, endIdx = clone->getNumResults(); idx < endIdx; ++idx) {
    innerToOuterMap[innerInst->getResult(idx)] = clone->getResult(idx);
  }
  return innerToOuterMap[innerValue];
}

// Insert destroys on the outer copy's or forwarding consume's lifetime
// frontier, or claim a existing consumes. Insert copies for unclaimed consumes.
void RewriteOuterBorrowUses::cleanupOuterValue(SILValue outerValue) {
  // Gather outerValue's transitive ownership uses for ValueLifetimeAnalysis.
  SmallVector<SILInstruction *, 4> outerUses;
  for (Operand *use : outerValue->getUses()) {
    outerUses.push_back(use->getUser());
    if (auto borrowingOper = BorrowingOperand(use)) {
      borrowingOper.visitExtendedScopeEndingUses([&](Operand *endBorrow) {
        outerUses.push_back(endBorrow->getUser());
        return true;
      });
    }
  }
  ValueLifetimeAnalysis lifetimeAnalysis(outerValue.getDefiningInstruction(),
                                         outerUses);

  auto createDestroy = [&](SILBuilder &b, SILLocation loc) {
      auto *dvi = b.createDestroyValue(loc, outerValue);
      scope.getCallbacks().createdNewInst(dvi);
  };
  if (outerUses.empty()) {
    SILBuilder b(outerValue->getNextInstruction());
    createDestroy(b, outerValue.getLoc());
    return;
  }

  ValueLifetimeBoundary boundary;
  lifetimeAnalysis.computeLifetimeBoundary(boundary);

  for (auto *boundaryEdge : boundary.boundaryEdges) {
    if (DeadEndBlocks::triviallyEndsInUnreachable(boundaryEdge))
      continue;
    auto insertPt = boundaryEdge->begin();
    auto *dvi = SILBuilderWithScope(insertPt).createDestroyValue(
        insertPt->getLoc(), outerValue);
    scope.getCallbacks().createdNewInst(dvi);
  }

  for (SILInstruction *lastUser : boundary.lastUsers) {
    if (unclaimedConsumingUsers.erase(lastUser))
      continue;

    SILBuilderWithScope::insertAfter(lastUser, [&](SILBuilder &b) {
      createDestroy(b, lastUser->getLoc());
    });
  }
  // Add copies for consuming users of outerValue.
  for (auto *use : consumingUses) {
    // If the user is still in the unclaimedConsumingUsers set, then it does not
    // end the outer copy's lifetime and therefore requires a copy. Only one
    // operand can be claimed as ending the lifetime, so return its user to the
    // unclaimedConsumingUsers set after skipping the first copy.
    auto iterAndInserted = unclaimedConsumingUsers.insert(use->getUser());
    if (!iterAndInserted.second) {
      copyLiveUse(use, scope.getCallbacks());
      scope.recordOuterCopy(cast<CopyValueInst>(use->get()));
    }
  }
}

// If this succeeds, then all uses of the borrowed value outside the borrow
// scope will be rewritten to use an outer copy, and all remaining uses of the
// borrowed value will be confined to the borrow scope.
//
// TODO: Canonicalize multi-block borrow scopes, load_borrow scope, and phi
// borrow scopes by adding one copy per block to persistentCopies for
// each block that dominates an outer use.
bool CanonicalizeBorrowScope::consolidateBorrowScope() {
  OuterUsers outerUseInsts;
  if (!isa<SILFunctionArgument>(borrowedValue.value)) {
    // getCanonicalCopiedDef ensures that if currentDef is a guaranteed value,
    // then it is a borrow scope introducer.
    assert(borrowedValue.isLocalScope());

    // Gather all potential outer uses before rewriting any to avoid scanning
    // any basic block more than once.
    Optional<OuterUsers> outerUsers = FindBorrowScopeUses(*this).findUses();
    if (!outerUsers)
      return false;

    outerUseInsts = std::move(outerUsers).value();

    filterOuterBorrowUseInsts(outerUseInsts);
  }
  // If there are no outer uses, then canonicalization is a simple matter of
  // removing all the destroys and rewriting the copies.
  if (outerUseInsts.empty()) {
    RewriteInnerBorrowUses innerRewriter(*this);
    beginVisitBorrowScopeUses(); // reset the def/use worklist
    bool succeed = visitBorrowScopeUses(borrowedValue.value, innerRewriter);
    assert(succeed && "should be filtered by FindBorrowScopeUses");
    return true;
  }
  LLVM_DEBUG(llvm::dbgs() << "  Outer uses:\n";
             for (SILInstruction *inst
                  : outerUseInsts) { llvm::dbgs() << "    " << *inst; });

  RewriteOuterBorrowUses::rewrite(cast<BeginBorrowInst>(borrowedValue.value),
                                  *this, outerUseInsts);
  return true;
}

//===----------------------------------------------------------------------===//
//                  MARK: Top-Level CanonicalizeBorrowScope
//===----------------------------------------------------------------------===//

bool CanonicalizeBorrowScope::canonicalizeFunctionArgument(
    SILFunctionArgument *arg) {
  BorrowedValue borrow(arg);
  if (!borrow)
    return false;

  initBorrow(borrow);

  LLVM_DEBUG(llvm::dbgs() << "*** Canonicalize Borrow: " << borrowedValue);

  RewriteInnerBorrowUses innerRewriter(*this);
  beginVisitBorrowScopeUses(); // reset the def/use worklist

  bool succeed = visitBorrowScopeUses(borrowedValue.value, innerRewriter);
  assert(succeed && "must always succeed for function arguments");
  return true;
}

/// Canonicalize a worklist of extended lifetimes. This iterates after rewriting
/// borrow scopes to handle new outer copies and new owned lifetimes from
/// forwarding operations.
bool CanonicalizeBorrowScope::
canonicalizeBorrowScope(BorrowedValue borrowedValue) {
  BitfieldRef<SSAPrunedLiveness>::StackState livenessBitfieldContainer(
      liveness, function);

  LLVM_DEBUG(llvm::dbgs() << "*** Canonicalize Borrow: " << borrowedValue);

  initBorrow(borrowedValue);

  if (!computeBorrowLiveness())
    return false;

  // Set outerCopy and persistentCopies and rewrite uses
  // outside the scope.
  if (!consolidateBorrowScope())
    return false;

  return true;
}

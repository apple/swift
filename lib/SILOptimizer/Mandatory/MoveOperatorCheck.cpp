//===--- MoveOperatorCheck.cpp --------------------------------------------===//
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

#define DEBUG_TYPE "sil-move-operator-check"

#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

static void
findUsesReachableFromMove(MoveValueInst *mvi,
                          SmallVectorImpl<Operand *> &targetUses,
                          SmallVectorImpl<Operand *> &reachableUses) {
  LLVM_DEBUG(for (auto *use : targetUses) {
    llvm::dbgs() << "Target Use: " << *use->getUser();
  });

  // First go through all of our uses and see if any of our uses are in the same
  // block as mvi. If they are, add the relevant user to the inst set and then
  // walk a single time from mvi to the end of block and see if we hit any of
  // these instructions. If the use is not in the same block as mvi, add its
  // block to the target blocks set.
  SmallFrozenMultiMap<SILBasicBlock *, Operand *, 8> blockToOperandsMap;
  SmallFrozenMultiMap<SILInstruction *, Operand *, 8> sameBlockUsers;
  SILBasicBlock *mviBlock = mvi->getParent();
  for (auto *use : targetUses) {
    auto *useParentBlock = use->getUser()->getParent();
    if (useParentBlock == mviBlock) {
      sameBlockUsers.insert(use->getUser(), use);
      continue;
    }
    blockToOperandsMap.insert(useParentBlock, use);
  }
  sameBlockUsers.setFrozen();
  blockToOperandsMap.setFrozen();

  if (sameBlockUsers.size()) {
    for (auto ii = std::next(mvi->getIterator()), ie = mviBlock->end();
         ii != ie; ++ii) {
      if (auto range = sameBlockUsers.find(&*ii)) {
        for (auto *use : *range) {
          reachableUses.push_back(use);
        }
      }
    }
  }

  BasicBlockWorklist worklist(mviBlock);
  worklist.pop();
  for (auto *succ : mviBlock->getSuccessorBlocks()) {
    worklist.pushIfNotVisited(succ);
  }

  while (auto *nextBlock = worklist.pop()) {
    if (auto foundUses = blockToOperandsMap.find(nextBlock)) {
      for (auto *use : *foundUses) {
        reachableUses.push_back(use);
      }
    }

    for (auto *succ : nextBlock->getSuccessorBlocks()) {
      worklist.pushIfNotVisited(succ);
    }
  }
}

/// Walk from use->def from mvi's operand looking for all owned value
/// introducers that introducer operand (or any part of operand if operand is an
/// aggregate). Then gather up all initial uses of those values and place them
/// into foundIntroducerDirectUses.
static void gatherIntroducers(
    MoveValueInst *mvi,
    SmallVectorImpl<OwnedValueIntroducer> &resultingFoundIntroducers) {
  bool foundIntroducers =
      getAllOwnedValueIntroducers(mvi->getOperand(), resultingFoundIntroducers);
  (void)foundIntroducers;
  assert(foundIntroducers && "Failed to find all introducers of move?!");
  SmallVector<Operand *, 32> introducerUses;
  for (unsigned i = 0; i != resultingFoundIntroducers.size(); ++i) {
    auto introducer = resultingFoundIntroducers[i];
    switch (introducer.kind) {
    case OwnedValueIntroducerKind::Invalid:
      llvm_unreachable("Should never hit this");
    case OwnedValueIntroducerKind::Copy: {
      auto *cvi = cast<CopyValueInst>(introducer.value);
      // If we are copying a guaranteed value, just continue. We do not support
      // this.
      //
      // TODO: Ban in the frontend passing an argument to the move operator.
      if (cvi->getOperand().getOwnershipKind() == OwnershipKind::Guaranteed) {
        continue;
      }
      bool foundIntroducers = getAllOwnedValueIntroducers(
          cvi->getOperand(), resultingFoundIntroducers);
      assert(foundIntroducers);
      continue;
    }
    case OwnedValueIntroducerKind::AllocBoxInit:
    case OwnedValueIntroducerKind::AllocRefInit:
    case OwnedValueIntroducerKind::Apply:
    case OwnedValueIntroducerKind::BeginApply:
    case OwnedValueIntroducerKind::TryApply:
    case OwnedValueIntroducerKind::FunctionArgument:
    case OwnedValueIntroducerKind::PartialApplyInit: {
      continue;
    }
    case OwnedValueIntroducerKind::Struct: {
      auto *si = dyn_cast<StructInst>(introducer.value);
      for (SILValue op : si->getOperandValues()) {
        bool foundIntroducers =
            getAllOwnedValueIntroducers(op, resultingFoundIntroducers);
        assert(foundIntroducers);
      }
      continue;
    }
    case OwnedValueIntroducerKind::Tuple: {
      auto *ti = dyn_cast<TupleInst>(introducer.value);
      for (SILValue op : ti->getOperandValues()) {
        bool foundIntroducers =
            getAllOwnedValueIntroducers(op, resultingFoundIntroducers);
        assert(foundIntroducers);
      }
      continue;
    }
    case OwnedValueIntroducerKind::Phi:
    case OwnedValueIntroducerKind::LoadCopy:
    case OwnedValueIntroducerKind::LoadTake:
      llvm_unreachable("Unimplemented?!");
    }
  }
}

static void performMoveVerification(MoveValueInst *mvi) {
  LLVM_DEBUG(llvm::dbgs() << "Visiting Move Inst: " << *mvi);
  auto &astCtx = mvi->getFunction()->getASTContext();
  auto &de = astCtx.Diags;

  SmallVector<OwnedValueIntroducer, 32> foundIntroducers;

  // First gather up all direct uses of any owned value introducer that we are
  // moving.
  gatherIntroducers(mvi, foundIntroducers);

  // Then for each introducer, make sure that they do not have any uses past our
  // move_value inst.
  SmallVector<Operand *, 32> introducerUses;
  while (!foundIntroducers.empty()) {
    auto introducer = foundIntroducers.pop_back_val();

    // Skip invalid introducers.
    //
    // Invalid introducers are copies or aggregates that we do not want to emit
    // diagnostics for. Instead we want to emit diagnostics for their operands.
    if (!introducer)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "Introducer: " << introducer.value);

    SWIFT_DEFER { introducerUses.clear(); };

    // Gather all uses that aren't our move_value instruction.
    llvm::copy_if(introducer.value->getUses(),
                  std::back_inserter(introducerUses),
                  [&](Operand *use) { return use->getUser() != mvi; });

    for (unsigned i = 0; i != introducerUses.size(); ++i) {
      auto *use = introducerUses[i];
      // Look through forwarding uses.
      if (auto forwardingOperand = ForwardingOperand(use)) {
        forwardingOperand.visitForwardedValues([&](SILValue value) -> bool {
          llvm::copy_if(value->getUses(), std::back_inserter(introducerUses),
                        [&](Operand *use) { return use->getUser() != mvi; });
          return true;
        });
      }

      // Look through copies.
      if (auto *cvi = dyn_cast<CopyValueInst>(use->getUser())) {
        llvm::copy_if(cvi->getUses(), std::back_inserter(introducerUses),
                      [&](Operand *use) { return use->getUser() != mvi; });
      }

      // Look through borrows.
      if (auto *bbi = dyn_cast<BeginBorrowInst>(use->getUser())) {
        llvm::copy_if(bbi->getUses(), std::back_inserter(introducerUses),
                      [&](Operand *use) { return use->getUser() != mvi; });
      }
    }

    // Ok, we now have all uses of our introducer that are not our
    // move_value. Prove that they are not reachable from our mvi.
    SmallVector<Operand *, 1> reachableUses;
    findUsesReachableFromMove(mvi, introducerUses, reachableUses);
    if (reachableUses.empty())
      continue;

    Optional<StringRef> name;
    if (auto *svi = dyn_cast<SingleValueInstruction>(introducer.value)) {
      if (auto debugVarCarryingInst = DebugVarCarryingInst(svi)) {
        if (auto varInfo = debugVarCarryingInst.getVarInfo()) {
          name = varInfo->Name;
        }
      }
    } else if (auto *arg = dyn_cast<SILFunctionArgument>(introducer.value)) {
      name = arg->getDecl()->getBaseName().userFacingName();
    }

    if (!name) {
      for (auto *use : getDebugUses(introducer.value)) {
        if (auto debugVarCarryingInst = DebugVarCarryingInst(use->getUser())) {
          if (auto varInfo = debugVarCarryingInst.getVarInfo()) {
            name = varInfo->Name;
          }
        }
      }
    }

    if (!name)
      name = "<unknown>";

    de.diagnoseWithNotes(
        de.diagnose(mvi->getLoc().getSourceLoc(),
                    diag::moveoperator_variablemoved_useaftermove, *name),
        [&]() {
          if (auto *arg = dyn_cast<SILArgument>(introducer.value)) {
            de.diagnose(arg->getDecl(),
                        diag::moveoperator_variable_defined_here);
          } else if (auto *inst = introducer.value->getDefiningInstruction()) {
            de.diagnose(inst->getLoc().getSourceLoc(),
                        diag::moveoperator_variable_defined_here);
          }

          for (auto *use : reachableUses) {
            de.diagnose(use->getUser()->getLoc().getSourceLoc(),
                        diag::moveoperator_useaftermove_here);
          }
        });
  }
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MoveOperatorCheck : public SILFunctionTransform {
  void run() override {
    bool madeChange = false;

    for (auto &block : *getFunction()) {
      for (auto &inst : block) {
        if (auto *mvi = dyn_cast<MoveValueInst>(&inst)) {
          if (mvi->requiresVerification()) {
            performMoveVerification(mvi);
            mvi->setRequiresVerification(false);
            madeChange = true;
          }
        }
      }
    }

    if (madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // namespace

SILTransform *swift::createMoveOperatorCheck() {
  return new MoveOperatorCheck();
}

//===--- LinearLifetimeChecker.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This file defines a linear lifetime checker for SIL. A value with a linear
/// lifetime is defined as a value that is guaranteed to be consuming exactly
/// once along any path through the program and has a guarantee that all
/// non-consuming uses and the initial value are joint-postdominated by the set
/// of consuming uses.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-linear-lifetime-checker"
#include "LinearLifetimeCheckerPrivate.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILUndef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                Declarations
//===----------------------------------------------------------------------===//

namespace {

struct State {
  /// If we are checking for a specific value, this is that value. This is only
  /// used for diagnostic purposes. The algorithm if this is set works on the
  /// parent block of the value.
  llvm::Optional<SILValue> value;

  /// The insertion point where the live range begins. If the field value is not
  /// None, then:
  //
  // 1. If this value is defined by an instruction, it will be
  //    value->getDefiningInstruction().
  //
  // 2. If this is an argument, this is the first instruction in the argument's
  //    defining block.
  SILInstruction *beginInst;

  /// A builder object that we use to build a LinearLifetimeChecker::Error
  /// object that describes exhaustively the set of errors that we encountered.
  ///
  /// It also handles any asserts/messages that need to be emitted if we are
  /// supposed to fail hard.
  LinearLifetimeChecker::ErrorBuilder &errorBuilder;

  /// The blocks that we have already visited.
  BasicBlockSet visitedBlocks;

  /// If non-null a callback that we should pass any detected leaking blocks for
  /// our caller. The intention is that this can be used in a failing case to
  /// put in missing destroys.
  llvm::Optional<function_ref<void(SILBasicBlock *)>> leakingBlockCallback;

  /// If non-null a callback that we should pass all uses that we detect are not
  /// within the linear lifetime we are checking.
  llvm::Optional<function_ref<void(Operand *)>>
      nonConsumingUseOutsideLifetimeCallback;

  /// The list of passed in consuming uses.
  ArrayRef<Operand *> consumingUses;

  /// The list of passed in non consuming uses.
  ArrayRef<Operand *> nonConsumingUses;

  /// The set of blocks with consuming uses.
  BasicBlockSet blocksWithConsumingUses;

  /// The set of blocks with non-consuming uses and the associated
  /// non-consuming use SILInstruction.
  ///
  /// NOTE: This is initialized in initializeAllNonConsumingUses after which it
  /// is frozen. Before this is frozen, one can only add new (Key, Value) pairs
  /// to the map. Once frozen, map operations and blot-erase operations can be
  /// performed. Additionally it provides a getRange() operation that can be
  /// used to iterate over all (Key, [Value]) pairs ignoring erased keys.
  SmallFrozenMultiMap<SILBasicBlock *, Operand *, 8> blocksWithNonConsumingUses;

  /// The worklist that we use when performing our block dataflow.
  SmallVector<SILBasicBlock *, 32> worklist;

  /// A list of successor blocks that we must visit by the time the algorithm
  /// terminates.
  SmallSetVector<SILBasicBlock *, 8> successorBlocksThatMustBeVisited;

  State(
      SILValue value, LinearLifetimeChecker::ErrorBuilder &errorBuilder,
      llvm::Optional<function_ref<void(SILBasicBlock *)>> leakingBlockCallback,
      llvm::Optional<function_ref<void(Operand *)>>
          nonConsumingUseOutsideLifetimeCallback,
      ArrayRef<Operand *> consumingUses, ArrayRef<Operand *> nonConsumingUses)
      : value(value), beginInst(value->getDefiningInsertionPoint()),
        errorBuilder(errorBuilder), visitedBlocks(value->getFunction()),
        leakingBlockCallback(leakingBlockCallback),
        nonConsumingUseOutsideLifetimeCallback(
            nonConsumingUseOutsideLifetimeCallback),
        consumingUses(consumingUses), nonConsumingUses(nonConsumingUses),
        blocksWithConsumingUses(value->getFunction()) {}

  State(
      SILBasicBlock *beginBlock,
      LinearLifetimeChecker::ErrorBuilder &errorBuilder,
      llvm::Optional<function_ref<void(SILBasicBlock *)>> leakingBlockCallback,
      llvm::Optional<function_ref<void(Operand *)>>
          nonConsumingUseOutsideLifetimeCallback,
      ArrayRef<Operand *> consumingUses, ArrayRef<Operand *> nonConsumingUses)
      : value(), beginInst(&*beginBlock->begin()), errorBuilder(errorBuilder),
        visitedBlocks(beginBlock->getParent()),
        leakingBlockCallback(leakingBlockCallback),
        nonConsumingUseOutsideLifetimeCallback(
            nonConsumingUseOutsideLifetimeCallback),
        consumingUses(consumingUses), nonConsumingUses(nonConsumingUses),
        blocksWithConsumingUses(beginBlock->getParent()) {}

  SILBasicBlock *getBeginBlock() const { return beginInst->getParent(); }

  void initializeAllNonConsumingUses(ArrayRef<Operand *> nonConsumingUsers);
  void initializeAllConsumingUses(
      ArrayRef<Operand *> consumingUsers,
      SmallVectorImpl<std::pair<Operand *, SILBasicBlock *>>
          &predsToAddToWorklist);

  /// Initializes state for a consuming use.
  ///
  /// If we are already tracking a consuming use for the block, this emits a
  /// double consume checker error.
  void initializeConsumingUse(Operand *consumingUse, SILBasicBlock *userBlock);

  /// Check that this newly initialized consuming user does not have any
  /// non-consuming uses after it. If the checker finds one, it emits a checker
  /// error.
  void checkForSameBlockUseAfterFree(Operand *consumingUse,
                                     SILBasicBlock *userBlock);

  /// Once we have marked all of our producing blocks.
  void checkPredsForDoubleConsume(Operand *consumingUse,
                                  SILBasicBlock *userBlock);
  void checkPredsForDoubleConsume(SILBasicBlock *userBlock);

  /// Once we have setup all of our consuming/non-consuming blocks and have
  /// validated that all intra-block dataflow is safe, perform the inter-block
  /// dataflow.
  void performDataflow(DeadEndBlocks *deBlocks);

  /// After we have performed the dataflow, check the end state of our dataflow
  /// for validity. If this is a linear typed value, return true. Return false
  /// otherwise.
  void checkDataflowEndState(DeadEndBlocks *deBlocks);

  void dumpConsumingUsers() const {
    llvm::errs() << "Consuming Users:\n";
    for (auto *use : consumingUses) {
      llvm::errs() << *use->getUser();
    }
    llvm::errs() << "\n";
  }

  void dumpNonConsumingUsers() const {
    llvm::errs() << "Non Consuming Users:\n";
    for (auto *use : nonConsumingUses) {
      llvm::errs() << *use->getUser();
    }
    llvm::errs() << "\n";
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                      Non Consuming Use Initialization
//===----------------------------------------------------------------------===//

void State::initializeAllNonConsumingUses(
    ArrayRef<Operand *> nonConsumingUsers) {
  SWIFT_DEFER {
    // Once we have finished initializing blocksWithNonConsumingUses, we need to
    // freeze it. By using a defer here, we can make sure we don't forget to do
    // this below.
    blocksWithNonConsumingUses.setFrozen();
  };

  for (Operand *use : nonConsumingUsers) {
    auto *userBlock = use->getUser()->getParent();

    // Make sure that this non consuming user is in either not in our definition
    // block or is strictly after our defining instruction. If so, stash the use
    // and continue.
    if (userBlock != getBeginBlock() ||
        std::find_if(beginInst->getIterator(), userBlock->end(),
                     [&use](const SILInstruction &inst) -> bool {
                       return use->getUser() == &inst;
                     }) != userBlock->end()) {
      blocksWithNonConsumingUses.insert(userBlock, use);
      continue;
    }

    if (nonConsumingUseOutsideLifetimeCallback) {
      (*nonConsumingUseOutsideLifetimeCallback)(use);
    }

    // Otherwise, we emit an error since we found a use before our def. We do
    // not bail early here since we want to gather up /all/ that we find.
    errorBuilder.handleUseOutsideOfLifetime([&] {
      llvm::errs() << "Found use outside of lifetime?!\n"
                   << "Value: ";
      if (auto v = value) {
        llvm::errs() << *v;
      } else {
        llvm::errs() << "N/A. \n";
      }
      llvm::errs() << "User: " << use->getUser();
    });
  }
}

//===----------------------------------------------------------------------===//
//                        Consuming Use Initialization
//===----------------------------------------------------------------------===//

void State::initializeAllConsumingUses(
    ArrayRef<Operand *> consumingUses,
    SmallVectorImpl<std::pair<Operand *, SILBasicBlock *>>
        &predsToAddToWorklist) {
  for (Operand *use : consumingUses) {
    SILBasicBlock *userBlock = use->getUser()->getParent();

    // First initialize our state for the consuming user.
    //
    // If we find another consuming instruction associated with userBlock this
    // will emit a checker error.
    initializeConsumingUse(use, userBlock);

    // Then check if the given block has a use after free and emit an error if
    // we find one.
    checkForSameBlockUseAfterFree(use, userBlock);

    // If this user is in the same block as the value, do not visit
    // predecessors. We must be extra tolerant here since we allow for
    // unreachable code.
    if (userBlock == getBeginBlock())
      continue;

    // Then for each predecessor of this block...
    for (auto *pred : userBlock->getPredecessorBlocks()) {
      // If this block is not a block that we have already put on the list, add
      // it to the worklist.
      predsToAddToWorklist.push_back({use, pred});
    }
  }
}

void State::initializeConsumingUse(Operand *consumingUse,
                                   SILBasicBlock *userBlock) {
  // Map this user to the block. If we already have a value for the block, then
  // we have a double consume and need to fail.
  if (blocksWithConsumingUses.insert(userBlock))
    return;

  errorBuilder.handleOverConsume([&] {
    llvm::errs() << "Found over consume?!\n";
    if (auto v = value) {
      llvm::errs() << "Value: " << *v;
    } else {
      llvm::errs() << "Value: N/A\n";
    }
    llvm::errs() << "User: " << *consumingUse->getUser() << "Block: bb"
                 << userBlock->getDebugID() << "\n";
    dumpConsumingUsers();
  });
}

void State::checkForSameBlockUseAfterFree(Operand *consumingUse,
                                          SILBasicBlock *userBlock) {
  // If we do not have any consuming uses in the same block as our
  // consuming user, then we can not have a same block use-after-free.
  auto iter = blocksWithNonConsumingUses.find(userBlock);
  if (!iter.has_value()) {
    return;
  }

  auto nonConsumingUsesInBlock = *iter;

  // Make sure that all of our non-consuming uses are before the consuming
  // use. Otherwise, we have a use after free. Since we do not allow for cond_br
  // anymore, we know that both of our users are non-cond branch users and thus
  // must be instructions in the given block. Make sure that the non consuming
  // user is strictly before the consuming user.
  for (auto *nonConsumingUse : nonConsumingUsesInBlock) {
    if (nonConsumingUse->getUser() != consumingUse->getUser()) {
      if (std::find_if(consumingUse->getUser()->getIterator(), userBlock->end(),
                       [&nonConsumingUse](const SILInstruction &i) -> bool {
                         return nonConsumingUse->getUser() == &i;
                       }) == userBlock->end()) {
        continue;
      }
    } else if (nonConsumingUse->getOperandOwnership() ==
                   OperandOwnership::Reborrow ||
               nonConsumingUse->getOperandOwnership() ==
                   OperandOwnership::GuaranteedForwarding) {
      continue;
    }

    if (nonConsumingUseOutsideLifetimeCallback) {
      (*nonConsumingUseOutsideLifetimeCallback)(nonConsumingUse);
    }

    // NOTE: We do not exit here since we want to catch /all/ errors that we can
    // find.
    errorBuilder.handleUseOutsideOfLifetime([&] {
      llvm::errs() << "Found outside of lifetime use?!\n"
                   << "Value: ";
      if (auto v = value) {
        llvm::errs() << *v;
      } else {
        llvm::errs() << "N/A. \n";
      }
      llvm::errs() << "Consuming User: " << *consumingUse->getUser()
                   << "Non Consuming User: " << *nonConsumingUse->getUser()
                   << "Block: bb" << userBlock->getDebugID() << "\n\n";
    });
  }

  // Erase the use since we know that it is either properly joint post-dominated
  // or it was not and we emitted use after free errors.
  blocksWithNonConsumingUses.erase(userBlock);
}

void State::checkPredsForDoubleConsume(Operand *consumingUse,
                                       SILBasicBlock *userBlock) {
  if (!blocksWithConsumingUses.contains(userBlock))
    return;

  // Check if this is a block that we have already visited. This means that we
  // had a back edge of some sort. Double check that we haven't missed any
  // successors.
  if (visitedBlocks.contains(userBlock)) {
    for (auto *succ : userBlock->getSuccessorBlocks()) {
      if (!visitedBlocks.contains(succ)) {
        successorBlocksThatMustBeVisited.insert(succ);
      }
    }
  }

  errorBuilder.handleOverConsume([&] {
    llvm::errs() << "Found over consume?!\n"
                 << "Value: ";
    if (auto v = value) {
      llvm::errs() << *v;
    } else {
      llvm::errs() << "N/A. \n";
    }

    llvm::errs() << "User: " << *consumingUse->getUser() << "Block: bb"
                 << userBlock->getDebugID() << "\n";
    dumpConsumingUsers();
  });
}

void State::checkPredsForDoubleConsume(SILBasicBlock *userBlock) {
  if (!blocksWithConsumingUses.contains(userBlock))
    return;

  // Check if this is a block that we have already visited. This means that we
  // had a back edge of some sort. Double check that we haven't missed any
  // successors.
  if (visitedBlocks.contains(userBlock)) {
    for (auto *succ : userBlock->getSuccessorBlocks()) {
      if (!visitedBlocks.contains(succ)) {
        successorBlocksThatMustBeVisited.insert(succ);
      }
    }
  }

  errorBuilder.handleOverConsume([&] {
    llvm::errs() << "Found over consume?!\n"
                 << "Value: ";
    if (auto v = value) {
      llvm::errs() << *v;
    } else {
      llvm::errs() << "N/A. \n";
    }

    llvm::errs() << "Block: bb" << userBlock->getDebugID() << "\n";
    dumpConsumingUsers();
  });
}

//===----------------------------------------------------------------------===//
//                                  Dataflow
//===----------------------------------------------------------------------===//

void State::performDataflow(DeadEndBlocks *deBlocks) {
  LLVM_DEBUG(llvm::dbgs() << "    Beginning to check dataflow constraints\n");
  // Until the worklist is empty...
  while (!worklist.empty()) {
    // Grab the next block to visit.
    SILBasicBlock *block = worklist.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "    Visiting Block: bb" << block->getDebugID()
                            << '\n');

    // Since the block is on our worklist, we know already that it is not a
    // block with lifetime ending uses, due to the invariants of our loop.

    // First remove BB from the SuccessorBlocksThatMustBeVisited list. This
    // ensures that when the algorithm terminates, we know that BB was not the
    // beginning of a non-covered path to the exit.
    successorBlocksThatMustBeVisited.remove(block);

    // Then remove BB from BlocksWithNonLifetimeEndingUses so we know that
    // this block was properly joint post-dominated by our lifetime ending
    // users.
    blocksWithNonConsumingUses.erase(block);

    // Ok, now we know that we do not have an overconsume. If this block does
    // not end in a no return function, we need to update our state for our
    // successors to make sure by the end of the traversal we visit them.
    //
    // We must consider such no-return blocks since we may be running during
    // SILGen before NoReturn folding has run.
    for (auto *succBlock : block->getSuccessorBlocks()) {
      // If we already visited the successor, there is nothing to do since we
      // already visited the successor.
      if (visitedBlocks.contains(succBlock))
        continue;

      // Then check if the successor is a transitively unreachable block. In
      // such a case, we ignore it since we are going to leak along that path.
      if (deBlocks && deBlocks->isDeadEnd(succBlock))
        continue;

      // Otherwise, add the successor to our SuccessorBlocksThatMustBeVisited
      // set to ensure that we assert if we do not visit it by the end of the
      // algorithm.
      successorBlocksThatMustBeVisited.insert(succBlock);
    }

    // If we are at the dominating block of our walk, continue. There is nothing
    // further to do since we do not want to visit the predecessors of our
    // dominating block. On the other hand, we do want to add its successors to
    // the successorBlocksThatMustBeVisited set.
    if (block == getBeginBlock())
      continue;

    // Then for each predecessor of this block:
    //
    // 1. If we have visited the predecessor already, then it is not a block
    // with lifetime ending uses. If it is a block with uses, then we have a
    // double release... so assert. If not, we continue.
    //
    // 2. We add the predecessor to the worklist if we have not visited it yet.
    for (auto *predBlock : block->getPredecessorBlocks()) {
      // Check if we have an over consume.
      checkPredsForDoubleConsume(predBlock);

      if (visitedBlocks.contains(predBlock)) {
        continue;
      }

      visitedBlocks.insert(predBlock);
      worklist.push_back(predBlock);
    }
  }
}

void State::checkDataflowEndState(DeadEndBlocks *deBlocks) {
  if (!successorBlocksThatMustBeVisited.empty()) {
    // If we are asked to store any leaking blocks, put them in the leaking
    // blocks array.
    if (leakingBlockCallback) {
      for (auto *block : successorBlocksThatMustBeVisited) {
        (*leakingBlockCallback)(block);
      }
    }

    // If we are supposed to error on leaks, do so now.
    errorBuilder.handleLeak([&] {
      llvm::errs() << "Error! Found a leak due to a consuming post-dominance "
                      "failure!\n";
      if (auto v = value) {
        llvm::errs() << "Value: " << *value;
      } else {
        llvm::errs() << "Value: N/A\n";
      }
      llvm::errs() << "Post Dominating Failure Blocks:\n";
      for (auto *succBlock : successorBlocksThatMustBeVisited) {
        llvm::errs() << " bb" << succBlock->getDebugID();
      }
      llvm::errs() << '\n';
    });

    // Otherwise... see if we have any other failures. This signals the user
    // wants us to tell it where to insert compensating destroys.
  }

  // If we do have remaining blocks, then these non lifetime ending uses must be
  // outside of our "alive" blocks implying an outside of lifetime use. It could
  // be a use-before-def or a use-after-free.
  for (auto pair : blocksWithNonConsumingUses.getRange()) {
    auto *block = pair.first;
    if (deBlocks && deBlocks->isDeadEnd(block)) {
      continue;
    }

    auto useList = pair.second;
    for (auto *use : useList) {
      if (nonConsumingUseOutsideLifetimeCallback) {
        (*nonConsumingUseOutsideLifetimeCallback)(use);
      }

      errorBuilder.handleUseOutsideOfLifetime([&] {
        llvm::errs() << "Found outside of lifetime use!\n"
                     << "Value: ";
        if (auto v = value) {
          llvm::errs() << *v;
        } else {
          llvm::errs() << "N/A. \n";
        }

        llvm::errs() << "User:" << *use->getUser() << "Block: bb"
                     << block->getDebugID() << "\n";
      });
    }
  }
}

//===----------------------------------------------------------------------===//
//                           Top Level Entrypoints
//===----------------------------------------------------------------------===//

LinearLifetimeChecker::Error LinearLifetimeChecker::checkValueImpl(
    SILValue value, ArrayRef<Operand *> consumingUses,
    ArrayRef<Operand *> nonConsumingUses, ErrorBuilder &errorBuilder,
    llvm::Optional<function_ref<void(SILBasicBlock *)>> leakingBlockCallback,
    llvm::Optional<function_ref<void(Operand *)>>
        nonConsumingUseOutsideLifetimeCallback) {
  // FIXME: rdar://71240363. This assert does not make sense because
  // consumingUses in some cases only contains the destroying uses. Owned values
  // may not be destroyed because they may be converted to
  // ValueOwnershipKind::None on all paths reaching a return. Instead, this
  // utility needs to find liveness first considering all uses (or at least all
  // uses that may be on a lifetime boundary). We probably then won't need this
  // assert, but I'm leaving the FIXME as a placeholder for that work.
  //
  // assert((!consumingUses.empty()
  //        || deadEndBlocks.isDeadEnd(value->getParentBlock())) &&
  //       "Must have at least one consuming user?!");

  State state(value, errorBuilder, leakingBlockCallback,
              nonConsumingUseOutsideLifetimeCallback, consumingUses,
              nonConsumingUses);

  // First add our non-consuming uses and their blocks to the
  // blocksWithNonConsumingUses map. While we do this, if we have multiple uses
  // in the same block, we only accept the last use since from a liveness
  // perspective that is all we care about.
  state.initializeAllNonConsumingUses(nonConsumingUses);

  // Then, we go through each one of our consuming users performing the
  // following operation:
  //
  // 1. Verifying that no two consuming users are in the same block. This
  // is accomplished by adding the user blocks to the blocksWithConsumingUsers
  // list. This avoids double consumes.
  //
  // 2. Verifying that no predecessor is a block with a consuming use. The
  // reason why this is necessary is because we wish to not add elements to the
  // worklist twice. Thus we want to check if we have already visited a
  // predecessor.
  SmallVector<std::pair<Operand *, SILBasicBlock *>, 32> predsToAddToWorklist;
  state.initializeAllConsumingUses(consumingUses, predsToAddToWorklist);

  // If we have a singular consuming use and it is in the same block as value's
  // def, we bail early. Any use-after-frees due to non-consuming uses would
  // have been detected by initializing our consuming uses. So we are done.
  if (consumingUses.size() == 1 &&
      consumingUses[0]->getUser()->getParent() == value->getParentBlock()) {
    // Check if any of our non consuming uses are not in the parent block and
    // are reachable. We flag those as additional use after frees. Any in the
    // same block, we would have flagged.
    for (auto *use : nonConsumingUses) {
      auto *useParent = use->getUser()->getParent();
      if (useParent == value->getParentBlock() ||
          (deadEndBlocks && deadEndBlocks->isDeadEnd(useParent))) {
        continue;
      }

      if (nonConsumingUseOutsideLifetimeCallback) {
        (*nonConsumingUseOutsideLifetimeCallback)(use);
      }

      state.errorBuilder.handleUseOutsideOfLifetime([&] {
        llvm::errs() << "Function: '" << value->getFunction()->getName()
                     << "'\n"
                     << "Found non consuming use outside of the lifetime being "
                        "verified.\n"
                     << "Value: " << *value << "User: " << *use->getUser();
      });
    }

    return std::move(state.errorBuilder).consumeAndGetFinalError();
  }

  // Ok, we may have multiple consuming uses. Add the user block of each of our
  // consuming users to the visited list since we do not want them to be added
  // to the successors to visit set.
  for (const auto &i : consumingUses) {
    state.visitedBlocks.insert(i->getUser()->getParent());
  }

  // Now that we have marked all of our producing blocks, we go through our
  // predsToAddToWorklist list and add our preds, making sure that none of these
  // preds are in blocksWithConsumingUses. This is important so that we do not
  // need to re-process.
  for (auto pair : predsToAddToWorklist) {
    Operand *use = pair.first;
    SILBasicBlock *predBlock = pair.second;

    // Make sure that the predecessor is not in our blocksWithConsumingUses
    // list.
    state.checkPredsForDoubleConsume(use, predBlock);

    if (!state.visitedBlocks.insert(predBlock))
      continue;

    state.worklist.push_back(predBlock);
  }

  // Now that our algorithm is completely prepared, run the
  // dataflow... If we find a failure, return false.
  state.performDataflow(deadEndBlocks);

  // ...and then check that the end state shows that we have a valid linear
  // typed value.
  state.checkDataflowEndState(deadEndBlocks);
  return std::move(state.errorBuilder).consumeAndGetFinalError();
}

LinearLifetimeChecker::Error LinearLifetimeChecker::checkValue(
    SILValue value, ArrayRef<Operand *> consumingUses,
    ArrayRef<Operand *> nonConsumingUses, ErrorBuilder &errorBuilder) {
  return checkValueImpl(value, consumingUses, nonConsumingUses, errorBuilder,
                        llvm::None, llvm::None);
}

LinearLifetimeChecker::Error LinearLifetimeChecker::checkValue(
    SILValue value, ArrayRef<Operand *> consumingUses,
    ArrayRef<Operand *> nonConsumingUses, ErrorBuilder &errorBuilder,
    function_ref<void(SILBasicBlock *)> leakingBlocksCallback) {
  return checkValueImpl(value, consumingUses, nonConsumingUses, errorBuilder,
                        leakingBlocksCallback, llvm::None);
}

bool LinearLifetimeChecker::completeConsumingUseSet(
    SILValue value, Operand *consumingUse,
    function_ref<void(SILBasicBlock::iterator)> visitor) {
  ErrorBuilder errorBuilder(*value->getFunction(),
                            ErrorBehaviorKind::ReturnFalse);
  auto error =
      checkValue(value, {consumingUse}, {}, errorBuilder,
                 [&](SILBasicBlock *block) { return visitor(block->begin()); });

  if (!error.getFoundError()) {
    return false;
  }

  // Return true if we found an over consume (meaning our use is in a loop).
  return error.getFoundOverConsume();
}

bool LinearLifetimeChecker::validateLifetime(
    SILValue value, ArrayRef<Operand *> consumingUses,
    ArrayRef<Operand *> nonConsumingUses) {
  ErrorBuilder errorBuilder(*value->getFunction(),
                            ErrorBehaviorKind::ReturnFalse);
  return !checkValue(value, consumingUses, nonConsumingUses, errorBuilder)
              .getFoundError();
}

bool LinearLifetimeChecker::usesNotContainedWithinLifetime(
    SILValue value, ArrayRef<Operand *> consumingUses,
    ArrayRef<Operand *> usesToTest) {
  auto errorBehavior = ErrorBehaviorKind(
      ErrorBehaviorKind::ReturnFalse |
      ErrorBehaviorKind::StoreNonConsumingUsesOutsideLifetime);
  ErrorBuilder errorBuilder(*value->getFunction(), errorBehavior);

  using OptType = llvm::Optional<function_ref<void(Operand *)>>;
#ifndef NDEBUG
  SmallVector<Operand *, 32> uniqueUsers;
#endif
  unsigned numFoundUses = 0;
  auto error = checkValueImpl(value, consumingUses, usesToTest, errorBuilder,
                              llvm::None, OptType([&](Operand *use) {
#ifndef NDEBUG
                                uniqueUsers.push_back(use);
#endif
                                ++numFoundUses;
                              }));

#ifndef NDEBUG
  // Make sure in assert builds that we did not double count any operands.
  sortUnique(uniqueUsers);
  assert(numFoundUses == uniqueUsers.size());
#endif

  // If we found any error except for uses outside of our lifetime, bail.
  if (error.getFoundLeak() || error.getFoundOverConsume() ||
      error.getFoundUseAfterFree())
    return false;

  // Return true if we /did/ find an error and when emitting that error, we
  // found /all/ uses we were looking for.
  return error.getFoundUseOutsideOfLifetime() &&
         numFoundUses == usesToTest.size();
}

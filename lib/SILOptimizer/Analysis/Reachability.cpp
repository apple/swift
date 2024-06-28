//=----------- Reachability.cpp - Walking from roots to barriers. -----------=//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/Analysis/Reachability.h"

using namespace swift;

/// Walks backwards from the specified roots to find barrier instructions, phis,
/// and blocks via the isBarrier predicate.
///
/// Implements IterativeBackwardReachability::Effects
/// Implements IterativeBackwardReachability::findBarriers::Visitor
class FindBarriersBackwardDataflow final {
  using Reachability =
      IterativeBackwardReachability<FindBarriersBackwardDataflow>;
  using Effect = Reachability::Effect;
  ArrayRef<SILInstruction *> const roots;
  function_ref<bool(SILInstruction *)> isBarrier;
  ReachableBarriers &barriers;
  Reachability::Result result;
  Reachability reachability;

public:
  FindBarriersBackwardDataflow(SILFunction &function,
                               ArrayRef<SILInstruction *> roots,
                               ArrayRef<SILBasicBlock *> stopBlocks,
                               ReachableBarriers &barriers,
                               function_ref<bool(SILInstruction *)> isBarrier)
      : roots(roots), isBarrier(isBarrier), barriers(barriers),
        result(&function), reachability(&function, stopBlocks, *this, result) {}
  FindBarriersBackwardDataflow(FindBarriersBackwardDataflow const &) = delete;
  FindBarriersBackwardDataflow &
  operator=(FindBarriersBackwardDataflow const &) = delete;

  void run();

private:
  friend Reachability;

  /// IterativeBackwardReachability::Effects

  auto gens() { return roots; }

  Effect effectForInstruction(SILInstruction *);
  Effect effectForPhi(SILBasicBlock *);

  auto localGens() { return result.localGens; }

  bool isLocalGen(SILInstruction *instruction) {
    return result.localGens.contains(instruction);
  }

  /// IterativeBackwardReachability::findBarriers::Visitor

  void visitBarrierInstruction(SILInstruction *instruction) {
    barriers.instructions.push_back(instruction);
  }

  void visitBarrierPhi(SILBasicBlock *block) { barriers.phis.push_back(block); }

  void visitBarrierBlock(SILBasicBlock *block) {
    barriers.edges.push_back(block);
  }

  void visitInitialBlock(SILBasicBlock *block) {
    barriers.initialBlocks.push_back(block);
  }
};

FindBarriersBackwardDataflow::Effect
FindBarriersBackwardDataflow::effectForInstruction(
    SILInstruction *instruction) {
  if (llvm::is_contained(roots, instruction))
    return Effect::Gen();
  auto barrier = isBarrier(instruction);
  return barrier ? Effect::Kill() : Effect::NoEffect();
}

FindBarriersBackwardDataflow::Effect
FindBarriersBackwardDataflow::effectForPhi(SILBasicBlock *block) {
  assert(llvm::all_of(block->getArguments(),
                      [&](auto argument) { return PhiValue(argument); }));

  bool barrier =
      llvm::any_of(block->getPredecessorBlocks(), [&](auto *predecessor) {
        return isBarrier(predecessor->getTerminator());
      });
  return barrier ? Effect::Kill() : Effect::NoEffect();
}

void FindBarriersBackwardDataflow::run() {
  reachability.initialize();
  reachability.solve();
  reachability.findBarriers(*this);
}

void swift::findBarriersBackward(
    ArrayRef<SILInstruction *> roots, ArrayRef<SILBasicBlock *> initialBlocks,
    SILFunction &function, ReachableBarriers &barriers,
    function_ref<bool(SILInstruction *)> isBarrier) {
  FindBarriersBackwardDataflow flow(function, roots, initialBlocks, barriers,
                                    isBarrier);
  flow.run();
}

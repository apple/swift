//===------- OptimizeHopToExecutor.cpp - optimize hop_to_executor ---------===//
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

#define DEBUG_TYPE "optimize-hop-to-executor"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/MemoryLocations.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SIL/MemAccessUtils.h"

using namespace swift;

namespace {

/// Optimizes hop_to_executor instructions.
///
/// * Redundant hop_to_executor elimination: if a hop_to_executor is dominated
///   by another hop_to_executor with the same operand, it is eliminated:
///   \code
///      hop_to_executor %a
///      ... // no suspension points
///      hop_to_executor %a // can be eliminated
///   \endcode
///
/// * Dead hop_to_executor elimination: if a hop_to_executor is not followed by
///   any code which requires to run on its actor's executor, it is eliminated:
///   \code
///      hop_to_executor %a
///      ... // no instruction which require to run on %a
///      return
///   \endcode
class OptimizeHopToExecutor {

private:

  typedef llvm::DenseMap<SILValue, int> Actors;

  /// Basic-block specific information used for dataflow analysis.
  struct BlockState {
    enum {
      NotSet = -2,
      
      // Used in the forward dataflow in removeRedundantHopToExecutors.
      Unknown = -1,

      // Used in the backward dataflow in removeDeadHopToExecutors.
      ExecutorNeeded = Unknown,
      NoExecutorNeeded = 0,
    };

    static_assert(ExecutorNeeded == Unknown,
      "needed for merge() to correctly merge ExecutorNeeded and NoExecutorNeeded");

    /// The backlink to the SILBasicBlock.
    SILBasicBlock *block = nullptr;

    /// The value at the entry (i.e. the first instruction) of the block.
    int entry = NotSet;
    
    /// The value of the block itself. It's NotSet if the block has no
    /// significant instructions for the dataflow.
    int intra = NotSet;

    /// The value at the exit (i.e. after the terminator) of the block.
    int exit = NotSet;
    
    /// Merge two values at a control-flow merge point.
    static int merge(int lhs, int rhs) {
      if (lhs == NotSet || lhs == rhs)
        return rhs;
      if (rhs == NotSet)
        return lhs;
      return Unknown;
    }
  };

  SILFunction *function;

  /// All block states.
  std::vector<BlockState> blockStates;

  llvm::DenseMap<SILBasicBlock *, BlockState *> block2State;

  void collectActors(Actors &actors);

  void allocateBlockStates();

  void solveDataflowForward();
  void solveDataflowBackward();

  bool removeRedundantHopToExecutors(const Actors &actors);

  bool removeDeadHopToExecutors();

  static void updateNeedExecutor(int &needExecutor, SILInstruction *inst);
  static bool needsExecutor(SILInstruction *inst);
  static bool isGlobalMemory(SILValue addr);

public:

  OptimizeHopToExecutor(SILFunction *function) : function(function) { }

  /// The entry point to the transformation.
  bool run();

  void dump();
};

/// Search for hop_to_executor instructions and add their operands to \p actors.
void OptimizeHopToExecutor::collectActors(Actors &actors) {
  int uniqueActorID = 0;
  for (SILBasicBlock &block : *function) {
    for (SILInstruction &inst : block) {
      if (auto *hop = dyn_cast<HopToExecutorInst>(&inst)) {
        auto oper = hop->getOperand();

        if (actors.count(oper))
          continue;

        actors[oper] = uniqueActorID++;
      }
    }
  }
}

/// Initialize blockStates and block2State.
void OptimizeHopToExecutor::allocateBlockStates() {
  // Resizing is mandatory! Just adding states with push_back would potentially
  // invalidate previous pointers to states, which are stored in block2State.
  blockStates.resize(function->size());

  for (auto blockAndIdx : llvm::enumerate(*function)) {
    BlockState *state = &blockStates[blockAndIdx.index()];
    state->block = &blockAndIdx.value();
    block2State[&blockAndIdx.value()] = state;
  }
}

/// Solve the dataflow in forward direction.
void OptimizeHopToExecutor::solveDataflowForward() {
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (BlockState &state : blockStates) {
      int newEntry = state.entry;
      for (SILBasicBlock *pred : state.block->getPredecessorBlocks()) {
        newEntry = BlockState::merge(newEntry, block2State[pred]->exit);
      }
      if (newEntry != state.entry || firstRound) {
        changed = true;
        state.entry = newEntry;
        if (state.intra == BlockState::NotSet)
          state.exit = state.entry;
      }
    }
    firstRound = false;
  } while (changed);
}

/// Solve the dataflow in backward direction.
void OptimizeHopToExecutor::solveDataflowBackward() {
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (BlockState &state : llvm::reverse(blockStates)) {
      int newExit = state.exit;
      for (SILBasicBlock *succ : state.block->getSuccessorBlocks()) {
        newExit = BlockState::merge(newExit, block2State[succ]->entry);
      }
      if (newExit != state.exit || firstRound) {
        changed = true;
        state.exit = newExit;
        if (state.intra == BlockState::NotSet)
          state.entry = state.exit;
      }
    }
    firstRound = false;
  } while (changed);
}

/// Returns true if \p inst is a suspension point or an async call.
static bool isSuspensionPoint(SILInstruction *inst) {
  if (auto applySite = FullApplySite::isa(inst)) {
    if (applySite.isAsync())
      return true;
    return false;
  }
  if (isa<AwaitAsyncContinuationInst>(inst))
    return true;
  return false;
}

/// Remove hop_to_executor instructions which are dominated by another
/// hop_to_executor with the same operand.
/// See the top-level comment on OptimizeHopToExecutor for details.
bool OptimizeHopToExecutor::removeRedundantHopToExecutors(const Actors &actors) {

  // Initialize the dataflow.
  for (BlockState &state : blockStates) {
    state.entry = (state.block == function->getEntryBlock() ?
                     BlockState::Unknown : BlockState::NotSet);
    state.intra = BlockState::NotSet;
    for (SILInstruction &inst : *state.block) {
      if (isSuspensionPoint(&inst)) {
        // A suspension point (like an async call) can switch to another
        // executor.
        state.intra = BlockState::Unknown;
      } else if (auto *hop = dyn_cast<HopToExecutorInst>(&inst)) {
        state.intra = actors.lookup(hop->getOperand());
      }
    }
    state.exit = state.intra;
  }

  solveDataflowForward();

  // Last step: do the transformation.
  bool changed = false;
  for (BlockState &state : blockStates) {
    // Iterating over all instructions is the same logic as above, just start
    // with the final entry-value.
    int actorIdx = state.entry;
    for (auto iter = state.block->begin(); iter != state.block->end();) {
      SILInstruction *inst = &*iter++;
      if (isSuspensionPoint(inst)) {
        actorIdx = BlockState::Unknown;
        continue;
      }
      auto *hop = dyn_cast<HopToExecutorInst>(inst);
      if (!hop)
        continue;

      int newActorIdx = actors.lookup(hop->getOperand());
      if (newActorIdx != actorIdx) {
        actorIdx = newActorIdx;
        continue;
      }
      if (hop->isMandatory())
        continue;

      // There is a dominating hop_to_executor with the same operand.
      LLVM_DEBUG(llvm::dbgs() << "Redundant executor " << *hop);
      hop->eraseFromParent();
      changed = true;
    }
    assert(actorIdx == state.exit);
  }
  return changed;
}

/// Remove hop_to_executor instructions which are not followed by any code which
/// requires to run on the actor's executor.
/// See the top-level comment on OptimizeHopToExecutor for details.
bool OptimizeHopToExecutor::removeDeadHopToExecutors() {

  // Initialize the dataflow: go bottom up and if we see any instruction which
  // might require a dedicated executor, don't remove a preceding
  // hop_to_executor instruction.
  for (BlockState &state : blockStates) {
    state.exit = (state.block->getSuccessors().empty() ?
                    BlockState::NoExecutorNeeded : BlockState::NotSet);
    state.intra = BlockState::NotSet;
    for (SILInstruction &inst : llvm::reverse(*state.block)) {
      updateNeedExecutor(state.intra, &inst);
    }
    state.entry = state.intra;
  }

  solveDataflowBackward();

  // Last step: do the transformation.
  bool changed = false;
  for (BlockState &state : blockStates) {
    // Iterating over all instructions is the same logic as above, just start
    // with the final exit-value.
    int needActor = state.exit;
    for (auto iter = state.block->rbegin(); iter != state.block->rend();) {
      SILInstruction *inst = &*iter++;
      auto *hop = dyn_cast<HopToExecutorInst>(inst);
      if (hop && !hop->isMandatory()
          && needActor == BlockState::NoExecutorNeeded) {
        // Remove the dead hop_to_executor.
        LLVM_DEBUG(llvm::dbgs() << "Dead executor " << *hop);
        hop->eraseFromParent();
        changed = true;
        continue;
      }
      updateNeedExecutor(needActor, inst);
    }
    assert(needActor == state.entry);
  }
  return changed;
}

/// Updates \p needExecutor for the dataflow evaluation.
void OptimizeHopToExecutor::updateNeedExecutor(int &needExecutor,
                                            SILInstruction *inst) {
  if (isa<HopToExecutorInst>(inst)) {
    needExecutor = BlockState::NoExecutorNeeded;
    return;
  }
  if (isSuspensionPoint(inst)) {
    needExecutor = BlockState::NoExecutorNeeded;
    return;
  }
  if (needsExecutor(inst))
    needExecutor = BlockState::ExecutorNeeded;
}

/// Returns true if \p inst needs to run on a specific executor.
bool OptimizeHopToExecutor::needsExecutor(SILInstruction *inst) {
  // TODO: Is this the correct thing to check?
  if (auto *load = dyn_cast<LoadInst>(inst)) {
    return isGlobalMemory(load->getOperand());
  }
  if (auto *store = dyn_cast<StoreInst>(inst)) {
    return isGlobalMemory(store->getDest());
  }
  if (auto *copy = dyn_cast<CopyAddrInst>(inst)) {
    return isGlobalMemory(copy->getSrc()) || isGlobalMemory(copy->getDest());
  }
  // BeginBorrowInst and EndBorrowInst currently have
  // MemoryBehavior::MayHaveSideEffects.  Fixing that is tracked by
  // rdar://111875527.  These instructions only have effects in the sense of
  // memory dependencies, which aren't relevant for hop_to_executor
  // elimination.
  if (isa<BeginBorrowInst>(inst) || isa<EndBorrowInst>(inst)) {
    return false;
  }
  return inst->mayReadOrWriteMemory();
}

bool OptimizeHopToExecutor::isGlobalMemory(SILValue addr) {
  // TODO: use escape analysis to rule out locally allocated non-stack objects.
  SILValue base = getAccessBase(addr);
  return !isa<AllocStackInst>(base);
}

bool OptimizeHopToExecutor::run() {
  Actors actors;
  collectActors(actors);
  if (actors.empty())
    return false;

  allocateBlockStates();

  bool changed = removeRedundantHopToExecutors(actors);
  changed |= removeDeadHopToExecutors();
 
  return changed;
}

LLVM_ATTRIBUTE_USED void OptimizeHopToExecutor::dump() {
  for (BlockState &state : blockStates) {
    llvm::dbgs() << "bb" << state.block->getDebugID() <<
      ": entry=" << state.entry <<
      ", intra=" << state.intra <<
      ", exit=" << state.exit << '\n';
  }
}

class OptimizeHopToExecutorPass : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    if (!getFunction()->isAsync())
      return;

    OptimizeHopToExecutor optimizeHopToExecutor(getFunction());
    if (optimizeHopToExecutor.run())
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createOptimizeHopToExecutor() {
  return new OptimizeHopToExecutorPass();
}

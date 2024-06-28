//===--- MovedAsyncVarDebugInfoPropagator.cpp -----------------------------===//
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
///
/// \file
///
/// This file contains a forward optimistic dataflow with intersection merging
/// that propagates debug instructions of moved async vars after async funclet
/// points where they are available. The reason that we are doing this is that
/// during LLVM, the CoroSplitter will split such functions into several
/// coroutine funclets. Rather than teaching LLVM heuristics to understand how
/// Swift needs to emit debug info, we pre-propagate debug info for moved values
/// so that after splitting the dbg info is in the appropriate place.
///
/// The lattice that we use for each Debug Variable we are tracking is as
/// follows:
///
///    uninitialized
///      /   |   \
///     v    v    v
///    a     b   no value
///     \    |    /
///      v   v   v
///        undef
///
/// Where:
///
///   * a, b are SIL debug info instructions associated with the
///     SILDebugVariable. We can always map such instructions to a SILValue that
///     we can use to create new debug_value instructions for the
///     SILDebugVariable.
///
///   * "no value" states that currently the SILDebugVariable does not have any
///     specific instruction associated with it from a dataflow perspective. An
///     example of where this would be used is in conditional control flow where
///     a variable's definition is not defined since the block is not dominated
///     by the variable's definition. This is the state that all blocks state is
///     initialized in since we refactor uninitialized into a separate bit on
///     the block state (see below).
///
///   * uninitialized is the state of a block before it has any out dataflow
///     state. This is represented as a bit in each block state that specifies
///     if the block has ever had its out dataflow computed. If a predecessor
///     block's state has this bit state, we skip it when intersected. We do not
///     represent this in the block's per SILDebugVariable state since we
///     perform the dataflow for all values all at the same time meaning that a
///     bit works well.
///
///   * undef is bottom. When we intersect two values of the lattice and they do
///     not match, we go to undef.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-async-var-debuginfo-propagator"

#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Format.h"
#include <cstring>

using namespace swift;

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

/// Clone \p original changing the clone's operand to be undef and insert at the
/// beginning of \p block.
static DebugVarCarryingInst
cloneDebugValueMakeUndef(DebugVarCarryingInst original, SILBasicBlock *block) {
  SILBuilderWithScope builder(&block->front());
  builder.setCurrentDebugScope(original->getDebugScope());
  auto *undef = SILUndef::get(original.getOperandForDebugValueClone());
  return builder.createDebugValue(original->getLoc(), undef,
                                  *original.getVarInfo(), false,
                                  UsesMoveableValueDebugInfo);
}

static DebugVarCarryingInst
cloneDebugValueMakeUndef(DebugVarCarryingInst original,
                         SILInstruction *insertPt) {
  SILBuilderWithScope builder(std::next(insertPt->getIterator()));
  builder.setCurrentDebugScope(original->getDebugScope());
  auto *undef = SILUndef::get(original.getOperandForDebugValueClone());
  return builder.createDebugValue(original->getLoc(), undef,
                                  *original.getVarInfo(), false,
                                  UsesMoveableValueDebugInfo);
}

static SILInstruction *cloneDebugValue(DebugVarCarryingInst original,
                                       SILInstruction *insertPt) {
  if (original.getSpareBits())
    return *cloneDebugValueMakeUndef(original, insertPt);

  SILBuilderWithScope builder(std::next(insertPt->getIterator()));
  builder.setCurrentDebugScope(original->getDebugScope());
  return builder.createDebugValue(
      original->getLoc(), original.getOperandForDebugValueClone(),
      *original.getVarInfo(), false, UsesMoveableValueDebugInfo);
}

static SILInstruction *cloneDebugValue(DebugVarCarryingInst original,
                                       SILBasicBlock *block) {
  if (original.getSpareBits())
    return *cloneDebugValueMakeUndef(original, block);

  SILBuilderWithScope builder(&block->front());
  builder.setCurrentDebugScope(original->getDebugScope());
  return builder.createDebugValue(
      original->getLoc(), original.getOperandForDebugValueClone(),
      *original.getVarInfo(), false, UsesMoveableValueDebugInfo);
}

namespace {

/// An ADT wrapping a mutable array ref with extra methods used by the pass for
/// convenience. The author just wished to avoid writing memcpy/memset/memcmp
/// multiple times by hand and potentially messing up. This /should/ be a swift
/// extension on MutableArrayRef in truth.
struct DebugInstMutableArrayRef {
  MutableArrayRef<DebugVarCarryingInst> state;

  unsigned getNumBytes() const {
    return sizeof(DebugVarCarryingInst) * state.size();
  }

  DebugVarCarryingInst &getElt(unsigned index) const { return state[index]; }

  /// Set all of state to be no tracked value.
  void setZero() { memset(state.data(), 0, getNumBytes()); }

  /// Use memcpy to copy the state of \p other into this data structure.
  void copy(DebugInstMutableArrayRef other) {
    assert(state.size() == other.state.size());
    memcpy(state.data(), other.state.data(), getNumBytes());
  }

  bool operator==(DebugInstMutableArrayRef other) const {
    assert(state.size() == other.state.size());
    return memcmp(state.data(), other.state.data(), getNumBytes()) == 0;
  }

  bool operator!=(DebugInstMutableArrayRef other) const {
    return !(*this == other);
  }

  unsigned size() const { return state.size(); }

  void cloneAfterInsertPt(SILInstruction *insertPt) {
    LLVM_DEBUG(llvm::dbgs()
               << "Cloning debug info at insert pt: " << *insertPt);
    if (!isa<TermInst>(insertPt)) {
      for (auto value : state) {
        if (!value)
          continue;
        LLVM_DEBUG(llvm::dbgs() << "    Inst to clone: " << **value);
        cloneDebugValue(value, insertPt);
      }
      return;
    }

    // Ok, we have a term inst, clone into our successors.
    auto *ti = cast<TermInst>(insertPt);
    for (auto *succBlock : ti->getSuccessorBlocks()) {
      for (auto value : state) {
        if (!value)
          continue;
        LLVM_DEBUG(llvm::dbgs() << "    Inst to clone: " << **value);
        cloneDebugValue(value, succBlock);
      }
    }
  }

  void cloneUndefOnlyAfterInsertPt(SILBasicBlock *insertBlock) {
    LLVM_DEBUG(llvm::dbgs() << "Cloning debug info for undef at block: bb"
                            << insertBlock->getDebugID() << '\n');
    for (auto value : state) {
      if (!value || !value.getSpareBits())
        continue;
      LLVM_DEBUG(llvm::dbgs() << "    Inst to clone: " << **value);
      cloneDebugValueMakeUndef(value, insertBlock);
    }
  }
};

} // namespace

/// Returns true if a new coroutine funclet begins immediately after this
/// instruction.
///
/// NOTE: \p inst could be a terminator if this is a yield!
static bool isAsyncFuncletEdge(SILInstruction *inst) {
  // This handles begin_apply.
  if (auto fas = FullApplySite::isa(inst)) {
    if (fas.beginsCoroutineEvaluation() || fas.isAsync())
      return true;
  }
  if (isa<HopToExecutorInst>(inst))
    return true;
  if (isa<EndApplyInst>(inst) || isa<AbortApplyInst>(inst))
    return true;
  return isa<YieldInst>(inst);
}

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

namespace {

struct BlockState {
  DebugInstMutableArrayRef inState;
  DebugInstMutableArrayRef outState;
  DebugInstMutableArrayRef genSet;

  /// Set to true by default so we can skip it when merging predecessors.
  bool uninitializedOutState = true;

  /// Set to true if this block had /any/ async edges within it. We use this to
  /// limit the amount of blocks whose instructions we need to visit in our
  /// final pass over the IR.
  bool containsAsyncEdge = false;

  void dump() const {
    llvm::dbgs() << "BlockState.\n";
    llvm::dbgs() << "Uninit Out State: "
                 << (uninitializedOutState ? "true" : "false") << '\n';
    llvm::dbgs() << "Contains Async Edge: "
                 << (containsAsyncEdge ? "true" : "false") << '\n';
    llvm::dbgs() << "InState.\n";
    for (unsigned i : range(inState.size())) {
      llvm::dbgs() << "[" << i << "] = "
                   << llvm::format_hex(uintptr_t(*inState.getElt(i)), 16)
                   << '\n';
    }
    llvm::dbgs() << "GenSet.\n";
    for (unsigned i : range(genSet.size())) {
      llvm::dbgs() << "[" << i << "] = "
                   << llvm::format_hex(uintptr_t(*genSet.getElt(i)), 16)
                   << '\n';
    }
    llvm::dbgs() << "OutSet.\n";
    for (unsigned i : range(outState.size())) {
      llvm::dbgs() << "[" << i << "] = "
                   << llvm::format_hex(uintptr_t(*outState.getElt(i)), 16)
                   << '\n';
    }
  }
};

} // namespace

namespace {

struct DebugInfoPropagator {
  SILFunction *fn;

  /// Set to true if we find /any/ func lets. We use this to know if we should
  /// early exit from the function. We purposely do not store this information
  /// on a per block level since we need to iterate over instructions right
  /// before we end... allowing us to save some memory.
  bool foundFuncLets = false;

  /// The total number of blocks in fn. We cache this when we have an
  /// opportunity early to compute this.
  unsigned numBlocks = 0;

  /// A map from a SILDebugVariable to its offset in the dataflow bitvectors
  /// allocated to it.
  ///
  /// The debug variables offset is defined by the count of debug vars we have
  /// seen so far when we see a specific SILDebugVariable the first time. This
  /// ensures that our SILDebugVariables will not change from compiler run to
  /// compiler run.
  llvm::SmallMapVector<SILDebugVariable, unsigned, 4> dbgVarToDbgVarIndexMap;

  /// A multi map from a SILDebugVariable index to the set of generating
  /// DebugVarCarryingInst for the variable within the entire function. Used to
  /// partition easily the set of debug instructions.
  FrozenMultiMap<unsigned, DebugVarCarryingInst> varToGenDbgInstsMultimap;

  /// A dense map that maps each block to the global state that we track for
  /// it. BlockState includes the various dataflow vectors.
  llvm::DenseMap<SILBasicBlock *, BlockState> blockToBlockState;

  /// Storage vector that contains all of our per block state.
  ///
  /// Each block state struct maintains 3 mutable array ref data structures for
  /// our dataflow that point into \p storage.
  std::vector<DebugVarCarryingInst> storage;

  /// The worklist that we use to store blocks that we visit during the global
  /// dataflow.
  std::vector<SILBasicBlock *> worklist;

  DebugInfoPropagator(SILFunction *fn) : fn(fn) {}

  /// Walk the CFG one block at a time finding gen out blocks. We also use this
  /// as an opportunity to cache if a block has async edges to save a little
  /// compile time later.
  void performInitialLocalDataflow();

  /// Initialize our dataflow state. Must run before performGlobalDataflow or
  /// applyDataflow.
  void initializeDataflowState();

  /// Performs the global dataflow.
  ///
  /// We only run this if we have any "GEN"ed debug values that escape to the
  /// end of a block.
  void performGlobalDataflow();

  /// Given that we have initialized the dataflow and performed the global
  /// dataflow if we needed to... apply our dataflow by inserting debug info
  /// instructions as needed.
  bool applyDataflow();

  /// Top level function that performs local dataflow, global dataflow, and then
  /// applies said dataflow.
  bool process();

  unsigned getIndexForDebugVar(const SILDebugVariable &debugVar) {
    // We take advantage of insert not inserting if we already inserted debug
    // var with a count already and return that value. If we did not, we insert
    // with the new count before expanding the set (initializing the map with
    // the correct value).
    auto debugVariable = debugVar;
    debugVariable.DIExpr = debugVariable.DIExpr.getFragmentPart();
    debugVariable.Type = {};
    auto iter = dbgVarToDbgVarIndexMap.insert(
        {debugVariable, dbgVarToDbgVarIndexMap.size()});
    LLVM_DEBUG(if (iter.second) llvm::dbgs()
                   << "Mapping: [" << iter.first->second
                   << "] = " << iter.first->first.Name << '\n';);
    return iter.first->second;
  }

  /// Once we have performed our initial processing to gather up debug info
  /// instructions, this returns the total number of SILDebugVariable slots
  /// needed for our dataflow to be performed.
  unsigned getEltsPerBlock() const {
    assert(varToGenDbgInstsMultimap.isFrozen() &&
           "We do not know the number of elts per block until we freeze the "
           "varToGenDbgInstsMultiMap");
    return dbgVarToDbgVarIndexMap.size();
  }
};

} // namespace

void DebugInfoPropagator::performInitialLocalDataflow() {
  // Map from SILDebugVariable index to the last DebugVarCarryingInst mapped to
  // that SILDebugVariable in the block we are processing.
  llvm::SmallMapVector<unsigned, DebugVarCarryingInst, 4> blockLastGenInst;

  // Walk through the function, mapping SILDebugVariable ->
  // DebugVarCarryingInst. We use our multi-map later to update our gen set once
  // we know how many SILDebugVariable that we actually need to track in each of
  // our block's vectors.
  for (auto &block : *fn) {
    // Track the total number of blocks so that we can use this in
    // initializeDataflowState to initialize enough memory for all of our blocks
    // without needing to iterate over the blocks an additional time.
    ++numBlocks;

    // blockLastGenInst is per block state, so use SWIFT_DEFER to make sure we
    // don't forget to clean it up before processing the next block.
    SWIFT_DEFER { blockLastGenInst.clear(); };

    auto &blockState = blockToBlockState[&block];
    LLVM_DEBUG(llvm::dbgs()
               << "Visiting Block: bb" << block.getDebugID() << '\n');

    for (auto &inst : block) {
      LLVM_DEBUG(llvm::dbgs() << "Visiting inst: " << inst);
      // If we have a funclet edge, just note that we saw one so we can exit
      // early if we do not have any. We are going to actually emit our
      // debug_value with a 2nd pass after we perform dataflow. We could store
      // these, but it probably isn't worth adding an additional SmallVector to
      // BlockState. It would make BlockState even larger and potentially malloc
      // memory if the data structure went large.
      if (isAsyncFuncletEdge(&inst)) {
        LLVM_DEBUG(llvm::dbgs() << "    Found funclet edge!\n");
        blockState.containsAsyncEdge = true;
        foundFuncLets = true;
        continue;
      }

      // If we have a moved debug var carrying instruction (alloc_stack,
      // debug_value, etc)...
      auto debugInst = DebugVarCarryingInst(&inst);
      if (!debugInst) {
        LLVM_DEBUG(llvm::dbgs() << "Found a non debug inst?! Continuing\n");
        continue;
      }

      if (!debugInst.getWasMoved()) {
        LLVM_DEBUG(
            llvm::dbgs()
            << "    Found a moved debug that was moved... continuing!\n");
        continue;
      }

      LLVM_DEBUG(llvm::dbgs() << "Found DebugValueInst!\n");

      // ... and we have a non-empty SILDebugVariable.
      auto debugInfo = debugInst.getVarInfo();
      if (!debugInfo) {
        LLVM_DEBUG(llvm::dbgs() << "        Has no var info?! Skipping!\n");
        continue;
      }

      // If debugInfo is a SILDebugVariable, we haven't seen before, update the
      // dbgVarToOffsetMap with a new offset for it. We are taking advantage of
      // insert not doing anything if debugInfo was already in there.
      unsigned dbgVarIndex = getIndexForDebugVar(*debugInfo);

      // Check if our debug inst is an undef. If so, we store an undef sentinel
      // value. This just means the spare bit is set to 1.
      if (isa<SILUndef>(debugInst.getOperandForDebugValueClone())) {
        debugInst.setSpareBits(1);
      }

      // Destructively update blockLastGenInst with this. This ensures we always
      // take the last debug inst.
      blockLastGenInst[dbgVarIndex] = debugInst;
    }

    LLVM_DEBUG(llvm::dbgs() << "  Postprocessing gen/kill for block: bb"
                            << block.getDebugID() << '\n');

    // Now post-process our state beginning by adding the set of last gened
    // debug var carrying inst to our multi-map.
    for (auto pair : blockLastGenInst) {
      LLVM_DEBUG(llvm::dbgs() << "Gen: " << **pair.second);
      varToGenDbgInstsMultimap.insert(pair.first, pair.second);
    }
  }
}

void DebugInfoPropagator::initializeDataflowState() {
  // Gather up our num elts.
  //
  // NOTE: This is going to be larger than the actual amount of
  // DebugVarCarryingInst per block that we need. This is because we are going
  // to deduplicate debug_value upon the same operand. So we will just not use
  // the now unused (and seemingly dead) other dbg insts. This just simplifies
  // the logic.
  unsigned eltsPerBlock = getEltsPerBlock();
  unsigned numBitSetsPerBlock = 3;
  unsigned totalElements = eltsPerBlock * numBlocks * numBitSetsPerBlock;
  storage.resize(totalElements);

  // Loop over the blocks again, initializing their memory. We do this later
  // since we need to know the total amount of debug var carrying inst, we are
  // going to gen.
  //
  // We also take this opportunity to insert into our worklist the initial set
  // of blocks we process. We begin by inserting all blocks into the worklist.
  MutableArrayRef<DebugVarCarryingInst> storageRef = {storage.data(),
                                                      storage.size()};
  for (auto &block : *fn) {
    worklist.push_back(&block);

    auto &state = blockToBlockState[&block];

    state.inState = {storageRef.take_front(eltsPerBlock)};
    storageRef = storageRef.drop_front(eltsPerBlock);

    state.outState = {storageRef.take_front(eltsPerBlock)};
    storageRef = storageRef.drop_front(eltsPerBlock);

    state.genSet = {storageRef.take_front(eltsPerBlock)};
    storageRef = storageRef.drop_front(eltsPerBlock);
  }

  // Now that our blocks are initialized with state, lets go through all of our
  // gen dbg insts and update the block sets.
  //
  // NOTE: The index (pair.first) is going to be the index of the debug inst
  // rather than the debug inst itself.
  for (auto pair : varToGenDbgInstsMultimap.getRange()) {
    auto dbgInstSet = pair.second;
    for (auto debugInst : dbgInstSet) {
      assert(debugInst && "null DebugVarCarryingInst");
      auto &state = blockToBlockState[debugInst->getParent()];
      state.genSet.getElt(pair.first) = debugInst;
    }
  }
}

void DebugInfoPropagator::performGlobalDataflow() {
  // Ok, now we are all setup to perform our dataflow.
  LLVM_DEBUG(llvm::dbgs() << "Performing dataflow!\n");

  std::vector<SILBasicBlock *> pending;
  SmallPtrSet<SILBasicBlock *, 8> inPendingWorklist;
  std::vector<DebugVarCarryingInst> tmpDataStorage(getEltsPerBlock());
  DebugInstMutableArrayRef tmpData = {MutableArrayRef<DebugVarCarryingInst>(
      tmpDataStorage.data(), tmpDataStorage.size())};

  // We assume worklist is always filled at the top of this loop. We know the
  // first iteration this is true since:
  //
  // 1. If we did not have any gen instructions, we would not get to this
  //    point implying we should have initial blocks.
  //
  // 2. When we go back around the loop, worklist will only have flushed
  //    values within it.
  do {
    while (!worklist.empty()) {
      auto *block = worklist.back();
      worklist.pop_back();
      auto &blockState = blockToBlockState[block];

      LLVM_DEBUG(llvm::dbgs()
                 << "Visiting block: bb" << block->getDebugID() << '\n');
      LLVM_DEBUG(blockState.dump());

      bool visitedFirstPred = false;
      for (auto *pred : block->getPredecessorBlocks()) {
        auto &predBlockState = blockToBlockState[pred];

        LLVM_DEBUG(llvm::dbgs()
                       << "PredBlock: bb" << pred->getDebugID() << '\n';
                   predBlockState.dump());

        // Skip uninitialized preds.
        if (predBlockState.uninitializedOutState) {
          LLVM_DEBUG(llvm::dbgs() << "    Skipping uninit block...\n");
          continue;
        }

        // If this is our first pred, just initialize our instate with that
        // pred.
        if (!visitedFirstPred) {
          LLVM_DEBUG(llvm::dbgs() << "    First pred... initing!\n");
          visitedFirstPred = true;
          blockState.inState.copy(predBlockState.outState);
          continue;
        }

        // Otherwise, lets merge!
        for (unsigned index : range(getEltsPerBlock())) {
          auto &currentValue = blockState.inState.getElt(index);

          // If we already have nothing for this slot...
          if (!currentValue) {
            // Check if we have a value for this slot in our pred... if we do,
            // we will need to insert an invalidating undef here later. So just
            // set currentValue to be dbgVar and set the spare bit to 1 to mark
            // it as an undef. This ensures that when we propagate this into
            // blocks, we have the appropriate SILDebugVariable stored and know
            // the value is undef.
            if (auto dbgVar = predBlockState.outState.getElt(index)) {
              LLVM_DEBUG(llvm::dbgs()
                         << "Invalidating along one path... inserting undef "
                            "at merge point?!\n");
              currentValue = dbgVar;
              currentValue.setSpareBits(1);
            }

            // In either case, we then continue.
            continue;
          }

          // Otherwise, do our intersection.
          if (currentValue == predBlockState.outState.getElt(index))
            continue;

          // If our intersection fails, need to insert later SILUndef
          // debug_value at merge point. Set the spareBit to 1 so we know this
          // is undef.
          currentValue.setSpareBits(1);
          LLVM_DEBUG(llvm::dbgs() << "Invalidating along one path... "
                                     "inserting undef at merge point 2?!\n");
        }
      }

      // Now we have our input set for the top of our block. Copy it into
      // tmpData and then prepare to update it based off of the gen state of
      // the block.
      tmpData.copy(blockState.inState);

      // Now add in our gen set. This overwrites anything acting as a
      // combination of a gen/kill.
      for (unsigned index : range(getEltsPerBlock())) {
        auto &value = tmpData.getElt(index);

        if (auto newValue = blockState.genSet.getElt(index))
          value = newValue;
      }

      // Now compare our tmpData with blockState.outSet. If they are
      // different, copy tmpData into blockState.outSet and add all of our
      // successors to pending.
      if (blockState.uninitializedOutState || tmpData != blockState.outState) {
        blockState.uninitializedOutState = false;
        blockState.outState.copy(tmpData);
        for (auto *succBlock : block->getSuccessorBlocks()) {
          if (inPendingWorklist.insert(succBlock).second) {
            LLVM_DEBUG(llvm::dbgs() << "Adding to pending list: bb"
                                    << succBlock->getDebugID() << '\n');
            pending.push_back(succBlock);
          }
        }
      }
      LLVM_DEBUG(llvm::dbgs() << "After Round.\n"; blockState.dump());
    }

    std::swap(worklist, pending);
    inPendingWorklist.clear();
  } while (!worklist.empty());
}

bool DebugInfoPropagator::applyDataflow() {
  // NOTE: We use the per block inState to accumulate results as we walk the
  // function. This is why we still call initializeDataflowState before calling
  // this function rather than skipping calling said function if we do not need
  // to perform a global dataflow.
  bool madeChange = false;
  for (auto &block : *fn) {
    auto &blockState = blockToBlockState[&block];

    // First before we do anything, dump the current undef state if we have
    // multiple predecessors. This ensures that at merge points, we propagate
    // undef appropriately.
    if (!block.pred_empty() && !block.getSinglePredecessorBlock())
      blockState.inState.cloneUndefOnlyAfterInsertPt(&block);

    // Then check if this block has any async edges in it at all... If we don't
    // have any edges, then we do not need to visit the internal instruction
    // state of the block.
    if (!blockState.containsAsyncEdge)
      continue;

    // Otherwise, we need to walk the block from top to bottom, dumping the
    // current available debug info whenever we see an async funclet
    // boundary. We update our info for in block debug_value that we see.
    for (auto &inst : block) {
      if (isAsyncFuncletEdge(&inst)) {
        blockState.inState.cloneAfterInsertPt(&inst);
        madeChange = true;
        continue;
      }

      // Check if we have a debug inst that we need to update.
      auto debugInst = DebugVarCarryingInst(&inst);
      if (!debugInst || !debugInst.getWasMoved())
        continue;

      auto debugInfo = debugInst.getVarInfo();
      if (!debugInfo)
        continue;

      unsigned offset = dbgVarToDbgVarIndexMap[*debugInfo];
      blockState.inState.getElt(offset) = debugInst;
    }
  }
  return madeChange;
}

bool DebugInfoPropagator::process() {
  // Begin by performing our local dataflow.
  performInitialLocalDataflow();

  // If we didn't find any funclets or any moved gen dbg, just bail.
  if (!foundFuncLets) {
    LLVM_DEBUG(llvm::dbgs() << "Exiting early! No seen func let edges?!\n");
    return false;
  }

  // Ok, we may need to propagate. First sort our multi-maps so they are in
  // multi-map mode.
  varToGenDbgInstsMultimap.setFrozen();

  // Then initialize our dataflow state. We do this whether or not we perform
  // global dataflow since when applying the dataflow we use this state also for
  // block internal propagation of the dataflow.
  initializeDataflowState();

  // Then if we found any debug values that "GEN"ed out of a block, perform our
  // global dataflow.
  if (varToGenDbgInstsMultimap.size()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Found gen out blocks, performing global dataflow!\n");
    performGlobalDataflow();
  } else {
    LLVM_DEBUG(llvm::dbgs()
               << "No gen out blocks! skipping global dataflow!\n");
  }

  // At this point, we have finished performing our dataflow if we needed
  // to. Now we apply the result.
  //
  // NOTE: If we found that we did not need to perform the dataflow, blocks will
  // still have the appropriate state that their input dataflow state will be
  // empty implying that we will just start each block without state, as we
  // wanted to.
  return applyDataflow();
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MovedAsyncVarDebugInfoPropagatorTransform : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();
    LLVM_DEBUG(llvm::dbgs()
               << "*** MovedAsyncVarDebugInfoPropagatorTransform on function: '"
               << fn->getName() << "\"\n");
    DebugInfoPropagator propagator(fn);

    if (propagator.process()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createMovedAsyncVarDebugInfoPropagator() {
  return new MovedAsyncVarDebugInfoPropagatorTransform();
}

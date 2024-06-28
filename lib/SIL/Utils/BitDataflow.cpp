//===--- BitDataflow.cpp --------------------------------------------------===//
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

#define DEBUG_TYPE "bit-dataflow"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/BitDataflow.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/MemoryLocations.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

BitDataflow::BitDataflow(SILFunction *function, unsigned numLocations) :
  blockStates(function, [numLocations](SILBasicBlock *block) {
    return BlockState(numLocations);
  }) {}

void BitDataflow::entryReachabilityAnalysis() {
  llvm::SmallVector<SILBasicBlock *, 16> workList;
  auto entry = blockStates.entry();
  entry.data.reachableFromEntry = true;
  workList.push_back(&entry.block);

  while (!workList.empty()) {
    SILBasicBlock *block = workList.pop_back_val();
    for (SILBasicBlock *succ : block->getSuccessorBlocks()) {
      BlockState &succState = blockStates[succ];
      if (!succState.reachableFromEntry) {
        succState.reachableFromEntry = true;
        workList.push_back(succ);
      }
    }
  }
}

void BitDataflow::exitReachableAnalysis() {
  llvm::SmallVector<SILBasicBlock *, 16> workList;
  for (auto bd : blockStates) {
    if (bd.block.getTerminator()->isFunctionExiting()) {
      bd.data.exitReachability = ExitReachability::ReachesExit;
      workList.push_back(&bd.block);
    } else if (isa<UnreachableInst>(bd.block.getTerminator())) {
      bd.data.exitReachability = ExitReachability::ReachesUnreachable;
      workList.push_back(&bd.block);
    }
  }
  while (!workList.empty()) {
    SILBasicBlock *block = workList.pop_back_val();
    BlockState &state = blockStates[block];
    for (SILBasicBlock *pred : block->getPredecessorBlocks()) {
      BlockState &predState = blockStates[pred];
      if (predState.exitReachability < state.exitReachability) {
        // As there are 3 states, each block can be put into the workList 2
        // times maximum.
        predState.exitReachability = state.exitReachability;
        workList.push_back(pred);
      }
    }
  }
}

void BitDataflow::solveForward(JoinOperation join) {
  // Pretty standard data flow solving.
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (auto bd : blockStates) {
      Bits bits = bd.data.entrySet;
      assert(!bits.empty());
      for (SILBasicBlock *pred : bd.block.getPredecessorBlocks()) {
        join(bits, blockStates[pred].exitSet);
      }
      if (firstRound || bits != bd.data.entrySet) {
        changed = true;
        bd.data.entrySet = bits;
        bits |= bd.data.genSet;
        bits.reset(bd.data.killSet);
        bd.data.exitSet = bits;
      }
    }
    firstRound = false;
  } while (changed);
}

void BitDataflow::solveForwardWithIntersect() {
  solveForward([](Bits &entry, const Bits &predExit){
    entry &= predExit;
  });
}

void BitDataflow::solveForwardWithUnion() {
  solveForward([](Bits &entry, const Bits &predExit){
    entry |= predExit;
  });
}

void BitDataflow::solveBackward(JoinOperation join) {
  // Pretty standard data flow solving.
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (auto bd : llvm::reverse(blockStates)) {
      Bits bits = bd.data.exitSet;
      assert(!bits.empty());
      for (SILBasicBlock *succ : bd.block.getSuccessorBlocks()) {
        join(bits, blockStates[succ].entrySet);
      }
      if (firstRound || bits != bd.data.exitSet) {
        changed = true;
        bd.data.exitSet = bits;
        bits |= bd.data.genSet;
        bits.reset(bd.data.killSet);
        bd.data.entrySet = bits;
      }
    }
    firstRound = false;
  } while (changed);
}

void BitDataflow::solveBackwardWithIntersect() {
  solveBackward([](Bits &entry, const Bits &predExit){
    entry &= predExit;
  });
}

void BitDataflow::solveBackwardWithUnion() {
  solveBackward([](Bits &entry, const Bits &predExit){
    entry |= predExit;
  });
}

void BitDataflow::dump() const {
    for (auto bd : blockStates) {
    llvm::dbgs() << "bb" << bd.block.getDebugID() << ":\n"
                 << "    entry: " << bd.data.entrySet << '\n'
                 << "    gen:   " << bd.data.genSet << '\n'
                 << "    kill:  " << bd.data.killSet << '\n'
                 << "    exit:  " << bd.data.exitSet << '\n';
  }
}

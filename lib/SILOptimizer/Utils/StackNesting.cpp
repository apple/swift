//===--- StackNesting.cpp - Utility for stack nesting  --------------------===//
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

#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "llvm/Support/Debug.h"

using namespace swift;

void StackNesting::setup() {
  SmallVector<SILBasicBlock *, 8> WorkList;

  // Start with the function entry block and add blocks while walking down along
  // the successor edges.
  // This ensures a correct ordering of stack locations: an inner location has
  // a higher bit-number than it's outer parent location.
  // This ordering is only important for inserting multiple deallocation
  // instructions (see below).
  auto Entry = BlockInfos.entry();
  WorkList.push_back(&Entry.block);
  Entry.data.visited = true;

  while (!WorkList.empty()) {
    SILBasicBlock *Block = WorkList.pop_back_val();
    BlockInfo &BI = BlockInfos[Block];
    for (SILInstruction &I : *Block) {
      if (I.isAllocatingStack()) {
        auto Alloc = cast<SingleValueInstruction>(&I);
        // Register this stack location.
        unsigned CurrentBitNumber = StackLocs.size();
        StackLoc2BitNumbers[Alloc] = CurrentBitNumber;
        StackLocs.push_back(StackLoc(Alloc));

        BI.StackInsts.push_back(Alloc);
      } else if (I.isDeallocatingStack()) {
        auto *AllocInst = cast<SingleValueInstruction>(I.getOperand(0));
        if (!BI.StackInsts.empty() && BI.StackInsts.back() == AllocInst) {
          // As an optimization, we ignore perfectly nested alloc-dealloc pairs
          // inside a basic block.
          // Actually, this catches most of the cases and keeps our bitsets
          // small.
          assert(StackLocs.back().Alloc == AllocInst);
          StackLocs.pop_back();
          BI.StackInsts.pop_back();
        } else {
          // Register the stack deallocation.
          BI.StackInsts.push_back(&I);
        }
      }
    }
    for (SILBasicBlock *SuccBB : Block->getSuccessorBlocks()) {
      BlockInfo &SuccBI = BlockInfos[SuccBB];
      if (!SuccBI.visited) {
        // Push the next reachable block onto the WorkList.
        WorkList.push_back(SuccBB);
        SuccBI.visited = true;
      }
    }
  }

  unsigned NumLocs = StackLocs.size();
  for (unsigned Idx = 0; Idx < NumLocs; ++Idx) {
    StackLocs[Idx].AliveLocs.resize(NumLocs);
    // Initially each location gets it's own alive-bit.
    StackLocs[Idx].AliveLocs.set(Idx);
  }
}

bool StackNesting::solve() {
  bool changed = false;
  bool isNested = false;
  BitVector Bits(StackLocs.size());

  StackList<SILBasicBlock *> deadEndWorklist(BlockInfos.getFunction());

  // Initialize all bit fields to 1s, expect 0s for the entry block.
  bool initVal = false;
  for (auto bd : BlockInfos) {
    bd.data.AliveStackLocsAtEntry.resize(StackLocs.size(), initVal);
    initVal = true;

    bd.data.isDeadEnd = !bd.block.getTerminator()->isFunctionExiting();
    if (!bd.data.isDeadEnd)
      deadEndWorklist.push_back(&bd.block);
  }

  // Calculate the isDeadEnd block flags.
  while (!deadEndWorklist.empty()) {
    SILBasicBlock *b = deadEndWorklist.pop_back_val();
    for (SILBasicBlock *pred : b->getPredecessorBlocks()) {
      BlockInfo &bi = BlockInfos[pred];
      if (bi.isDeadEnd) {
        bi.isDeadEnd = false;
        deadEndWorklist.push_back(pred);
      }
    }
  }

  // First step: do a forward dataflow analysis to get the live stack locations
  // at the block exits.
  // This is necessary to get the live locations at dead-end blocks (otherwise
  // the backward data flow would be sufficient).
  // The special thing about dead-end blocks is that it's okay to have alive
  // locations at that point (e.g. at an `unreachable`) i.e. locations which are
  // never dealloced. We cannot get such locations with a purly backward dataflow.
  do {
    changed = false;

    for (auto bd : BlockInfos) {
      Bits = bd.data.AliveStackLocsAtEntry;
      for (SILInstruction *StackInst : bd.data.StackInsts) {
        if (StackInst->isAllocatingStack()) {
          Bits.set(bitNumberForAlloc(StackInst));
        } else if (StackInst->isDeallocatingStack()) {
          Bits.reset(bitNumberForDealloc(StackInst));
        }
      }
      if (Bits != bd.data.AliveStackLocsAtExit) {
        bd.data.AliveStackLocsAtExit = Bits;
        changed = true;
      }
      // Merge the bits into the successors.
      for (SILBasicBlock *SuccBB : bd.block.getSuccessorBlocks()) {
        BlockInfos[SuccBB].AliveStackLocsAtEntry &= Bits;
      }
    }
  } while (changed);

  // Second step: do a backward dataflow analysis to extend the lifetimes of
  // no properly nested allocations.
  do {
    changed = false;

    for (auto bd : llvm::reverse(BlockInfos)) {
      // Collect the alive-bits (at the block exit) from the successor blocks.
      for (SILBasicBlock *SuccBB : bd.block.getSuccessorBlocks()) {
        bd.data.AliveStackLocsAtExit |= BlockInfos[SuccBB].AliveStackLocsAtEntry;
      }
      Bits = bd.data.AliveStackLocsAtExit;
      assert(!(bd.data.visited && bd.block.getTerminator()->isFunctionExiting()
               && Bits.any())
             && "stack location is missing dealloc");

      if (bd.data.isDeadEnd) {
        // We treat `unreachable` as an implicit deallocation for all locations
        // which are still alive at this point. The same is true for dead-end
        // CFG regions due to an infinite loop.
        for (int BitNr = Bits.find_first(); BitNr >= 0;
             BitNr = Bits.find_next(BitNr)) {
          // For each alive location extend the lifetime of all locations which
          // are alive at the allocation point. This is the same as we do for
          // a "real" deallocation instruction (see below).
          // In dead-end CFG regions we have to do that for all blocks (because
          // of potential infinite loops), whereas in "normal" CFG regions it's
          // sufficient to do it at deallocation instructions.
          Bits |= StackLocs[BitNr].AliveLocs;
        }
        bd.data.AliveStackLocsAtExit = Bits;
      }
      for (SILInstruction *StackInst : llvm::reverse(bd.data.StackInsts)) {
        if (StackInst->isAllocatingStack()) {
          int BitNr = bitNumberForAlloc(StackInst);
          if (Bits != StackLocs[BitNr].AliveLocs) {
            // More locations are alive around the StackInst's location.
            // Update the AlivaLocs bitset, which contains all those alive
            // locations.
            assert(Bits.test(BitNr) && "no dealloc found for alloc stack");
            StackLocs[BitNr].AliveLocs = Bits;
            changed = true;
            isNested = true;
          }
          // The allocation ends the lifetime of it's stack location (in reverse
          // order)
          Bits.reset(BitNr);
        } else if (StackInst->isDeallocatingStack()) {
          // A stack deallocation begins the lifetime of its location (in
          // reverse order). And it also begins the lifetime of all other
          // locations which are alive at the allocation point.
          Bits |= StackLocs[bitNumberForDealloc(StackInst)].AliveLocs;
        }
      }
      if (Bits != bd.data.AliveStackLocsAtEntry) {
        bd.data.AliveStackLocsAtEntry = Bits;
        changed = true;
      }
    }
  } while (changed);
  
  return isNested;
}

static SILInstruction *createDealloc(SingleValueInstruction *Alloc,
                                     SILInstruction *InsertionPoint,
                                     SILLocation Location) {
  SILBuilderWithScope B(InsertionPoint);
  switch (Alloc->getKind()) {
    case SILInstructionKind::PartialApplyInst:
    case SILInstructionKind::AllocStackInst:
      assert((isa<AllocStackInst>(Alloc) ||
              cast<PartialApplyInst>(Alloc)->isOnStack()) &&
             "wrong instruction");
      return B.createDeallocStack(Location, Alloc);
    case SILInstructionKind::AllocRefDynamicInst:
    case SILInstructionKind::AllocRefInst:
      assert(cast<AllocRefInstBase>(Alloc)->canAllocOnStack());
      return B.createDeallocStackRef(Location, Alloc);
    case SILInstructionKind::AllocPackInst:
      return B.createDeallocPack(Location, Alloc);
    default:
      llvm_unreachable("unknown stack allocation");
  }
}

bool StackNesting::insertDeallocs(const BitVector &AliveBefore,
                                  const BitVector &AliveAfter,
                                  SILInstruction *InsertionPoint,
                                  Optional<SILLocation> Location) {
  if (!AliveBefore.test(AliveAfter))
    return false;

  // The order matters here if we have to insert more than one
  // deallocation. We already ensured in setup() that the bit numbers
  // are allocated in the right order.
  bool changesMade = false;
  for (int LocNr = AliveBefore.find_first(); LocNr >= 0;
       LocNr = AliveBefore.find_next(LocNr)) {
    if (!AliveAfter.test(LocNr)) {
      auto *Alloc = StackLocs[LocNr].Alloc;
      InsertionPoint = createDealloc(Alloc, InsertionPoint,
                   Location.has_value() ? Location.value() : Alloc->getLoc());
      changesMade = true;
    }
  }
  return changesMade;
}

// Insert deallocations at block boundaries.
// This can be necessary for unreachable blocks. Example:
//
//   %1 = alloc_stack
//   %2 = alloc_stack
//   cond_br %c, bb2, bb3
// bb2: <--- need to insert a dealloc_stack %2 at the begin of bb2
//   dealloc_stack %1
//   unreachable
// bb3:
//   dealloc_stack %2
//   dealloc_stack %1
StackNesting::Changes StackNesting::insertDeallocsAtBlockBoundaries() {
  Changes changes = Changes::None;
   for (auto bd : llvm::reverse(BlockInfos)) {
    // Collect the alive-bits (at the block exit) from the successor blocks.
    for (auto succAndIdx : llvm::enumerate(bd.block.getSuccessorBlocks())) {
      BlockInfo &SuccBI = BlockInfos[succAndIdx.value()];
      if (SuccBI.AliveStackLocsAtEntry == bd.data.AliveStackLocsAtExit)
        continue;

      // Insert deallocations for all locations which are alive at the end of
      // the current block, but not at the begin of the successor block.
      SILBasicBlock *InsertionBlock = succAndIdx.value();
      if (!InsertionBlock->getSinglePredecessorBlock()) {
        // If the current block is not the only predecessor of the successor
        // block, we have to insert a new block where we can add the
        // deallocations.
        InsertionBlock = splitEdge(bd.block.getTerminator(), succAndIdx.index());
        changes = Changes::CFG;
      }
      if (insertDeallocs(bd.data.AliveStackLocsAtExit,
                SuccBI.AliveStackLocsAtEntry, &InsertionBlock->front(), None)) {
        if (changes == Changes::None)
          changes = Changes::Instructions;
      }
    }
  }
  return changes;
}

StackNesting::Changes StackNesting::adaptDeallocs() {
  bool InstChanged = false;
  BitVector Bits(StackLocs.size());

  // Visit all blocks. Actually the order doesn't matter, but let's to it in
  // the same order as in solve().
   for (auto bd : llvm::reverse(BlockInfos)) {
    Bits = bd.data.AliveStackLocsAtExit;

    // Insert/remove deallocations inside blocks.
    for (SILInstruction *StackInst : llvm::reverse(bd.data.StackInsts)) {
      if (StackInst->isAllocatingStack()) {
        // For allocations we just update the bit-set.
        int BitNr = bitNumberForAlloc(StackInst);
        assert(Bits == StackLocs[BitNr].AliveLocs &&
               "dataflow didn't converge");
        Bits.reset(BitNr);
      } else if (StackInst->isDeallocatingStack()) {
        // Handle deallocations.
        SILLocation Loc = StackInst->getLoc();
        int BitNr = bitNumberForDealloc(StackInst);
        SILInstruction *InsertionPoint = &*std::next(StackInst->getIterator());
        if (Bits.test(BitNr)) {
          // The location of StackInst is alive after StackInst. So we have to
          // remove this deallocation.
          StackInst->eraseFromParent();
          InstChanged = true;
        } else {
          // Avoid inserting another deallocation for BitNr (which is already
          // StackInst).
          Bits.set(BitNr);
        }

        // Insert deallocations for all locations which are not alive after
        // StackInst but _are_ alive at the StackInst.
        InstChanged |= insertDeallocs(StackLocs[BitNr].AliveLocs, Bits,
                                      InsertionPoint, Loc);
        Bits |= StackLocs[BitNr].AliveLocs;
      }
    }
    assert(Bits == bd.data.AliveStackLocsAtEntry && "dataflow didn't converge");
  }
  return InstChanged ? Changes::Instructions : Changes::None;
}

StackNesting::Changes StackNesting::fixNesting(SILFunction *F) {
  Changes changes = Changes::None;
  {
    StackNesting SN(F);
    if (!SN.analyze())
      return Changes::None;

    // Insert deallocs at block boundaries. This might be necessary in CFG sub
    // graphs which don't  reach a function exit, but only an unreachable.
    changes = SN.insertDeallocsAtBlockBoundaries();
    if (changes == Changes::None) {
      // Do the real work: extend lifetimes by moving deallocs.
      return SN.adaptDeallocs();
    }
  }
  {
    // Those inserted deallocs make it necessary to re-compute the analysis.
    StackNesting SN(F);
    SN.analyze();
    // Do the real work: extend lifetimes by moving deallocs.
    return std::max(SN.adaptDeallocs(), changes);
  }
}

void StackNesting::dump() const {
  for (auto bd : BlockInfos) {
    llvm::dbgs() << "Block " << bd.block.getDebugID();
    if (bd.data.isDeadEnd)
      llvm::dbgs() << "(deadend)";
    llvm::dbgs() << ": entry-bits=";
    dumpBits(bd.data.AliveStackLocsAtEntry);
    llvm::dbgs() << ": exit-bits=";
    dumpBits(bd.data.AliveStackLocsAtExit);
    llvm::dbgs() << '\n';
    for (SILInstruction *StackInst : bd.data.StackInsts) {
      if (StackInst->isAllocatingStack()) {
        auto AllocInst = cast<SingleValueInstruction>(StackInst);
        int BitNr = StackLoc2BitNumbers.lookup(AllocInst);
        llvm::dbgs() << "  alloc #" << BitNr << ": alive=";
        dumpBits(StackLocs[BitNr].AliveLocs);
        llvm::dbgs() << ",     " << *StackInst;
      } else if (StackInst->isDeallocatingStack()) {
        auto *AllocInst = cast<SingleValueInstruction>(StackInst->getOperand(0));
        int BitNr = StackLoc2BitNumbers.lookup(AllocInst);
        llvm::dbgs() << "  dealloc for #" << BitNr << "\n"
                        "    " << *StackInst;
      }
    }
  }
}

void StackNesting::dumpBits(const BitVector &Bits) {
  llvm::dbgs() << '<';
  const char *separator = "";
  for (int Bit = Bits.find_first(); Bit >= 0; Bit = Bits.find_next(Bit)) {
    llvm::dbgs() << separator << Bit;
    separator = ",";
  }
  llvm::dbgs() << '>';
}

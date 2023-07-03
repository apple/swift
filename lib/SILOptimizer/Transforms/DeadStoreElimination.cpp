//===--- DeadStoreElimination.cpp - SIL Dead Store Elimination ------------===//
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
///
/// \file
///
/// This pass eliminates dead stores across basic blocks.
///
/// A store is dead if after the store has occurred:
///
/// 1. The store to pointer is not used along any path to program exit.
/// 2. The store to pointer is overwritten by another store before any
/// potential use of the pointer.
///
/// Dead store elimination (DSE) eliminates such stores by:
///
/// 1. Introducing a notion of a LSLocation that is used to model objects
/// fields. (See below for more details).
///
/// 2. Performing a post-order walk over the control flow graph, tracking any
/// LSLocations that are read from or stored into in each basic block. After
/// eliminating any dead stores in single blocks, it computes a genset and
/// killset for each block. The genset keeps a list of upward visible stores
/// and the killset keeps a list of LSLocation this basic block reads (kills).
///
/// 3. An optimistic iterative dataflow is performed on the genset and killset
/// until convergence.
///
/// At the core of DSE, there is the LSLocation class. a LSLocation is an
/// abstraction of an object field in program. It consists of a base and a
/// projection path to the field accessed.
///
/// When a load or store instruction is encountered, the memory is broken down
/// to the indivisible components, i.e aggregates are broken down to their
/// individual fields using the expand function. This gives the flexibility to
/// find exactly which part of the store is alive and which part is dead.
///
/// After the live parts of the store are determined, they are merged into the
/// minimum number of stores possible using the reduce function. This is done
/// so that we do not bloat SIL IR.
///
/// Another way to implement the DSE optimization is to keep the instructions
/// that read and/or write memory without breaking the memory read/written
/// using the ProjectionPath. However, this can easily lead to loss of
/// opportunities, e.g. a read that only kills part of a store may need to be
/// treated as killing the entire store.  However, using ProjectionPath does
/// lead to more memory uses.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-dead-store-elim"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/MemoryLocations.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/LoadStoreOptUtils.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumDeadStores, "Number of dead stores removed");
STATISTIC(NumPartialDeadStores, "Number of partial dead stores removed");

/// If a large store is broken down to too many smaller stores, bail out.
/// Currently, we only do partial dead store if we can form a single contiguous
/// live store.
static llvm::cl::opt<unsigned>
MaxPartialStoreCount("max-partial-store-count", llvm::cl::init(1), llvm::cl::Hidden);

/// ComputeMaxStoreSet - If we ignore all reads, what is the max store set that
/// can reach a particular point in a basic block. This helps in generating
/// the genset and killset. i.e. if there is no upward visible store that can
/// reach the beginning of a basic block, then we know that the genset and
/// killset for the stored location need not be set for the basic block.
///
/// BuildGenKillSet - Build the genset and killset of the basic block.
///
/// PerformDSE - Perform the actual dead store elimination.
enum class DSEKind : unsigned {
  ComputeMaxStoreSet = 0,
  BuildGenKillSet = 1,
  PerformDSE = 2,
};

//===----------------------------------------------------------------------===//
//                             Utility Functions
//===----------------------------------------------------------------------===//

/// Return the deallocate stack instructions corresponding to the given
/// AllocStackInst.
static llvm::SmallVector<SILInstruction *, 1>
findDeallocStackInst(AllocStackInst *ASI) {
  llvm::SmallVector<SILInstruction *, 1> DSIs;
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI) {
    if (auto *D = dyn_cast<DeallocStackInst>(UI->getUser())) {
      DSIs.push_back(D);
    }   
  }
  return DSIs;
}

/// Return the deallocate ref instructions corresponding to the given
/// AllocRefInst.
static llvm::SmallVector<SILInstruction *, 1>
findDeallocStackRefInst(AllocRefInstBase *ARI) {
  llvm::SmallVector<SILInstruction *, 1> DSIs;
  for (auto UI = ARI->use_begin(), E = ARI->use_end(); UI != E; ++UI) {
    if (auto *D = dyn_cast<DeallocStackRefInst>(UI->getUser()))
      DSIs.push_back(D);
  }
  return DSIs;
}

static inline bool isComputeMaxStoreSet(DSEKind Kind) {
  return Kind == DSEKind::ComputeMaxStoreSet;
}

static inline bool isBuildingGenKillSet(DSEKind Kind) {
  return Kind == DSEKind::BuildGenKillSet;
}

static inline bool isPerformingDSE(DSEKind Kind) {
  return Kind == DSEKind::PerformDSE;
}

/// Returns true if this is an instruction that may have side effects in a
/// general sense but are inert from a load store perspective.
static bool isDeadStoreInertInstruction(SILInstruction *Inst) {
  switch (Inst->getKind()) {
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::StrongRetain##Name##Inst:                           \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::DeallocStackInst:
  case SILInstructionKind::DeallocStackRefInst:
  case SILInstructionKind::DeallocRefInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::SetDeallocatingInst:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
//                       Basic Block Location State
//===----------------------------------------------------------------------===//

namespace {
/// If this function has too many basic blocks or too many locations, it may
/// take a long time to compute the genset and killset. The number of memory
/// behavior or alias query we need to do in worst case is roughly linear to
/// # of BBs x(times) # of locations.
///
/// we could run DSE on functions with 256 basic blocks and 256 locations,
/// which is a large function.  
constexpr unsigned MaxLSLocationBBMultiplicationNone = 256*256;

/// we could run optimistic DSE on functions with less than 64 basic blocks
/// and 64 locations which is a sizable function.
constexpr unsigned MaxLSLocationBBMultiplicationPessimistic = 64*64;

/// forward declaration.
class DSEContext;
/// BlockState summarizes how LSLocations are used in a basic block.
///
/// Initially the BBWriteSetOut is empty. Before a basic block is processed, it
/// is initialized to the intersection of BBWriteSetIns of all successors of the
/// basic block.
///
/// BBWriteSetMid is initialized to BBWriteSetOut of the current basic block
/// before instructions in the basic block is processed.
///
/// Initially BBWriteSetIn is set to true. After the basic block is processed,
/// if its BBWriteSetMid is different from BBWriteSetIn, BBWriteSetIn is
/// assigned the value of BBWriteSetMid and the data flow is rerun on the
/// current basic block's predecessors.
///
/// Instructions in each basic block are processed in post-order as follows:
///
/// 1. When a store instruction is encountered, the stored location is tracked.
///
/// 2. When a load instruction is encountered, remove the loaded location and
///    any location it may alias with from the BBWriteSetMid.
///
/// 3. When an instruction reads from memory in an unknown way, the BBWriteSet
///    bit is cleared if the instruction can read the corresponding LSLocation.
class BlockState {
public:
  /// The basic block this BlockState represents.
  SILBasicBlock *BB = nullptr;

  /// Keep the number of LSLocations in the LocationVault.
  unsigned LocationNum = 0;

  /// A bit vector for which the ith bit represents the ith LSLocation in
  /// LocationVault. If the bit is set, then the location currently has an
  /// upward visible store at the end of the basic block.
  SmallBitVector BBWriteSetOut;

  /// A bit vector for which the ith bit represents the ith LSLocation in
  /// LocationVault. If the bit is set, then the location currently has an
  /// upward visible store in middle of the basic block.
  SmallBitVector BBWriteSetMid;

  /// A bit vector for which the ith bit represents the ith LSLocation in
  /// LocationVault. If a bit in the vector is set, then the location has an
  /// upward visible store at the beginning of the basic block.
  SmallBitVector BBWriteSetIn;

  /// A bit vector for which the ith bit represents the ith LSLocation in
  /// LocationVault. If the bit is set, then the current basic block
  /// generates an upward visible store.
  SmallBitVector BBGenSet;

  /// A bit vector for which the ith bit represents the ith LSLocation in
  /// LocationVault. If the bit is set, then the current basic block
  /// kills an upward visible store.
  SmallBitVector BBKillSet;

  /// A bit vector to keep the maximum number of stores that can reach a 
  /// certain point of the basic block. If a bit is set, that means there is
  /// potentially an upward visible store to the location at the particular
  /// point of the basic block.
  SmallBitVector BBMaxStoreSet;

  /// If a bit in the vector is set, then the location is dead at the end of
  /// this basic block. 
  SmallBitVector BBDeallocateLocation;

  /// The dead stores in the current basic block.
  llvm::SmallVector<SILInstruction *, 2> DeadStores;

  /// Keeps track of what stores to generate after the data flow stabilizes.
  /// these stores come from partial dead stores.
  ///
  /// The first SILValue keeps the address of the live store and the second
  /// SILValue keeps the value of the store.
  llvm::SetVector<SILValue> LiveAddr;
  llvm::DenseMap<SILValue, SILValue> LiveStores;

  /// Constructors.
  BlockState() = default;

  void dump();

  /// Initialize the bitvectors for the current basic block.
  void init(SILBasicBlock *BB, unsigned LocationNum, bool Optimistic);

  /// Check whether the BBWriteSetIn has changed. If it does, we need to rerun
  /// the data flow on this block's predecessors to reach fixed point.
  bool updateBBWriteSetIn(SmallBitVector &X);

  /// Functions to manipulate the write set.
  void startTrackingLocation(SmallBitVector &BV, unsigned bit);
  void stopTrackingLocation(SmallBitVector &BV, unsigned bit);
  bool isTrackingLocation(SmallBitVector &BV, unsigned bit);

  /// Set the store bit for stack slot deallocated in this basic block. 
  void initStoreSetAtEndOfBlock(DSEContext &Ctx);
};

} // end anonymous namespace

bool BlockState::updateBBWriteSetIn(SmallBitVector &X) {
  if (BBWriteSetIn == X)
    return false;
  BBWriteSetIn = X;
  return true;
}

void BlockState::startTrackingLocation(SmallBitVector &BV, unsigned i) {
  BV.set(i);
}

void BlockState::stopTrackingLocation(SmallBitVector &BV, unsigned i) {
  BV.reset(i);
}

bool BlockState::isTrackingLocation(SmallBitVector &BV, unsigned i) {
  return BV.test(i);
}

//===----------------------------------------------------------------------===//
//                          Top Level Implementation
//===----------------------------------------------------------------------===//

namespace {
/// The dead store elimination context, keep information about stores in a basic
/// block granularity.
class DSEContext {
/// How to process the current function.
enum class ProcessKind {
  ProcessOptimistic = 0,
  ProcessPessimistic = 1,
  ProcessNone = 2,
}; 

private:
  /// The module we are currently processing.
  SILModule *Mod;

  /// The function we are currently processing.
  SILFunction *F;

  /// Pass manager, used to get various analysis.
  SILPassManager *PM;

  /// Alias Analysis.
  AliasAnalysis *AA;

  /// Type Expansion Analysis.
  TypeExpansionAnalysis *TE;

  /// Epilogue release analysis.
  EpilogueARCFunctionInfo *EAFI;

  /// Map every basic block to its location state.
  BasicBlockData<BlockState> BBToLocState;

  /// Keeps all the locations for the current function. The BitVector in each
  /// BlockState is then laid on top of it to keep track of which LSLocation
  /// has an upward visible store.
  std::vector<LSLocation> LocationVault;

  /// Keeps a list of basic blocks that have StoreInsts. If a basic block does
  /// not have StoreInst, we do not actually perform the last iteration where
  /// DSE is actually performed on the basic block.
  ///
  /// NOTE: This is never populated for functions which will only require 1
  /// data flow iteration. For function that requires more than 1 iteration of
  /// the data flow this is populated when the first time the functions is
  /// walked, i.e. when the we generate the genset and killset.
  BasicBlockSet BBWithStores;

  /// Contains a map between location to their index in the LocationVault.
  /// used to facilitate fast location to index lookup.
  LSLocationIndexMap LocToBitIndex;

  /// Keeps a map between the accessed SILValue and the location.
  LSLocationBaseMap BaseToLocIndex;

  /// Return the BlockState for the basic block this basic block belongs to.
  BlockState *getBlockState(SILBasicBlock *B) { return &BBToLocState[B]; }

  /// Return the BlockState for the basic block this instruction belongs to.
  BlockState *getBlockState(SILInstruction *I) {
    return getBlockState(I->getParent());
  }

  /// LSLocation written has been extracted, expanded and mapped to the bit
  /// position in the bitvector. update the max store set using the bit
  /// position.
  void processWriteForMaxStoreSet(BlockState *S, unsigned bit);

  /// There is a read to a location, expand the location into individual fields
  /// before processing them.
  void processRead(SILInstruction *Inst, SILValue M, DSEKind K);
  void processReadForGenKillSet(BlockState *S, unsigned bit);
  void processReadForDSE(BlockState *S, unsigned Bit);

  /// There is a write to a location, expand the location into individual fields
  /// before processing them.
  void processWrite(SILInstruction *Inst, SILValue V, SILValue M, DSEKind K);
  void processWriteForGenKillSet(BlockState *S, unsigned bit);
  bool processWriteForDSE(BlockState *S, unsigned bit);

  /// Process instructions. Extract locations from SIL LoadInst.
  void processLoadInst(SILInstruction *Inst, DSEKind Kind);

  /// Process instructions. Extract locations from SIL StoreInst.
  void processStoreInst(SILInstruction *Inst, DSEKind Kind);

  /// Process instructions. Extract locations from SIL DebugValueInst.
  /// DebugValueInst w/ address value maybe promoted to DebugValueInst w/
  /// scalar value when this is done,
  /// DebugValueInst is effectively a read on the location.
  void processDebugValueAddrInst(SILInstruction *I, DSEKind Kind);
  void processDebugValueAddrInstForGenKillSet(SILInstruction *I);
  void processDebugValueAddrInstForDSE(SILInstruction *I);

  /// Process unknown read instructions. Extract locations from unknown memory
  /// inst.
  void processUnknownReadInst(SILInstruction *Inst, DSEKind Kind);
  void processUnknownReadInstForGenKillSet(SILInstruction *Inst);
  void processUnknownReadInstForDSE(SILInstruction *Inst);

  /// Check whether the instruction invalidate any locations due to change in
  /// its location Base.
  ///
  /// This is to handle a case like this.
  ///
  ///  class Foo { var a : Int = 12 }
  ///  for _ in 0 ...x {
  ///     x = Foo();
  ///     x.a = 13
  ///  }
  ///  x.a = 12
  ///
  /// In this case, DSE cannot remove the x.a = 13 inside the loop.
  ///
  /// To do this, when the algorithm reaches the beginning of the basic block in
  /// the loop it will need to invalidate the location in the BBWriteSetMid.
  /// i.e. the base of the location is changed.
  ///
  /// If not, on the second iteration, the intersection of the successors of
  /// the loop basic block will have store to x.a and therefore x.a = 13 can now
  /// be considered dead.
  void invalidateBase(SILValue B, BlockState *S, DSEKind Kind);
  void invalidateBaseForGenKillSet(SILValue B, BlockState *S);
  void invalidateBaseForDSE(SILValue B, BlockState *S);

  /// Get the bit representing the location in the LocationVault.
  unsigned getLocationBit(const LSLocation &L);

public:
  /// Constructor.
  DSEContext(SILFunction *F, SILModule *M, SILPassManager *PM,
             AliasAnalysis *AA, TypeExpansionAnalysis *TE,
             EpilogueARCFunctionInfo *EAFI,
             llvm::SpecificBumpPtrAllocator<BlockState> &BPA) 
    : Mod(M), F(F), PM(PM), AA(AA), TE(TE), EAFI(EAFI), BBToLocState(F),
      BBWithStores(F) {}

  void dump();

  /// Entry point for dead store elimination.
  bool run();

  /// Run the iterative DF to converge the BBWriteSetIn.
  void runIterativeDSE();

  /// Returns the location vault of the current function.
  std::vector<LSLocation> &getLocationVault() { return LocationVault; }

  /// Use a set of ad hoc rules to tell whether we should run a pessimistic
  /// one iteration data flow on the function.
  ProcessKind getProcessFunctionKind(unsigned StoreCount);

  /// Compute the kill set for the basic block. return true if the store set
  /// changes.
  void processBasicBlockForDSE(SILBasicBlock *BB, bool Optimistic);

  /// Compute the genset and killset for the current basic block.
  void processBasicBlockForGenKillSet(SILBasicBlock *BB);

  /// Compute the BBWriteSetOut and BBWriteSetIn for the current basic
  /// block with the generated gen and kill set.
  bool processBasicBlockWithGenKillSet(SILBasicBlock *BB);

  /// Intersect the successors' BBWriteSetIns.
  void mergeSuccessorLiveIns(SILBasicBlock *BB);

  /// Update the BlockState based on the given instruction.
  void processInstruction(SILInstruction *I, DSEKind Kind);
};

} // end anonymous namespace

void BlockState::dump() {
  llvm::dbgs() << "  block " << BB->getDebugID() << ": in=" << BBWriteSetIn
               << ", out=" << BBWriteSetOut << ", mid=" << BBWriteSetMid
               << ", gen=" << BBGenSet << ", kill=" << BBKillSet << '\n';
}

void BlockState::init(SILBasicBlock *BB, unsigned LocationNum, bool Optimistic) {
  this->BB = BB;
  this->LocationNum = LocationNum;

  // For function that requires just 1 iteration of the data flow to converge
  // we set the initial state of BBWriteSetIn to 0.
  //
  // For other functions, the initial state of BBWriteSetIn should be all 1's.
  // Otherwise the dataflow solution could be too conservative.
  //
  // Consider this case, the dead store by var a = 10 before the loop will not
  // be eliminated if the BBWriteSetIn is set to 0 initially.
  //
  //   var a = 10
  //   for _ in 0...1024 {}
  //   a = 10
  //
  // However, by doing so, we can only eliminate the dead stores after the
  // data flow stabilizes.
  //
  BBWriteSetIn.resize(LocationNum, Optimistic);
  BBWriteSetOut.resize(LocationNum, false);
  BBWriteSetMid.resize(LocationNum, false);

  // GenSet and KillSet initially empty.
  BBGenSet.resize(LocationNum, false);
  BBKillSet.resize(LocationNum, false);

  // MaxStoreSet is optimistically set to true initially.
  BBMaxStoreSet.resize(LocationNum, true);

  // DeallocateLocation initially empty.
  BBDeallocateLocation.resize(LocationNum, false);
}

#if __has_attribute(used)
__attribute((used))
#endif
void DSEContext::dump() {
  llvm::dbgs() << "Locations:\n";
  unsigned idx = 0;
  for (const LSLocation &loc : LocationVault) {
    llvm::dbgs() << "  #" << idx << ": " << loc.getBase();
    ++idx;
  }
  for (SILBasicBlock &BB : *F) {
    getBlockState(&BB)->dump();
  }
}

unsigned DSEContext::getLocationBit(const LSLocation &Loc) {
  // Return the bit position of the given Loc in the LocationVault. The bit
  // position is then used to set/reset the bitvector kept by each BlockState.
  //
  // We should have the location populated by the enumerateLSLocation at this
  // point.
  auto Iter = LocToBitIndex.find(Loc);
  assert(Iter != LocToBitIndex.end() && "LSLocation should have been enum'ed");
  return Iter->second;
}

DSEContext::ProcessKind DSEContext::getProcessFunctionKind(unsigned StoreCount) {
  // Don't optimize function that are marked as 'no.optimize'.
  if (!F->shouldOptimize())
    return ProcessKind::ProcessNone;

  // Really no point optimizing here as there is no dead stores.
  if (StoreCount < 1)
    return ProcessKind::ProcessNone;

  bool RunOneIteration = true;
  unsigned BBCount = 0;
  unsigned LocationCount = LocationVault.size();

  // If all basic blocks will have their successors processed if
  // the basic blocks in the functions are iterated in post order.
  // Then this function can be processed in one iteration, i.e. no
  // need to generate the genset and killset.
  auto *PO = PM->getAnalysis<PostOrderAnalysis>()->get(F);
  BasicBlockSet HandledBBs(F);
  for (SILBasicBlock *B : PO->getPostOrder()) {
    ++BBCount;
    for (SILBasicBlock *succ : B->getSuccessors()) {
      if (!HandledBBs.contains(succ)) {
        RunOneIteration = false;
        break;
      }
    }
    HandledBBs.insert(B);
  }

  // Data flow may take too long to run.
  if (BBCount * LocationCount > MaxLSLocationBBMultiplicationNone)
    return ProcessKind::ProcessNone;

  // This function's data flow would converge in 1 iteration.
  if (RunOneIteration)
    return ProcessKind::ProcessPessimistic;
  
  // We run one pessimistic data flow to do dead store elimination on
  // the function.
  if (BBCount * LocationCount > MaxLSLocationBBMultiplicationPessimistic)
    return ProcessKind::ProcessPessimistic;

  return ProcessKind::ProcessOptimistic;
}

void DSEContext::processBasicBlockForGenKillSet(SILBasicBlock *BB) {
  // Compute the MaxStoreSet at the end of the basic block.
  auto *BBState = getBlockState(BB);
  if (BB->succ_empty()) {
    BBState->BBMaxStoreSet |= BBState->BBDeallocateLocation;
  } else {
    auto Iter = BB->succ_begin();
    BBState->BBMaxStoreSet = getBlockState(*Iter)->BBMaxStoreSet;
    Iter = std::next(Iter);
    for (auto EndIter = BB->succ_end(); Iter != EndIter; ++Iter) {
      BBState->BBMaxStoreSet &= getBlockState(*Iter)->BBMaxStoreSet;
    }
  }

  // Compute the genset and killset. 
  //
  // Also compute the MaxStoreSet at the current position of the basic block.
  //
  // This helps generating the genset and killset. If there is no way a
  // location can have an upward visible store at a particular point in the
  // basic block, we do not need to turn on the genset and killset for the
  // location.
  //
  // Turning on the genset and killset can be costly as it involves querying
  // the AA interface.
  for (auto I = BB->rbegin(), E = BB->rend(); I != E; ++I) {
    // Only process store insts.
    if (isa<StoreInst>(*I)) {
      if (!BBWithStores.contains(BB))
        BBWithStores.insert(BB);
      processStoreInst(&(*I), DSEKind::ComputeMaxStoreSet);
    }

    // Compute the genset and killset for this instruction.
    processInstruction(&(*I), DSEKind::BuildGenKillSet);
  }

  // Handle SILArgument for base invalidation.
  ArrayRef<SILArgument *> Args = BB->getArguments();
  for (auto &X : Args) {
    invalidateBase(X, BBState, DSEKind::BuildGenKillSet);
  }
}

bool DSEContext::processBasicBlockWithGenKillSet(SILBasicBlock *BB) {
  // Compute the BBWriteSetOut at the end of the basic block.
  mergeSuccessorLiveIns(BB);

  // Compute the BBWriteSet at the beginning of the basic block.
  BlockState *S = getBlockState(BB);
  S->BBWriteSetMid = S->BBWriteSetOut;
  S->BBWriteSetMid.reset(S->BBKillSet);
  S->BBWriteSetMid |= S->BBGenSet;
 
  // If BBWriteSetIn changes, then keep iterating until reached a fixed point.
  return S->updateBBWriteSetIn(S->BBWriteSetMid);
}

void DSEContext::processBasicBlockForDSE(SILBasicBlock *BB, bool Optimistic) {
  // If we know this is not a one iteration function which means its
  // its BBWriteSetIn and BBWriteSetOut have been computed and converged, 
  // and this basic block does not even have StoreInsts, there is no point
  // in processing every instruction in the basic block again as no store
  // will be eliminated. 
  if (Optimistic && !BBWithStores.contains(BB))
    return;

  // Intersect in the successor WriteSetIns. A store is dead if it is not read
  // from any path to the end of the program. Thus an intersection.
  mergeSuccessorLiveIns(BB);

  // Initialize the BBWriteSetMid to BBWriteSetOut to get started.
  BlockState *S = getBlockState(BB);
  S->BBWriteSetMid = S->BBWriteSetOut;

  // Process instructions in post-order fashion.
  for (auto I = BB->rbegin(), E = BB->rend(); I != E; ++I) {
    processInstruction(&(*I), DSEKind::PerformDSE);
  }

  // Handle SILArgument for base invalidation.
  ArrayRef<SILArgument *> Args = BB->getArguments();
  for (auto &X : Args) {
    invalidateBase(X, S, DSEKind::BuildGenKillSet);
  }

  S->BBWriteSetIn = S->BBWriteSetMid;
}

void BlockState::initStoreSetAtEndOfBlock(DSEContext &Ctx) {
  std::vector<LSLocation> &LocationVault = Ctx.getLocationVault();
  // We set the store bit at the end of the basic block in which a stack
  // allocated location is deallocated.
  for (unsigned i = 0; i < LocationVault.size(); ++i) {
    // Turn on the store bit at the block which the stack slot is deallocated.
    if (auto *ASI = dyn_cast<AllocStackInst>(LocationVault[i].getBase())) {
      for (auto X : findDeallocStackInst(ASI)) {
        SILBasicBlock *DSIBB = X->getParent();
        if (DSIBB != BB)
          continue;
        startTrackingLocation(BBDeallocateLocation, i);
      }
    }
    if (auto *ARI = dyn_cast<AllocRefInstBase>(LocationVault[i].getBase())) {
      if (!ARI->isAllocatingStack())
        continue;
      for (auto X : findDeallocStackRefInst(ARI)) {
        SILBasicBlock *DSIBB = X->getParent();
        if (DSIBB != BB)
          continue;
        startTrackingLocation(BBDeallocateLocation, i);
      }
    }
  }
}

void DSEContext::mergeSuccessorLiveIns(SILBasicBlock *BB) {
  // If basic block has no successor, then all local writes can be considered
  // dead for block with no successor.
  BlockState *C = getBlockState(BB);
  if (BB->succ_empty()) {
    if (isa<UnreachableInst>(BB->getTerminator())) {
      C->BBWriteSetOut.set();
      return;
    }
    C->BBWriteSetOut |= C->BBDeallocateLocation;
    return;
  }

  // Use the first successor as the base condition.
  auto Iter = BB->succ_begin();
  C->BBWriteSetOut = getBlockState(*Iter)->BBWriteSetIn;

  /// Merge/intersection is very frequently performed, so it is important to
  /// make it as cheap as possible.
  ///
  /// To do so, we canonicalize LSLocations, i.e. traced back to the underlying
  /// object. Therefore, no need to do a O(N^2) comparison to figure out what is
  /// dead along all successors.
  ///
  /// NOTE: Canonicalization does not solve the problem entirely. i.e. it is
  /// still possible that 2 LSLocations with different bases that happen to be
  /// the same object and field. In such case, we would miss a dead store
  /// opportunity. But this happens less often with canonicalization.
  Iter = std::next(Iter);
  for (auto EndIter = BB->succ_end(); Iter != EndIter; ++Iter) {
    C->BBWriteSetOut &= getBlockState(*Iter)->BBWriteSetIn;
  }

  // We set the store bit at the end of the basic block in which a stack
  // allocated location is deallocated.
  C->BBWriteSetOut |= C->BBDeallocateLocation;
}

void DSEContext::invalidateBaseForGenKillSet(SILValue B, BlockState *S) {
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (LocationVault[i].getBase() != B)
      continue;
    S->startTrackingLocation(S->BBKillSet, i);
    S->stopTrackingLocation(S->BBGenSet, i);
  }
}

void DSEContext::invalidateBaseForDSE(SILValue B, BlockState *S) {
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->BBWriteSetMid.test(i))
      continue;
    if (LocationVault[i].getBase() != B)
      continue;
    S->stopTrackingLocation(S->BBWriteSetMid, i);
  }
}

void DSEContext::invalidateBase(SILValue B, BlockState *S, DSEKind Kind) {
  // If this instruction defines the base of a location, then we need to
  // invalidate any locations with the same base.
  //
  // Are we building genset and killset.
  if (isBuildingGenKillSet(Kind)) {
    invalidateBaseForGenKillSet(B, S);
    return;
  }

  // Are we performing dead store elimination.
  if (isPerformingDSE(Kind)) {
    invalidateBaseForDSE(B, S);
    return;
  }

  llvm_unreachable("Unknown DSE compute kind");
}

void DSEContext::processReadForDSE(BlockState *S, unsigned bit) {
  // Remove any may/must-aliasing stores to the LSLocation, as they can't be
  // used to kill any upward visible stores due to the interfering load.
  LSLocation &R = LocationVault[bit];
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->isTrackingLocation(S->BBWriteSetMid, i))
      continue;
    LSLocation &L = LocationVault[i];
    if (!L.isMayAliasLSLocation(R, AA))
      continue;
    S->stopTrackingLocation(S->BBWriteSetMid, i);
  }
}

void DSEContext::processReadForGenKillSet(BlockState *S, unsigned bit) {
  // Start tracking the read to this LSLocation in the killset and update
  // the genset accordingly.
  //
  // Even though, LSLocations are canonicalized, we still need to consult
  // alias analysis to determine whether 2 LSLocations are disjointed.
  LSLocation &R = LocationVault[bit];
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->BBMaxStoreSet.test(i))
      continue;
    // Do nothing if the read location NoAlias with the current location.
    LSLocation &L = LocationVault[i];
    if (!L.isMayAliasLSLocation(R, AA))
      continue;
    // Update the genset and kill set.
    S->startTrackingLocation(S->BBKillSet, i);
    S->stopTrackingLocation(S->BBGenSet, i);
  }
}

void DSEContext::processRead(SILInstruction *I, SILValue Mem, DSEKind Kind) {
  auto *S = getBlockState(I);
  // Construct a LSLocation to represent the memory read by this instruction.
  // NOTE: The base will point to the actual object this inst is accessing,
  // not this particular field.
  //
  // e.g. %1 = alloc_stack $S
  //      %2 = struct_element_addr %1, #a
  //      %3 = load %2 : $*Int
  //
  // Base will point to %1, but not %2. Projection path will indicate which
  // field is accessed.
  //
  // This will make comparison between locations easier. This eases the
  // implementation of intersection operator in the data flow.
  LSLocation L;
  if (BaseToLocIndex.find(Mem) != BaseToLocIndex.end()) {
    L = BaseToLocIndex[Mem];
  } else {
    SILValue UO = getUnderlyingObject(Mem);
    L = LSLocation(UO, ProjectionPath::getProjectionPath(UO, Mem));
  }

  // If we can't figure out the Base or Projection Path for the read instruction,
  // process it as an unknown memory instruction for now.
  if (!L.isValid()) {
    processUnknownReadInst(I, Kind);
    return;
  }

  // Expand the given Mem into individual fields and process them as separate
  // reads.
  LSLocationList Locs;
  LSLocation::expand(L, &I->getModule(),
                     TypeExpansionContext(*I->getFunction()), Locs, TE);

  // Are we building the genset and killset.
  if (isBuildingGenKillSet(Kind)) {
    for (auto &E : Locs) {
      // Only building the gen and kill sets for now.
      processReadForGenKillSet(S, getLocationBit(E));
    }
    return;
  }

  // Are we performing the actual DSE.
  if (isPerformingDSE(Kind)) {
    for (auto &E : Locs) {
      // This is the last iteration, compute BBWriteSetOut and perform DSE. 
      processReadForDSE(S, getLocationBit(E));
    }
    return;
  }

  llvm_unreachable("Unknown DSE compute kind");
}

bool DSEContext::processWriteForDSE(BlockState *S, unsigned bit) {
  // If a tracked store must aliases with this store, then this store is dead.
  bool StoreDead = false;
  LSLocation &R = LocationVault[bit];
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->isTrackingLocation(S->BBWriteSetMid, i))
      continue;
    // If 2 locations may alias, we can still keep both stores.
    LSLocation &L = LocationVault[i];
    if (!L.isMustAliasLSLocation(R, AA))
      continue;
    // There is a must alias store. No need to check further.
    StoreDead = true;
    break;
  }

  // Track this new store.
  S->startTrackingLocation(S->BBWriteSetMid, bit);
  return StoreDead;
}

void DSEContext::processWriteForGenKillSet(BlockState *S, unsigned bit) {
  S->startTrackingLocation(S->BBGenSet, bit);
}

void DSEContext::processWriteForMaxStoreSet(BlockState *S, unsigned bit) {
  S->startTrackingLocation(S->BBMaxStoreSet, bit);
}

void DSEContext::processWrite(SILInstruction *I, SILValue Val, SILValue Mem,
                              DSEKind Kind) {
  auto *S = getBlockState(I);
  // Construct a LSLocation to represent the memory read by this instruction.
  // NOTE: The base will point to the actual object this inst is accessing,
  // not this particular field.
  //
  // e.g. %1 = alloc_stack $S
  //      %2 = struct_element_addr %1, #a
  //      store %3 to %2 : $*Int
  //
  // Base will point to %1, but not %2. Projection path will indicate which
  // field is accessed.
  //
  // This will make comparison between locations easier. This eases the
  // implementation of intersection operator in the data flow.
  LSLocation L;
  if (BaseToLocIndex.find(Mem) != BaseToLocIndex.end()) {
    L = BaseToLocIndex[Mem];
  } else {
    SILValue UO = getUnderlyingObject(Mem);
    L = LSLocation(UO, ProjectionPath::getProjectionPath(UO, Mem));
  }

  // If we can't figure out the Base or Projection Path for the store
  // instruction, simply ignore it.
  if (!L.isValid())
    return;

  // Expand the given Mem into individual fields and process them as separate
  // writes.
  bool Dead = true;
  LSLocationList Locs;
  LSLocation::expand(L, Mod, TypeExpansionContext(*I->getFunction()), Locs, TE);
  SmallBitVector V(Locs.size());

  // Are we computing max store set.
  if (isComputeMaxStoreSet(Kind)) {
    for (auto &E : Locs) {
      // Update the max store set for the basic block.
      processWriteForMaxStoreSet(S, getLocationBit(E));
    }
    return;
  }

  // Are we computing genset and killset.
  if (isBuildingGenKillSet(Kind)) {
    for (auto &E : Locs) {
      // Only building the gen and kill sets here.
      processWriteForGenKillSet(S, getLocationBit(E));
    }
    // Data flow has not stabilized, do not perform the DSE just yet.
    return;
  }

  // We are doing the actual DSE.
  assert(isPerformingDSE(Kind) && "Invalid computation kind");
  unsigned idx = 0;
  for (auto &E : Locs) {
    // This is the last iteration, compute BBWriteSetOut and perform the dead
    // store elimination.
    if (processWriteForDSE(S, getLocationBit(E)))
      V.set(idx);
    Dead &= V.test(idx);
    ++idx;
  }

  // Fully dead store - stores to all the components are dead, therefore this
  // instruction is dead.
  if (Dead) {
    LLVM_DEBUG(llvm::dbgs() << "Instruction Dead: " << *I << "\n");
    S->DeadStores.push_back(I);
    ++NumDeadStores;
    return;
  }

  // Partial dead store - stores to some locations are dead, but not all. This
  // is a partially dead store. Also at this point we know what locations are
  // dead.
  LSLocationList Alives;
  if (V.any()) {
    // Take out locations that are dead.
    for (unsigned i = 0; i < V.size(); ++i) {
      if (V.test(i))
        continue;
      // This location is alive.
      Alives.push_back(Locs[i]);
    }

    // Try to create as few aggregated stores as possible out of the locations.
    LSLocation::reduce(L, Mod, TypeExpansionContext(*I->getFunction()), Alives);

    // Oops, we have too many smaller stores generated, bail out.
    if (Alives.size() > MaxPartialStoreCount)
      return;

    // At this point, we are performing a partial dead store elimination.
    //
    // Locations here have a projection path from their Base, but this
    // particular instruction may not be accessing the base, so we need to
    // *rebase* the locations w.r.t. to the current instruction.
    SILValue B = Locs[0].getBase();
    llvm::Optional<ProjectionPath> BP =
        ProjectionPath::getProjectionPath(B, Mem);
    // Strip off the projection path from base to the accessed field.
    for (auto &X : Alives) {
      X.removePathPrefix(BP);
    }

    // We merely setup the remaining live stores, but do not materialize in IR
    // yet, These stores will be materialized before the algorithm exits.
    for (auto &X : Alives) {
      SILValue Value = X.getPath()->createExtract(Val, I, true);
      SILValue Addr = X.getPath()->createExtract(Mem, I, false);
      S->LiveAddr.insert(Addr);
      S->LiveStores[Addr] = Value;
    }

    // Lastly, mark the old store as dead.
    LLVM_DEBUG(llvm::dbgs() << "Instruction Partially Dead: " << *I << "\n");
    S->DeadStores.push_back(I);
    ++NumPartialDeadStores;
  }
}

void DSEContext::processLoadInst(SILInstruction *I, DSEKind Kind) {
  processRead(I, cast<LoadInst>(I)->getOperand(), Kind);
}

void DSEContext::processStoreInst(SILInstruction *I, DSEKind Kind) {
  auto *SI = cast<StoreInst>(I);
  processWrite(I, SI->getSrc(), SI->getDest(), Kind);
}

void DSEContext::processDebugValueAddrInstForGenKillSet(SILInstruction *I) {
  BlockState *S = getBlockState(I);
  SILValue Mem = cast<DebugValueInst>(I)->getOperand();
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->BBMaxStoreSet.test(i))
      continue;
    if (AA->isNoAlias(Mem, LocationVault[i].getBase()))
      continue;
    S->stopTrackingLocation(S->BBGenSet, i);
    S->startTrackingLocation(S->BBKillSet, i);
  }
}

void DSEContext::processDebugValueAddrInstForDSE(SILInstruction *I) {
  BlockState *S = getBlockState(I);
  SILValue Mem = cast<DebugValueInst>(I)->getOperand();
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->isTrackingLocation(S->BBWriteSetMid, i))
      continue;
    if (AA->isNoAlias(Mem, LocationVault[i].getBase()))
      continue;
    S->stopTrackingLocation(S->BBWriteSetMid, i);
  }
}

void DSEContext::processDebugValueAddrInst(SILInstruction *I, DSEKind Kind) {
  // Are we building genset and killset.
  if (isBuildingGenKillSet(Kind)) {
    processDebugValueAddrInstForGenKillSet(I);
    return;
  }

  // Are we performing dead store elimination.
  if (isPerformingDSE(Kind)) {
    processDebugValueAddrInstForDSE(I);
    return;
  }

  llvm_unreachable("Unknown DSE compute kind");
}

void DSEContext::processUnknownReadInstForGenKillSet(SILInstruction *I) {
  BlockState *S = getBlockState(I);
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->BBMaxStoreSet.test(i))
      continue;
    if (!AA->mayReadFromMemory(I, LocationVault[i].getBase()))
      continue;
    // Update the genset and kill set.
    S->startTrackingLocation(S->BBKillSet, i);
    S->stopTrackingLocation(S->BBGenSet, i);
  }
}

void DSEContext::processUnknownReadInstForDSE(SILInstruction *I) {
  BlockState *S = getBlockState(I);
  for (unsigned i = 0; i < S->LocationNum; ++i) {
    if (!S->isTrackingLocation(S->BBWriteSetMid, i))
      continue;
    if (!AA->mayReadFromMemory(I, LocationVault[i].getBase()))
      continue;
    S->stopTrackingLocation(S->BBWriteSetMid, i);
  }
}

void DSEContext::processUnknownReadInst(SILInstruction *I, DSEKind Kind) {
  // If this is a release on a guaranteed parameter, it can not call deinit,
  // which might read or write memory.
  if (isIntermediateRelease(I, EAFI))
    return;

  // Are we building genset and killset.
  if (isBuildingGenKillSet(Kind)) {
    processUnknownReadInstForGenKillSet(I);
    return;
  }

  // Are we performing dead store elimination.
  if (isPerformingDSE(Kind)) {
    processUnknownReadInstForDSE(I);
    return;
  }

  llvm_unreachable("Unknown DSE compute kind");
}

void DSEContext::processInstruction(SILInstruction *I, DSEKind Kind) {
  // If this instruction has side effects, but is inert from a store
  // perspective, skip it.
  if (isDeadStoreInertInstruction(I))
    return;

  // A set of ad-hoc rules to process instructions.
  if (isa<LoadInst>(I)) {
    processLoadInst(I, Kind);
  } else if (isa<StoreInst>(I)) {
    processStoreInst(I, Kind);
  } else if (DebugValueInst::hasAddrVal(I)) {
    processDebugValueAddrInst(I, Kind);
  } else if (I->mayReadFromMemory()) {
    processUnknownReadInst(I, Kind);
  }

  // Check whether this instruction will invalidate any other locations.
  for (auto result : I->getResults())
    invalidateBase(result, getBlockState(I), Kind);
}

void DSEContext::runIterativeDSE() {
  // Generate the genset and killset for each basic block. We can process the
  // basic blocks in any order.
  // 
  // We also compute the max store set at the beginning of the basic block.
  //
  auto *PO = PM->getAnalysis<PostOrderAnalysis>()->get(F);
  for (SILBasicBlock *B : PO->getPostOrder()) {
    processBasicBlockForGenKillSet(B);
  }

  // Process each basic block with the gen and kill set. Every time the
  // BBWriteSetIn of a basic block changes, the optimization is rerun on its
  // predecessors.
  BasicBlockWorklist WorkList(F);
  // Push into reverse post order so that we can pop from the back and get
  // post order.
  for (SILBasicBlock *B : PO->getReversePostOrder()) {
    WorkList.push(B);
  }
  while (SILBasicBlock *BB = WorkList.popAndForget()) {
    if (processBasicBlockWithGenKillSet(BB)) {
      for (SILBasicBlock *pred : BB->getPredecessorBlocks()) {
        WorkList.pushIfNotVisited(pred);
      }
    }
  }
}

bool DSEContext::run() {
  int numLoads = 0, numStores = 0;
  bool immutableLoadsFound = false;
  // Walk over the function and find all the locations accessed by
  // this function.
  LSLocation::enumerateLSLocations(*F, LocationVault, LocToBitIndex,
                                   BaseToLocIndex, TE,
                                   /*stopAtImmutable*/ false,
                                   numLoads, numStores, immutableLoadsFound);

  // Check how to optimize this function.
  ProcessKind Kind = getProcessFunctionKind(numStores);
  
  // We do not optimize this function at all.
  if (Kind == ProcessKind::ProcessNone)
    return false;

  // Do we run a pessimistic data flow ?
  const bool Optimistic = (Kind == ProcessKind::ProcessOptimistic);

  // For all basic blocks in the function, initialize a BB state.
  //
  // Initialize the BBToLocState mapping.
  unsigned LocationNum = this->getLocationVault().size();

  if (LocationNum > 500) {
    // Emergency exit to avoid bad compile time problems in rare corner cases.
    // This limit is more than enough for "real world" code.
    // Even large functions have < 100 locations.
    // But in some corner cases - especially in generated code - we can run
    // into quadratic complexity for large functions.
    // TODO: implement DSE with a better (non-quadratic) algorithm
    return false;
  }

  for (auto bs : BBToLocState) {
    bs.data.init(&bs.block, LocationNum, Optimistic);
    bs.data.initStoreSetAtEndOfBlock(*this);
  }

  // We perform dead store elimination in the following phases.
  //
  // Phase 1. we compute the max store set at the beginning of the basic block.
  //
  // Phase 2. we compute the genset and killset for every basic block.
  //
  // Phase 3. we run the data flow with the genset and killset until
  // BBWriteSetIns stop changing.
  //
  // Phase 4. we run the data flow for the last iteration and perform the DSE.
  //
  // Phase 5. we remove the dead stores.
  //
  // Phase 1 - 3 are only performed when we know the data flow will not
  // converge in a single iteration. Otherwise, we only run phase 4 and 5
  // on the function.

  // We need to run the iterative data flow on the function.
  if (Optimistic) {
    runIterativeDSE();
  }

  // The data flow has stabilized, run one last iteration over all the basic
  // blocks and try to remove dead stores.
  // Is this a one iteration function.
  auto *PO = PM->getAnalysis<PostOrderAnalysis>()->get(F);
  for (SILBasicBlock *B : PO->getPostOrder()) {
    processBasicBlockForDSE(B, Optimistic);
  }

  // Finally, delete the dead stores and create the live stores.
  bool Changed = false;
  for (auto bs : BBToLocState) {
    // Create the stores that are alive due to partial dead stores.
    for (auto &X : bs.data.LiveAddr) {
      Changed = true;
      auto I = bs.data.LiveStores.find(X);
      SILInstruction *Inst = I->first->getDefiningInstruction();
      auto *IT = &*std::next(Inst->getIterator());
      SILBuilderWithScope Builder(IT);
      Builder.createStore(Inst->getLoc(), I->second, I->first,
                          StoreOwnershipQualifier::Unqualified);
    }
    // Delete the dead stores.
    for (auto &I : getBlockState(&bs.block)->DeadStores) {
      Changed = true;
      LLVM_DEBUG(llvm::dbgs() << "*** Removing: " << *I << " ***\n");
      // This way, we get rid of pass dependence on DCE.
      recursivelyDeleteTriviallyDeadInstructions(I, true);
    }
  }

  return Changed;
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

namespace {

class DeadStoreElimination : public SILFunctionTransform {
public:
  /// The entry point to the transformation.
  void run() override {
    SILFunction *F = getFunction();
    LLVM_DEBUG(llvm::dbgs() << "*** DSE on function: " << F->getName()
                            << " ***\n");

    auto *AA = PM->getAnalysis<AliasAnalysis>(F);
    auto *TE = PM->getAnalysis<TypeExpansionAnalysis>();
    auto *EAFI = PM->getAnalysis<EpilogueARCAnalysis>()->get(F);

    // The allocator we are using.
    llvm::SpecificBumpPtrAllocator<BlockState> BPA;

    DSEContext DSE(F, &F->getModule(), PM, AA, TE, EAFI, BPA);
    if (DSE.run()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDeadStoreElimination() {
  return new DeadStoreElimination();
}

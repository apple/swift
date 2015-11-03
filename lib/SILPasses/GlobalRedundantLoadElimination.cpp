///===---- LoadStoreOpts.cpp - SIL Load/Store Optimizations Forwarding -----===//
///
/// This source file is part of the Swift.org open source project
///
/// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
/// Licensed under Apache License v2.0 with Runtime Library Exception
///
/// See http://swift.org/LICENSE.txt for license information
/// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
///
///===----------------------------------------------------------------------===//
///
/// This pass eliminates redundant loads, dead stores, and performs load
/// forwarding.
///
/// TODO: Plan to implement new redundant load elimination, a.k.a. load
/// forwarding.
///
/// A load can be eliminated if its value has already been held somewhere,
/// i.e. loaded by a previous load, memory location stored by a known
/// value.
///
/// In this case, one can replace the load instruction with the previous
/// results.
///
/// RedudantLoadElimination (RLE) eliminates such loads by:
///
/// 1. Introducing a notion of a MemLocation that is used to model objects
/// fields. (See below for more details).
///
/// 2. Introducing a notion of a LoadStoreValue that is used to model the value
/// that currently resides in the associated MemLocation on the particular
/// program path. (See below for more details).
///
/// 3. Performing a RPO walk over the control flow graph, tracking any
/// MemLocations that are read from or stored into in each basic block. The
/// read or stored value, kept in a map (gen-set) between MemLocation and
/// LoadStoreValue, becomes the avalable value for the MemLocation. 
///
/// 4. An optimistic iterative intersection-based dataflow is performed on the
/// gen sets until convergence.
///
/// At the core of RLE, there is the MemLocation class. a MemLocation is an
/// abstraction of an object field in program. It consists of a base and a 
/// projection path to the field accessed.
///
/// In SIL, one can access an aggregate as a whole, i.e. store to a struct with
/// 2 Int fields. A store like this will generate 2 *indivisible* MemLocations, 
/// 1 for each field and in addition to keeping a list of MemLocation, RLE also
/// keeps their available LoadStoreValues. We call it *indivisible* because it
/// can not be broken down to more MemLocations.
///
/// LoadStoreValues consists of a base - a SILValue from the load or store inst,
/// as well as a projection path to which the field it represents. So, a
/// store to an 2-field struct as mentioned above will generate 2 MemLocations
/// and 2 LoadStoreValues.
///
/// Every basic block keeps a map between MemLocation <-> LoadStoreValue. By
/// keeping the MemLocation and LoadStoreValue in their indivisible form, one can
/// easily find which part of the load is redundant and how to compute its
/// forwarding value.
///
/// Given the case which the 2 fields of the struct both have available values,
/// RLE can find their LoadStoreValues (maybe by struct_extract from a larger
/// value) and then aggregate them.
///
/// However, this may introduce a lot of extraction and aggregation which may
/// not be necessary. i.e. a store the the struct followed by a load from the
/// struct. To solve this problem, when RLE detects that an load instruction
/// can be replaced by forwarded value, it will try to find minimum # of
/// extraction necessary to form the forwarded value. It will group the
/// available value's by the LoadStoreValue base, i.e. the LoadStoreValues come
/// from the same instruction,  and then use extraction to obtain the needed
/// components of the base.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-redundant-load-elim"
#include "swift/SILPasses/Passes.h"
#include "swift/SIL/MemLocation.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILAnalysis/AliasAnalysis.h"
#include "swift/SILAnalysis/PostOrderAnalysis.h"
#include "swift/SILAnalysis/DominanceAnalysis.h"
#include "swift/SILAnalysis/ValueTracking.h"
#include "swift/SILPasses/Utils/SILSSAUpdater.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SILPasses/Utils/CFG.h"
#include "swift/SILPasses/Transforms.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

using namespace swift;

STATISTIC(NumForwardedLoads, "Number of loads forwarded");

//===----------------------------------------------------------------------===//
//                             Utility Functions
//===----------------------------------------------------------------------===//

/// Returns true if this is an instruction that may have side effects in a
/// general sense but are inert from a load store perspective.
static bool isRLEInertInstruction(SILInstruction *Inst) {
  switch (Inst->getKind()) {
  case ValueKind::StrongRetainInst:
  case ValueKind::StrongRetainUnownedInst:
  case ValueKind::UnownedRetainInst:
  case ValueKind::RetainValueInst:
  case ValueKind::DeallocStackInst:
  case ValueKind::CondFailInst:
  case ValueKind::IsUniqueInst:
  case ValueKind::IsUniqueOrPinnedInst:
    return true;
  default:
    return false;
  }
}

/// Returns true if the given basic block is reachable from the entry block.
///
/// TODO: this is very inefficient, can we make use of the domtree. 
static bool isReachable(SILBasicBlock *Block) {
  SmallPtrSet<SILBasicBlock *, 16> Visited;
  llvm::SmallVector<SILBasicBlock *, 16> Worklist;
  SILBasicBlock *EntryBB = Block->getParent()->begin();
  Worklist.push_back(EntryBB);
  Visited.insert(EntryBB);

  while (!Worklist.empty()) {
    auto *CurBB = Worklist.back();
    Worklist.pop_back();

    if (CurBB == Block)
      return true;

    for (auto &Succ : CurBB->getSuccessors())
      if (!Visited.insert(Succ).second)
        Worklist.push_back(Succ);
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                            RLEContext Interface
//===----------------------------------------------------------------------===//

namespace {

class BBState;
/// This class stores global state that we use when processing and also drives
/// the computation. We put its interface at the top for use in other parts of
/// the pass which may want to use this global information.
class RLEContext {
  /// Function this context is currently processing.
  SILFunction *F;

  /// The alias analysis that we will use during all computations.
  AliasAnalysis *AA;

  /// The range that we use to iterate over the reverse post order of the given
  /// function.
  PostOrderFunctionInfo::reverse_range ReversePostOrder;

  /// Caches a list of projection paths to leaf nodes in the given type.
  TypeExpansionMap TypeExpansionVault;

  /// Keeps all the locations for the current function. The BitVector in each
  /// BBState is then laid on top of it to keep track of which MemLocation
  /// has a downward available value.
  std::vector<MemLocation> MemLocationVault;

  /// Contains a map between MemLocation to their index in the MemLocationVault.
  llvm::DenseMap<MemLocation, unsigned> LocToBitIndex;

  /// A map from each BasicBlock to its BBState.
  llvm::DenseMap<SILBasicBlock *, BBState> BBToLocState;

public:
  RLEContext(SILFunction *F, AliasAnalysis *AA,
             PostOrderFunctionInfo::reverse_range RPOT);

  RLEContext(const RLEContext &) = delete;
  RLEContext(RLEContext &&) = default;
  ~RLEContext() = default;

  bool run();

  /// Returns the alias analysis interface for this RLEContext.
  AliasAnalysis *getAA() const { return AA; }

  /// Returns the type expansion cache for this RLEContext.
  TypeExpansionMap &getTypeExpansionVault() { return TypeExpansionVault; }

  /// Return the BBState for the basic block this basic block belongs to.
  BBState &getBBLocState(SILBasicBlock *B) { return BBToLocState[B]; }

  /// Get the bit representing the location in the MemLocationVault.
  unsigned getMemLocationBit(const MemLocation &L);

  /// Given the bit, get the memory location from the MemLocationVault.
  MemLocation &getMemLocation(const unsigned index);

  /// Go to the predecessors of the given basic block, compute the value
  /// for the given MemLocation.
  SILValue computePredecessorCoveringValue(SILBasicBlock *B, MemLocation &L); 

  /// Given a MemLocation, try to collect all the LoadStoreValues for this
  /// MemLocation in the given basic block. If a LoadStoreValue is a covering
  /// value, collectForwardingValues also create a SILArgument for it. As a
  /// a result, collectForwardingValues may invalidate TerminatorInsts for 
  /// basic blocks.
  /// 
  /// UseForwardValOut tells whether to use the ForwardValOut or not. i.e.
  /// when materialize a covering value, we go to each predecessors and
  /// collect forwarding values from their ForwardValOuts.
  bool collectForwardingValues(SILBasicBlock *B, MemLocation &L,
                               MemLocationValueMap &Values,
                               bool UseForwardValOut);

  /// Dump all the memory locations in the MemLocationVault.
  void printMemLocationVault() const {
    for (auto &X : MemLocationVault)
      X.print();
  }
};

} // end anonymous namespace


//===----------------------------------------------------------------------===//
//                               BBState
//===----------------------------------------------------------------------===//

namespace {

/// State of the load store in one basic block which allows for forwarding from
/// loads, stores -> loads
class BBState {
  /// The basic block that we are optimizing.
  SILBasicBlock *BB;

  /// A bit vector for which the ith bit represents the ith MemLocation in
  /// MemLocationVault.
  ///
  /// If the bit is set, then the location has an downward visible value
  /// at current instruction.
  ///
  /// ForwardSetIn is initialized to the intersection of ForwardSetOut of
  /// all predecessors.
  llvm::BitVector ForwardSetIn;

  /// A bit vector for which the ith bit represents the ith MemLocation in
  /// MemLocationVault.
  ///
  /// If the bit is set, then the location has an downward visible value at
  /// the end of this basic block.
  ///
  /// At the end of the basic block, if ForwardSetIn != ForwardSetOut then
  /// we rerun the data flow until convergence.
  ///
  /// TODO: we only need to reprocess this basic block's successsors.
  llvm::BitVector ForwardSetOut;

  /// This is a map between MemLocations and their LoadStoreValues.
  ///
  /// If there is an entry for a MemLocation, then the MemLocation has an
  /// available value at current instruction.
  ///
  /// TODO: can we create a LoadStoreValue vault so that we do not need to keep
  /// them per basic block. This would also give ForwardValIn more symmetry.
  /// i.e. MemLocation and LoadStoreValue both represented as bit vector indices.
  ValueTableMap ForwardValIn;

  /// This is map between MemLocations and their available values at the end of
  /// this basic block.
  ValueTableMap ForwardValOut;

  /// Keeps a list of replaceable instructions in the current basic block as
  /// well as their SILValue replacement.
  llvm::DenseMap<SILInstruction *, SILValue> RedundantLoads;

  /// Check whether the ForwardSetOut has changed. If it does, we need to
  /// rerun the data flow to reach fixed point.
  bool updateForwardSetOut() {
    bool Changed = (ForwardSetIn != ForwardSetOut);
    // Reached the end of this basic block, update the end-of-block
    // ForwardSetOut and ForwardValOut;
    ForwardSetOut = ForwardSetIn;
    ForwardValOut = ForwardValIn;
    return Changed;
  }

  /// Merge in the state of an individual predecessor.
  void mergePredecessorState(BBState &OtherState);

  /// MemLocation and LoadStoreValue read have been extracted, expanded and
  /// mapped to the bit position in the bitvector. process it using the bit
  /// position.
  bool updateForwardSetForRead(RLEContext &Ctx, unsigned Bit, LoadStoreValue Val);

  /// MemLocation and LoadStoreValue written have been extracted, expanded and
  /// mapped to the bit position in the bitvector. process it using the bit
  /// position.
  void updateForwardSetForWrite(RLEContext &Ctx, unsigned Bit, LoadStoreValue Val);

  /// There is a read to a MemLocation, expand the MemLocation into individual
  /// fields before processing them.
  void processRead(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                   SILValue Val, bool PF);

  /// There is a write to a MemLocation, expand the MemLocation into individual
  /// fields before processing them.
  void processWrite(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                    SILValue Val);

public:
  /// Constructor and initializer.
  BBState() = default;
  void init(SILBasicBlock *NewBB, unsigned bitcnt, bool reachable) {
    BB = NewBB;
    // The initial state of ForwardSetOut for reachable basic block should be
    // all 1's. Otherwise the dataflow solution could be too conservative.
    //
    // Consider this case, the forwardable value by var a = 10 before the loop
    // will not be forwarded if the ForwardSetOut is set to 0 initially.
    //
    //   var a = 10
    //   for _ in 0...1024 {}
    //   use(a);
    //
    // However, by doing so, we can only do the data forwarding after the
    // data flow stablizes.
    //
    ForwardSetIn.resize(bitcnt, false);
    ForwardSetOut.resize(bitcnt, reachable);
  }

  /// Returns the current basic block.
  SILBasicBlock *getBB() const { return BB; }

  /// Returns the ForwardValIn for the current basic block.
  ValueTableMap  &getForwardValIn() { return ForwardValIn; }

  /// Returns the ForwardValOut for the current basic block.
  ValueTableMap  &getForwardValOut() { return ForwardValOut; }

  /// Returns the list of redundant loads in the current basic block.
  llvm::DenseMap<SILInstruction *, SILValue> &getRL() {
    return RedundantLoads;
  }

  bool optimize(RLEContext &Ctx, bool PF);

  /// BitVector manipulation fucntions.
  void clearMemLocations();
  void startTrackingMemLocation(unsigned bit, LoadStoreValue Val);
  void stopTrackingMemLocation(unsigned bit);
  void updateTrackedMemLocation(unsigned bit, LoadStoreValue Val);
  bool isTrackingMemLocation(unsigned bit);

  /// Merge in the states of all predecessors.
  void mergePredecessorStates(RLEContext &Ctx);

  /// Process Instruction which writes to memory in an unknown way.
  void processUnknownWriteInst(RLEContext &Ctx, SILInstruction *I);

  /// Process LoadInst. Extract MemLocations and LoadStoreValue from LoadInst.
  void processLoadInst(RLEContext &Ctx, LoadInst *LI, bool PF);

  /// Process StoreInst. Extract MemLocations and LoadStoreValue from StoreInst.
  void processStoreInst(RLEContext &Ctx, StoreInst *SI);

  /// Returns a *single* forwardable SILValue for the given MemLocation right
  /// before the InsertPt instruction.
  SILValue computeForwardingValues(RLEContext &Ctx, MemLocation &L,
                                   SILInstruction *InsertPt,
                                   bool UseForwardValOut);

  /// Set up the value for redundant load elimination right before the
  /// InsertPt instruction.
  void setupRLE(RLEContext &Ctx, MemLocation &L, SILInstruction *InsertPt);
};

} // end anonymous namespace

bool BBState::isTrackingMemLocation(unsigned bit) {
  return ForwardSetIn.test(bit);
}

void BBState::stopTrackingMemLocation(unsigned bit) {
  ForwardSetIn.reset(bit);
  ForwardValIn.erase(bit);
}

void BBState::clearMemLocations() {
  ForwardSetIn.reset();
  ForwardValIn.clear();
}

void BBState::startTrackingMemLocation(unsigned bit, LoadStoreValue Val) {
  ForwardSetIn.set(bit);
  ForwardValIn[bit] = Val;
}

void BBState::updateTrackedMemLocation(unsigned bit, LoadStoreValue Val) {
  ForwardValIn[bit] = Val;
}

SILValue BBState::computeForwardingValues(RLEContext &Ctx, MemLocation &L,
                                          SILInstruction *InsertPt,
                                          bool UseForwardValOut) {
  SILBasicBlock *ParentBB = InsertPt->getParent();
  bool IsTerminator = (InsertPt == ParentBB->getTerminator());
  // We do not have a SILValue for the current MemLocation, try to construct
  // one.
  //
  // First, collect current available locations and their corresponding values
  // into a map.
  MemLocationValueMap Values;
  if (!Ctx.collectForwardingValues(ParentBB, L, Values, UseForwardValOut))
    return SILValue();

  // If the InsertPt is the terminator instruction of the basic block, we 
  // *refresh* it as terminator instruction could be deleted as a result
  // of adding new edge values to the terminator instruction.
  if (IsTerminator)
    InsertPt = ParentBB->getTerminator();

  // Second, reduce the available values into a single SILValue we can use to
  // forward.
  SILValue TheForwardingValue;
  TheForwardingValue = MemLocation::reduceWithValues(L, &ParentBB->getModule(),
                                                     Values, InsertPt);
  /// Return the forwarding value.
  return TheForwardingValue;
}

void BBState::setupRLE(RLEContext &Ctx, MemLocation &L, 
                       SILInstruction *InsertPt) {
  SILValue TheForwardingValue;
  TheForwardingValue = computeForwardingValues(Ctx, L, InsertPt, false);
  if (!TheForwardingValue)
    return;

  // Now we have the forwarding value, record it for forwarding!.
  //
  // NOTE: we do not perform the RLE right here because doing so could introduce
  // new memory locations.
  //
  // e.g.
  //    %0 = load %x
  //    %1 = load %x
  //    %2 = extract_struct %1, #a
  //    %3 = load %2 
  //
  // If we perform the RLE and replace %1 with %0, we end up having a memory
  // location we do not have before, i.e. Base == %0, and Path == #a.
  //
  // We may be able to add the memory location to the vault, but it gets
  // complicated very quickly, e.g. we need to resize the bit vectors size,
  // etc.
  //
  // However, since we already know the instruction to replace and the value to
  // replace it with, we can record it for now and forwarded it after all the
  // forwardable values are recorded in the function.
  //
  RedundantLoads[InsertPt] = TheForwardingValue;
}

bool BBState::updateForwardSetForRead(RLEContext &Ctx, unsigned bit,
                                      LoadStoreValue Val) {
  // If there is already an available value for this location, use
  // the existing value.
  if (isTrackingMemLocation(bit))
    return true;

  // Track the new location and value.
  startTrackingMemLocation(bit, Val);
  return false;
}

void BBState::updateForwardSetForWrite(RLEContext &Ctx, unsigned bit,
                                       LoadStoreValue Val) {
  // This is a store.
  //
  // 1. Update any MemLocation that this MemLocation Must alias. As we have
  // a new value.
  //
  // 2. Invalidate any Memlocation that this location may alias, as their value
  // can no longer be forwarded.
  //
  MemLocation &R = Ctx.getMemLocation(bit);
  llvm::SmallVector<unsigned, 8> LocDeleteList;
  for (unsigned i = 0; i < ForwardSetIn.size(); ++i) {
    if (!isTrackingMemLocation(i))
      continue;
    MemLocation &L = Ctx.getMemLocation(i);
    // MustAlias, update the tracked value.
    if (L.isMustAliasMemLocation(R, Ctx.getAA())) {
      updateTrackedMemLocation(i, Val);
      continue;
    }
    if (!L.isMayAliasMemLocation(R, Ctx.getAA()))
      continue;
    // MayAlias, invaliate the MemLocation.
    LocDeleteList.push_back(i);
  }

  // Invalidate MayAlias memory locations.
  for (auto i : LocDeleteList) {
    stopTrackingMemLocation(i);
  }

  // Start tracking this memory location.
  startTrackingMemLocation(bit, Val);
}

void BBState::processWrite(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                           SILValue Val) {
  // Initialize the memory location.
  MemLocation L(Mem);

  // If we cant figure out the Base or Projection Path for the write,
  // process it as an unknown memory instruction.
  if (!L.isValid()) {
    processUnknownWriteInst(Ctx, I);
    return;
  }

  // Expand the given Mem into individual fields and process them as
  // separate writes.
  MemLocationList Locs;
  LoadStoreValueList Vals;
  MemLocation::expandWithValues(L, Val, &I->getModule(), Locs, Vals);
  for (unsigned i = 0; i < Locs.size(); ++i) {
    updateForwardSetForWrite(Ctx, Ctx.getMemLocationBit(Locs[i]), Vals[i]);
  }
}

void BBState::processRead(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                          SILValue Val, bool PF) {
  // Initialize the memory location.
  MemLocation L(Mem);

  // If we cant figure out the Base or Projection Path for the read, simply
  // ignore it for now.
  if (!L.isValid())
    return;

  // Expand the given Val into individual fields and process them as
  // separate reads.
  MemLocationList Locs;
  LoadStoreValueList Vals;
  MemLocation::expandWithValues(L, Val, &I->getModule(), Locs, Vals);

  bool CanForward = true;
  for (auto &X : Locs) {
    CanForward &= isTrackingMemLocation(Ctx.getMemLocationBit(X));
  }  

  // We do not have every location available, track the memory locations and
  // their values from this instruction, and return.
  if (!CanForward) {
    for (unsigned i = 0; i < Locs.size(); ++i) {
      updateForwardSetForRead(Ctx, Ctx.getMemLocationBit(Locs[i]), Vals[i]);
    }
    return;
  }

  // At this point, we have all the memory locations and their values
  // available.
  //
  // If we are not doing forwarding just yet, simply return.
  if (!PF)
    return;

  // Lastly, set up the forwardable value right before this instruction.
  setupRLE(Ctx, L, I);
}

void BBState::processStoreInst(RLEContext &Ctx, StoreInst *SI) {
  processWrite(Ctx, SI, SI->getDest(), SI->getSrc());
}

void BBState::processLoadInst(RLEContext &Ctx, LoadInst *LI, bool PF) {
  processRead(Ctx, LI, LI->getOperand(), SILValue(LI), PF);
}

void BBState::processUnknownWriteInst(RLEContext &Ctx, SILInstruction *I) {
  llvm::SmallVector<unsigned, 8> LocDeleteList;
  for (unsigned i = 0; i < ForwardSetIn.size(); ++i) {
    if (!isTrackingMemLocation(i))
      continue;
    // Invalidate any location this instruction may write to.
    //
    // TODO: checking may alias with Base is overly conservative,
    // we should check may alias with base plus projection path.
    auto *AA = Ctx.getAA();
    MemLocation &R = Ctx.getMemLocation(i);
    if (!AA->mayWriteToMemory(I, R.getBase()))
      continue;
    // MayAlias.
    LocDeleteList.push_back(i);
  }

  for (auto i : LocDeleteList) {
    stopTrackingMemLocation(i);
  }
}

/// Promote stored values to loads and merge duplicated loads.
bool BBState::optimize(RLEContext &Ctx, bool PF) {
  auto II = BB->begin(), E = BB->end();
  bool Changed = false;
  while (II != E) {
    SILInstruction *Inst = II++;
    DEBUG(llvm::dbgs() << "    Visiting: " << *Inst);

    // This is a StoreInst, try to see whether it clobbers any forwarding
    // value.
    if (auto *SI = dyn_cast<StoreInst>(Inst)) {
      processStoreInst(Ctx, SI);
      continue;
    }

    // This is a LoadInst. Let's see if we can find a previous loaded, stored
    // value to use instead of this load.
    if (auto *LI = dyn_cast<LoadInst>(Inst)) {
      processLoadInst(Ctx, LI, PF);
      continue;
    }

    // If this instruction has side effects, but is inert from a load store
    // perspective, skip it.
    if (isRLEInertInstruction(Inst)) {
      DEBUG(llvm::dbgs() << "        Found inert instruction: " << *Inst);
      continue;
    }

    // If this instruction does not read or write memory, we can skip it.
    if (!Inst->mayReadOrWriteMemory()) {
      DEBUG(llvm::dbgs() << "        Found readnone instruction, does not "
                            "affect loads and stores.\n");
      continue;
    }

    // If we have an instruction that may write to memory and we can not prove
    // that it and its operands can not alias a load we have visited, invalidate
    // that load.
    if (Inst->mayWriteToMemory()) {
      // Invalidate all the aliasing location.
      processUnknownWriteInst(Ctx, Inst);
    }
  }

  // The basic block is finished, see whether there is a change in the
  // ForwardSetOut set.
  return updateForwardSetOut();
}

void BBState::mergePredecessorState(BBState &OtherState) {
  // Merge in the predecessor state.
  llvm::SmallVector<unsigned, 8> LocDeleteList;
  for (unsigned i = 0; i < ForwardSetIn.size(); ++i) {
    // If the predecessor basic block does not have a LoadStoreValue available,
    // then there is no available value to forward to this MemLocation.
    if (!OtherState.ForwardSetOut[i]) {
      stopTrackingMemLocation(i);
      continue;
    }

    // There are multiple values from multiple predecessors, set this as
    // a covering value. 
    //
    // NOTE: We do not need to track the value itself, as we can always go
    // to the predecessors BBState to find it.
    ForwardValIn[i].setCoveringValue();
  }
}

void BBState::mergePredecessorStates(RLEContext &Ctx) {
  // Clear the state if the basic block has no predecessor.
  if (BB->getPreds().begin() == BB->getPreds().end()) {
    clearMemLocations();
    return;
  }

  // We initialize the state with the first predecessor's state and merge
  // in states of other predecessors.
  bool HasAtLeastOnePred = false;
  SILBasicBlock *TheBB = getBB();
  // For each predecessor of BB...
  for (auto Pred : BB->getPreds()) {
    BBState &Other = Ctx.getBBLocState(Pred);
    // If we have not had at least one predecessor, initialize BBState
    // with the state of the initial predecessor.
    if (!HasAtLeastOnePred) {
      ForwardSetIn = Other.ForwardSetOut;
      ForwardValIn = Other.ForwardValOut;
    } else {
      mergePredecessorState(Other);
    }
    HasAtLeastOnePred = true;
  }

  for (auto &X : ForwardValIn) {
    assert(X.second.isValid() && "Invalid load store value");
  }
}

//===----------------------------------------------------------------------===//
//                          RLEContext Implementation
//===----------------------------------------------------------------------===//

RLEContext::RLEContext(SILFunction *F, AliasAnalysis *AA,
                       PostOrderFunctionInfo::reverse_range RPOT)
    : F(F), AA(AA), ReversePostOrder(RPOT) {
  // Walk over the function and find all the locations accessed by
  // this function.
  MemLocation::enumerateMemLocations(*F, MemLocationVault, LocToBitIndex,
                                     TypeExpansionVault);

  // For all basic blocks in the function, initialize a BB state. Since we
  // know all the locations accessed in this function, we can resize the bit
  // vector to the approproate size.
  SILBasicBlock *EBB = F->begin();
  for (auto &B : *F) {
    BBToLocState[&B] = BBState();
    // We set the initial state of unreachable block to 0, as we do not have
    // a value for the location.
    //
    // This is a bit conservative as we could be missing forwarding
    // opportunities. i.e. a joint block with 1 predecessor being an
    // unreachable block.
    //
    // we rely on other passes to clean up unreachable block.
    BBToLocState[&B].init(&B, MemLocationVault.size(), isReachable(&B));
  }
}

MemLocation &RLEContext::getMemLocation(const unsigned index) {
  return MemLocationVault[index];
}

unsigned RLEContext::getMemLocationBit(const MemLocation &Loc) {
  // Return the bit position of the given Loc in the MemLocationVault. The bit
  // position is then used to set/reset the bitvector kept by each BBState.
  //
  // We should have the location populated by the enumerateMemLocation at this
  // point.
  //
  auto Iter = LocToBitIndex.find(Loc);
  assert(Iter != LocToBitIndex.end() &&
         "MemLocation should have been enumerated");
  return Iter->second;
}

SILValue RLEContext::computePredecessorCoveringValue(SILBasicBlock *BB,
                                                     MemLocation &L) {
  // This is a covering value, need to go to each of the predecessors to
  // materialize them and create a SILArgument to merge them.
  //
  // If any of the predecessors can not forward an edge value, bail out
  // for now.
  //
  // *NOTE* This is a strong argument in favor of representing PHI nodes
  // separately from SILArguments.
  //
  // TODO: this is overly conservative, we should only check basic blocks
  // which are relevant. Or better, we can create a trampoline basic block
  // if the predecessor has a non-edgevalue terminator inst.
  //
  for (auto &BB : *BB->getParent()) {
    if (auto *TI = BB.getTerminator())
      if (!isa<CondBranchInst>(TI) && !isa<BranchInst>(TI) &&
          !isa<ReturnInst>(TI) && !isa<UnreachableInst>(TI))
        return SILValue();
  }

  // At this point, we know this MemLocation has available value and we also
  // know we can forward a SILValue from every predecesor. It is safe to
  // insert the basic block argument.
  BBState &Forwarder = getBBLocState(BB);
  SILValue TheForwardingValue = BB->createBBArg(L.getType());

  // For the given MemLocation, we just created a concrete value at the
  // beginning of this basic block. Update the ForwardValOut for the
  // current basic block.
  //
  // ForwardValOut keeps all the MemLocations and their forwarding values
  // at the end of the basic block. If a MemLocation has a covering value
  // at the end of the basic block, we can now replace the covering value with
  // this concrete SILArgument.
  //
  // However, if the MemLocation has a concrete value, we know there must
  // be an instruction that generated the concrete value between the current
  // instruction and the end of the basic block, we do not update the 
  // ForwardValOut in this case.
  //
  // NOTE: This is necessary to prevent an infinite loop while materializing
  // the covering value.
  //
  // Imagine an empty selfloop block with 1 predecessor having a load [A], to
  // materialize [A]'s covering value, we go to its predecessors. However,
  // the backedge will carry a covering value as well in this case.
  //
  MemLocationList Locs;
  LoadStoreValueList Vals;
  MemLocation::expandWithValues(L, TheForwardingValue, &BB->getModule(), Locs,
                                Vals);
  ValueTableMap &VTM = Forwarder.getForwardValOut();
  for (unsigned i = 0; i < Locs.size(); ++i) {
    unsigned bit = getMemLocationBit(Locs[i]);
    if (!VTM[bit].isCoveringValue())
      continue;
    VTM[bit] = Vals[i];
  }

  // Compute the SILArgument for the covering value.
  llvm::SmallVector<SILBasicBlock *, 4> Preds;
  for (auto Pred : BB->getPreds()) {
    Preds.push_back(Pred);
  }

  llvm::DenseMap<SILBasicBlock *, SILValue> Args;
  for (auto Pred : Preds) {
    BBState &Forwarder = getBBLocState(Pred);
    // Call computeForwardingValues with using ForwardValOut as we are
    // computing the MemLocation value at the end of each predecessor. 
    Args[Pred] = Forwarder.computeForwardingValues(*this, L,
                                                   Pred->getTerminator(),
                                                   true);
    assert(Args[Pred] && "Fail to create a forwarding value");
  }

  // Create the new SILArgument and set ForwardingValue to it.
  for (auto Pred : Preds) {
    // Update all edges. We do not create new edges in between BBs so this
    // information should always be correct.
    addNewEdgeValueToBranch(Pred->getTerminator(), BB, Args[Pred]);
  }

  return TheForwardingValue;
}

bool RLEContext::collectForwardingValues(SILBasicBlock *B, MemLocation &L,
                                         MemLocationValueMap &Values,
                                         bool UseForwardValOut) {
  // First, we need to materialize all the MemLocation with covering set
  // LoadStoreValue.

  // Expand the location into its individual fields.
  MemLocationSet CSLocs;
  MemLocationList Locs;
  MemLocation::expand(L, &B->getModule(), Locs, getTypeExpansionVault());

  // Are we using the ForwardVal at the end of the basic block or not.
  // If we are collecting values at the end of the basic block, we can
  // use its ForwardValOut.
  //
  BBState &Forwarder = getBBLocState(B);
  ValueTableMap &OTM = UseForwardValOut ? Forwarder.getForwardValOut() :
                                          Forwarder.getForwardValIn();
  for (auto &X : Locs) {
    Values[X] = OTM[getMemLocationBit(X)];
    if (!Values[X].isCoveringValue())
      continue;
    CSLocs.insert(X);
  }

  // Try to reduce it to the minimum # of locations possible, this will help
  // us to generate as few extractions as possible.
  MemLocation::reduce(L, &B->getModule(), CSLocs);

  // To handle covering value, we need to go to the predecessors and
  // materialize them there.
  for (auto &X : CSLocs) {
    SILValue V = computePredecessorCoveringValue(B, X);
    if (!V)
       return false;
    // We've constructed a concrete value for the covering value. Expan and
    // collect the newly created forwardable values.
    MemLocationList Locs;
    LoadStoreValueList Vals;
    MemLocation::expandWithValues(X, V, &B->getModule(), Locs, Vals);
    for (unsigned i = 0; i < Locs.size() ; ++i) {
      Values[Locs[i]] = Vals[i]; 
      assert(Values[Locs[i]].isValid() && "Invalid load store value");
    }
  }

  // Second, collect all non-covering values into the MemLocationValueMap.
  ValueTableMap &VTM = Forwarder.getForwardValIn();
  for (auto &X : Locs) {
    // These locations have a value already, i.e. materialized covering value.
    if (Values.find(X) != Values.end())
      continue;
    Values[X] = VTM[getMemLocationBit(X)];
    assert(Values[X].isValid() && "Invalid load store value");
  }

  // Done, we've successfully collected all the values for this MemLocation.
  return true;
}

bool RLEContext::run() {
  // Process basic blocks in RPO. After the data flow converges, run last
  // iteration and perform load forwarding.
  bool LastIteration = false;
  bool ForwardSetChanged = false;
  do {
    ForwardSetChanged = false;
    for (SILBasicBlock *BB : ReversePostOrder) {
      BBState &Forwarder = getBBLocState(BB);
      assert(Forwarder.getBB() == BB && "We just constructed this!?");

      // Merge the predecessors. After merging, BBState now contains
      // lists of available memory locations and their values that reach the
      // beginning of the basic block along all paths.
      Forwarder.mergePredecessorStates(*this);

      // Merge duplicate loads, and forward stores to
      // loads. We also update lists of stores|loads to reflect the end
      // of the basic block.
      ForwardSetChanged |= Forwarder.optimize(*this, LastIteration);
    }

    // Last iteration completed, we are done here.
    if (LastIteration)
      break;

    // ForwardSetOut have not changed in any basic block. Run one last
    // the data flow has converged, run last iteration and try to perform
    // load forwarding.
    //
    if (!ForwardSetChanged) {
      LastIteration = true;
    }

    // ForwardSetOut in some basic blocks changed, rerun the data flow.
    //
    // TODO: We only need to rerun basic blocks with predecessors changed.
    // use a worklist in the future.
    //
  } while (ForwardSetChanged || LastIteration);

  // Finally, perform the redundant load replacements.
  static int count = 0;
  bool SILChanged = false;
  for (auto &X : BBToLocState) {
    for (auto &F : X.second.getRL()) {
      SILChanged = true;
      SILValue(F.first).replaceAllUsesWith(F.second);
      ++NumForwardedLoads;
    }
  }

  return SILChanged;
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

namespace {

class GlobalRedundantLoadElimination : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    SILFunction *F = getFunction();
    DEBUG(llvm::dbgs() << "***** Redundant Load Elimination on function: "
                       << F->getName() << " *****\n");

    auto *AA = PM->getAnalysis<AliasAnalysis>();
    auto *PO = PM->getAnalysis<PostOrderAnalysis>()->get(F);

    RLEContext RLE(F, AA, PO->getReversePostOrder());
    if (RLE.run())
      invalidateAnalysis(SILAnalysis::PreserveKind::ProgramFlow);
  }

  StringRef getName() override { return "SIL Redundant Load Elimination"; }
};

} // end anonymous namespace

SILTransform *swift::createGlobalRedundantLoadElimination() {
  return new GlobalRedundantLoadElimination();
}

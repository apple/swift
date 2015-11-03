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

  /// Keeps all the locations for the current function. The BitVector in each
  /// BBState is then laid on top of it to keep track of which MemLocation
  /// has a downward available value.
  std::vector<MemLocation> MemLocationVault;

  /// Caches a list of projection paths to leaf nodes in the given type.
  TypeExpansionMap TypeExpansionVault;

  /// Contains a map between MemLocation to their index in the MemLocationVault.
  llvm::DenseMap<MemLocation, unsigned> LocToBitIndex;

  /// A "map" from a BBID (which is just an index) to an BBState.
  std::vector<BBState> BBIDToBBStateMap;

  /// A map from each BasicBlock to its index in the BBIDToBBStateMap.
  ///
  /// TODO: Each block does not need its own BBState instance. Only
  /// the set of reaching loads and stores is specific to the block.
  llvm::DenseMap<SILBasicBlock *, unsigned> BBToBBIDMap;

public:
  RLEContext(SILFunction *F, AliasAnalysis *AA,
             PostOrderFunctionInfo::reverse_range RPOT);

  RLEContext(const RLEContext &) = delete;
  RLEContext(RLEContext &&) = default;
  ~RLEContext() = default;

  bool run();

  AliasAnalysis *getAA() const { return AA; }

  TypeExpansionMap &getTypeExpansionVault() { return TypeExpansionVault; }

  BBState &getBBState(SILBasicBlock *BB) {
    auto IDIter = BBToBBIDMap.find(BB);
    assert(IDIter != BBToBBIDMap.end() && "We just constructed this!?");
    unsigned ID = IDIter->second;
    BBState &Forwarder = BBIDToBBStateMap[ID];
    return Forwarder;
  }

  /// Get the bit representing the location in the MemLocationVault.
  unsigned getMemLocationBit(const MemLocation &L);

  /// Given the bit, get the memory location from the MemLocationVault.
  MemLocation &getMemLocation(const unsigned index);

  /// Given a memory location, collect all the LoadStoreValues for this
  /// memory location. collectRLEValues assumes that every part of this
  /// memory location has a valid LoadStoreValue.
  bool collectRLEValues(SILInstruction *I, MemLocation &L,
                        MemLocationValueMap &Values);

  /// Dump all the memory locations in the MemLocationVault.
  void printMemLocationVault() const {
    for (auto &X : MemLocationVault) {
      X.print();
    }
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

  /// If ForwardSetIn changes while processing a basicblock, then all its
  /// predecessors needs to be rerun.
  llvm::BitVector ForwardSetIn;

  /// A bit vector for which the ith bit represents the ith MemLocation in
  /// MemLocationVault. If the bit is set, then the location currently has an
  /// downward visible value.
  llvm::BitVector ForwardSetOut;

  /// This is a list of MemLocations that have available values.
  ///
  /// TODO: can we create a LoadStoreValue vault so that we do not need to keep
  /// them per basic block. This would also give ForwardSetVal more symmetry.
  /// i.e. MemLocation and LoadStoreValue both represented as bit vector indices.
  ///
  llvm::SmallMapVector<unsigned, LoadStoreValue, 8> ForwardSetVal;

  /// Keep a list of *materialized* LoadStoreValues in the current basic block.
  llvm::SmallMapVector<MemLocation, SILValue, 8> MaterializedValues;
  
  /// Keeps a list of replaceable instructions in the current basic block as
  /// well as their SILValue replacement.
  llvm::DenseMap<SILInstruction *, SILValue> RedundantLoads;

  /// Check whether the ForwardSetOut has changed. If it does, we need to
  /// rerun the data flow to reach fixed point.
  bool updateForwardSetOut() {
    bool Changed = (ForwardSetIn != ForwardSetOut);
    ForwardSetOut = ForwardSetIn;
    return Changed;
  }

  /// BitVector manipulation fucntions.
  void clearMemLocations();
  void startTrackingMemLocation(unsigned bit, LoadStoreValue Val);
  void stopTrackingMemLocation(unsigned bit);
  void updateTrackedMemLocation(unsigned bit, LoadStoreValue Val);
  bool isTrackingMemLocation(unsigned bit);

public:
  BBState() = default;

  void init(SILBasicBlock *NewBB, unsigned bitcnt) {
    BB = NewBB;
    // The initial state of ForwardSetOut should be all 1's. Otherwise the
    // dataflow solution could be too conservative.
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
    ForwardSetOut.resize(bitcnt, true);
  }

  llvm::SmallMapVector<unsigned, LoadStoreValue, 8>  &getForwardSetVal() {
    return ForwardSetVal;
  }

  SILBasicBlock *getBB() const { return BB; }

  llvm::DenseMap<SILInstruction *, SILValue> &getRL() {
    return RedundantLoads;
  }

  bool optimize(RLEContext &Ctx, bool PF);

  /// Set up the value for redundant load elimination.
  bool setupRLE(RLEContext &Ctx, SILInstruction *I, SILValue Mem);

  /// Merge in the states of all predecessors.
  void
  mergePredecessorStates(llvm::DenseMap<SILBasicBlock *, unsigned> &BBToBBIDMap,
                         std::vector<BBState> &BBIDToBBStateMap);

  /// Process Instruction which writes to memory in an unknown way.
  void processUnknownWriteInst(RLEContext &Ctx, SILInstruction *I);

  /// Process LoadInst. Extract MemLocations from LoadInst.
  void processLoadInst(RLEContext &Ctx, LoadInst *LI, bool PF);

  /// Process LoadInst. Extract MemLocations from StoreInst.
  void processStoreInst(RLEContext &Ctx, StoreInst *SI);

private:
  /// Merge in the state of an individual predecessor.
  void mergePredecessorState(BBState &OtherState);

  /// MemLocation read has been extracted, expanded and mapped to the bit
  /// position in the bitvector. process it using the bit position.
  bool updateForwardSetForRead(RLEContext &Ctx, unsigned Bit, LoadStoreValue Val);

  /// MemLocation written has been extracted, expanded and mapped to the bit
  /// position in the bitvector. process it using the bit position.
  void updateForwardSetForWrite(RLEContext &Ctx, unsigned Bit, LoadStoreValue Val);

  /// There is a read to a MemLocation, expand the MemLocation into individual
  /// fields before processing them.
  void processRead(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                   SILValue Val, bool PF);

  /// There is a write to a MemLocation, expand the MemLocation into individual
  /// fields before processing them.
  void processWrite(RLEContext &Ctx, SILInstruction *I, SILValue Mem,
                    SILValue Val);
};

} // end anonymous namespace

bool BBState::isTrackingMemLocation(unsigned bit) {
  return ForwardSetIn.test(bit);
}

void BBState::stopTrackingMemLocation(unsigned bit) {
  ForwardSetIn.reset(bit);
  ForwardSetVal.erase(bit);
}

void BBState::clearMemLocations() {
  ForwardSetIn.reset();
  ForwardSetVal.clear();
}

void BBState::startTrackingMemLocation(unsigned bit, LoadStoreValue Val) {
  assert(Val.isValid() && "Invalid load store value");
  ForwardSetIn.set(bit);
  ForwardSetVal[bit] = Val;
}

void BBState::updateTrackedMemLocation(unsigned bit, LoadStoreValue Val) {
  assert(Val.isValid() && "Invalid load store value");
  ForwardSetVal[bit] = Val;
}

bool BBState::setupRLE(RLEContext &Ctx, SILInstruction *I, SILValue Mem) {
  // We have already materialized a SILValue for this MemLocation. Use it.
  MemLocation L(Mem);
  if (MaterializedValues.find(L) != MaterializedValues.end()) {
    RedundantLoads[I] = MaterializedValues[L];
    return true;
  }

  // We do not have a SILValue for the current MemLocation, try to construct
  // one.
  //
  // Collect the locations and their corresponding values into a map.
  MemLocationValueMap Values;
  if (!Ctx.collectRLEValues(I, L, Values))
    return false;

  // Reduce the available values into a single SILValue we can use to forward.
  SILModule *Mod = &I->getModule();
  SILValue TheForwardingValue;
  TheForwardingValue = MemLocation::reduceWithValues(L, Mod, Values, I);
  if (!TheForwardingValue)
    return false;

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
  RedundantLoads[I] = TheForwardingValue;
  // Make sure we cache this constructed SILValue so that we could use it
  // later.
  MaterializedValues[L] = TheForwardingValue;
  return true;
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

  // Lastly, forward value to the load.
  setupRLE(Ctx, I, Mem);
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
    if (OtherState.ForwardSetOut[i]) {
      // There are multiple values from multiple predecessors, set this as
      // a covering value. We do not need to track the value itself, as we
      // can always go to the predecessors BBState to find it.
      ForwardSetVal[i].setCoveringValue();
      continue;
    }
    // If this location does have an available value, then clear it.
    stopTrackingMemLocation(i);
  }
}

void BBState::mergePredecessorStates(
    llvm::DenseMap<SILBasicBlock *, unsigned> &BBToBBIDMap,
    std::vector<BBState> &BBIDToBBStateMap) {
  // Clear the state if the basic block has no predecessor.
  if (BB->getPreds().begin() == BB->getPreds().end()) {
    clearMemLocations();
    return;
  }

  // We initialize the state with the first
  // predecessor's state and merge in states of other predecessors.
  //
  bool HasAtLeastOnePred = false;
  SILBasicBlock *TheBB = getBB();
  // For each predecessor of BB...
  for (auto Pred : BB->getPreds()) {

    // Lookup the BBState associated with the predecessor and merge the
    // predecessor in.
    auto I = BBToBBIDMap.find(Pred);

    // If we can not lookup the BBID then the BB was not in the RPO,
    // implying that it is unreachable. LLVM will ensure that the BB is removed
    // if we do not reach it at the SIL level. Since it is unreachable, ignore
    // it.
    if (I == BBToBBIDMap.end())
      continue;

    BBState &Other = BBIDToBBStateMap[I->second];

    // If we have not had at least one predecessor, initialize BBState
    // with the state of the initial predecessor.
    // If BB is also a predecessor of itself, we should not initialize.
    if (!HasAtLeastOnePred) {
      DEBUG(llvm::dbgs() << "    Initializing with pred: " << I->second
                         << "\n");
      ForwardSetIn = Other.ForwardSetOut;
      ForwardSetVal = Other.ForwardSetVal;
    } else {
      DEBUG(llvm::dbgs() << "    Merging with pred  bb" << Pred->getDebugID()
                         << "\n");
      mergePredecessorState(Other);
    }
    HasAtLeastOnePred = true;
  }

  for (auto &X : ForwardSetVal) {
    assert(X.second.isValid() && "Invalid load store value");
  }
}

//===----------------------------------------------------------------------===//
//                          RLEContext Implementation
//===----------------------------------------------------------------------===//

static inline unsigned
roundPostOrderSize(PostOrderFunctionInfo::reverse_range R) {
  unsigned PostOrderSize = std::distance(R.begin(), R.end());

  // NextPowerOf2 operates on uint64_t, so we can not overflow since our input
  // is a 32 bit value. But we need to make sure if the next power of 2 is
  // greater than the representable UINT_MAX, we just pass in (1 << 31) if the
  // next power of 2 is (1 << 32).
  uint64_t SizeRoundedToPow2 = llvm::NextPowerOf2(PostOrderSize);
  if (SizeRoundedToPow2 > uint64_t(UINT_MAX))
    return 1 << 31;
  return unsigned(SizeRoundedToPow2);
}

RLEContext::RLEContext(SILFunction *F, AliasAnalysis *AA,
                       PostOrderFunctionInfo::reverse_range RPOT)
    : F(F), AA(AA), ReversePostOrder(RPOT),
      BBToBBIDMap(roundPostOrderSize(RPOT)),
      BBIDToBBStateMap(roundPostOrderSize(RPOT)) {
  // Walk over the function and find all the locations accessed by
  // this function.
  MemLocation::enumerateMemLocations(*F, MemLocationVault, LocToBitIndex,
                                     TypeExpansionVault);

  for (SILBasicBlock *BB : ReversePostOrder) {
    unsigned count = BBToBBIDMap.size();
    BBToBBIDMap[BB] = count;
    BBIDToBBStateMap[count].init(BB, MemLocationVault.size());
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

bool RLEContext::collectRLEValues(SILInstruction *I, MemLocation &L,
                                  MemLocationValueMap &Values) {
  MemLocationList Locs;
  MemLocation::expand(L, &I->getModule(), Locs, getTypeExpansionVault());
  SILBasicBlock *BB = I->getParent();
  BBState &Forwarder = getBBState(BB);
  for (auto &X : Locs) {
    Values[X] = Forwarder.getForwardSetVal()[getMemLocationBit(X)];
    // Currently do not handle covering value, return false for now.
    // NOTE: to handle covering value, we need to go to the predecessor and
    // materialize them there.
    if (Values[X].isCoveringValue())
      return false;
  }

  // Sanity check to make sure we have valid load store values for each
  // memory location.
  for (auto &X : Locs) {
    assert(Values[X].isValid() && "Invalid load store value");
  }
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
      auto IDIter = BBToBBIDMap.find(BB);
      assert(IDIter != BBToBBIDMap.end() && "We just constructed this!?");
      unsigned ID = IDIter->second;
      BBState &Forwarder = BBIDToBBStateMap[ID];
      assert(Forwarder.getBB() == BB && "We just constructed this!?");

      // Merge the predecessors. After merging, BBState now contains
      // lists of available memory locations and their values that reach the
      // beginning of the basic block along all paths.
      Forwarder.mergePredecessorStates(BBToBBIDMap, BBIDToBBStateMap);

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
  bool SILChanged = false;
  for (auto &X : BBIDToBBStateMap) {
    for (auto &F : X.getRL()) {
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

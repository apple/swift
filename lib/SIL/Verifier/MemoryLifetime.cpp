//===--- MemoryLifetime.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-memory-lifetime"

#include "swift/SIL/MemoryLifetime.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

llvm::cl::opt<bool> DontAbortOnMemoryLifetimeErrors(
    "dont-abort-on-memory-lifetime-errors",
    llvm::cl::desc("Don't abort compliation if the memory lifetime checker "
                   "detects an error."));

/// Debug dump a location bit vector.
void swift::printBitsAsArray(llvm::raw_ostream &OS, const SmallBitVector &bits) {
  const char *separator = "";
  OS << '[';
  for (int idx = bits.find_first(); idx >= 0; idx = bits.find_next(idx)) {
    OS << separator << idx;
    separator = ",";
  }
  OS << ']';
}

void swift::dumpBits(const SmallBitVector &bits) {
  llvm::dbgs() << bits << '\n';
}

namespace swift {
namespace {

//===----------------------------------------------------------------------===//
//                            Utility functions
//===----------------------------------------------------------------------===//

/// Enlarge the bitset if needed to set the bit with \p idx.
static void setBitAndResize(SmallBitVector &bits, unsigned idx) {
  if (bits.size() <= idx)
    bits.resize(idx + 1);
  bits.set(idx);
}

static bool allUsesInSameBlock(AllocStackInst *ASI) {
  SILBasicBlock *BB = ASI->getParent();
  int numDeallocStacks = 0;
  for (Operand *use : ASI->getUses()) {
    SILInstruction *user = use->getUser();
    if (isa<DeallocStackInst>(user)) {
      ++numDeallocStacks;
      if (user->getParent() != BB)
        return false;
    }
  }
  // In case of an unreachable, the dealloc_stack can be missing. In this
  // case we don't treat it as a single-block location.
  assert(numDeallocStacks <= 1 &&
    "A single-block stack location cannot have multiple deallocations");
  return numDeallocStacks == 1;
}

static bool shouldTrackLocation(SILType ty, SILFunction *function) {
  // Ignore empty tuples and empty structs.
  if (auto tupleTy = ty.getAs<TupleType>()) {
    return tupleTy->getNumElements() != 0;
  }
  if (StructDecl *decl = ty.getStructOrBoundGenericStruct()) {
    return decl->getStoredProperties().size() != 0;
  }
  return true;
}

} // anonymous namespace
} // namespace swift


//===----------------------------------------------------------------------===//
//                     MemoryLocations members
//===----------------------------------------------------------------------===//

MemoryLocations::Location::Location(SILValue val, unsigned index, int parentIdx) :
      representativeValue(val),
      parentIdx(parentIdx) {
  assert(((parentIdx >= 0) ==
    (isa<StructElementAddrInst>(val) || isa<TupleElementAddrInst>(val))) &&
    "sub-locations can only be introduced with struct/tuple_element_addr");
  setBitAndResize(subLocations, index);
  setBitAndResize(selfAndParents, index);
}

void MemoryLocations::Location::updateFieldCounters(SILType ty, int increment) {
  SILFunction *function = representativeValue->getFunction();
  if (shouldTrackLocation(ty, function)) {
    numFieldsNotCoveredBySubfields += increment;
    if (!ty.isTrivial(*function))
      numNonTrivialFieldsNotCovered += increment;
  }
}

static SILValue getBaseValue(SILValue addr) {
  while (true) {
    switch (addr->getKind()) {
      case ValueKind::BeginAccessInst:
        addr = cast<BeginAccessInst>(addr)->getOperand();
        break;
      default:
        return addr;
    }
  }
}

int MemoryLocations::getLocationIdx(SILValue addr) const {
  auto iter = addr2LocIdx.find(getBaseValue(addr));
  if (iter == addr2LocIdx.end())
    return -1;
  return iter->second;
}

void MemoryLocations::analyzeLocations(SILFunction *function) {
  // As we have to limit the set of handled locations to memory, which is
  // guaranteed to be not aliased, we currently only handle indirect function
  // arguments and alloc_stack locations.
  for (SILArgument *arg : function->getArguments()) {
    SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
    switch (funcArg->getArgumentConvention()) {
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_In_Constant:
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_Out:
      analyzeLocation(funcArg);
      break;
    default:
      break;
    }
  }
  for (SILBasicBlock &BB : *function) {
    for (SILInstruction &I : BB) {
      auto *ASI = dyn_cast<AllocStackInst>(&I);
      if (ASI && !ASI->hasDynamicLifetime()) {
        if (allUsesInSameBlock(ASI)) {
          singleBlockLocations.push_back(ASI);
        } else {
          analyzeLocation(ASI);
        }
      }
    }
  }
}

void MemoryLocations::analyzeLocation(SILValue loc) {
  SILFunction *function = loc->getFunction();
  assert(function && "cannot analyze a SILValue which is not in a function");

  // Ignore trivial types to keep the number of locations small. Trivial types
  // are not interesting anyway, because such memory locations are not
  // destroyed.
  if (loc->getType().isTrivial(*function))
    return;

  if (!shouldTrackLocation(loc->getType(), function))
    return;

  unsigned currentLocIdx = locations.size();
  locations.push_back(Location(loc, currentLocIdx));
  SmallVector<SILValue, 8> collectedVals;
  SubLocationMap subLocationMap;
  if (!analyzeLocationUsesRecursively(loc, currentLocIdx, collectedVals,
                                      subLocationMap)) {
    locations.set_size(currentLocIdx);
    for (SILValue V : collectedVals) {
      addr2LocIdx.erase(V);
    }
    return;
  }
  addr2LocIdx[loc] = currentLocIdx;
}

void MemoryLocations::handleSingleBlockLocations(
                       std::function<void (SILBasicBlock *block)> handlerFunc) {
  SILBasicBlock *currentBlock = nullptr;
  clear();

  // Walk over all collected single-block locations.
  for (SingleValueInstruction *SVI : singleBlockLocations) {
    // Whenever the parent block changes, process the block's locations.
    if (currentBlock && SVI->getParent() != currentBlock) {
      handlerFunc(currentBlock);
      clear();
    }
    currentBlock = SVI->getParent();
    analyzeLocation(SVI);
  }
  // Process the last block's locations.
  if (currentBlock)
    handlerFunc(currentBlock);
  clear();
}

const MemoryLocations::Bits &MemoryLocations::getNonTrivialLocations() {
  if (nonTrivialLocations.empty()) {
    // Compute the bitset lazily.
    nonTrivialLocations.resize(getNumLocations());
    nonTrivialLocations.reset();
    unsigned idx = 0;
    for (Location &loc : locations) {
      initFieldsCounter(loc);
      if (loc.numNonTrivialFieldsNotCovered != 0)
        nonTrivialLocations.set(idx);
      ++idx;
    }
  }
  return nonTrivialLocations;
}

void MemoryLocations::dump() const {
  unsigned idx = 0;
  for (const Location &loc : locations) {
    llvm::dbgs() << "location #" << idx << ": sublocs=" << loc.subLocations
                 << ", parent=" << loc.parentIdx
                 << ", parentbits=" << loc.selfAndParents
                 << ", #f=" << loc.numFieldsNotCoveredBySubfields
                 << ", #ntf=" << loc.numNonTrivialFieldsNotCovered
                 << ": " << loc.representativeValue;
    ++idx;
  }
}

void MemoryLocations::clear() {
  locations.clear();
  addr2LocIdx.clear();
  nonTrivialLocations.clear();
}

bool MemoryLocations::analyzeLocationUsesRecursively(SILValue V, unsigned locIdx,
                                    SmallVectorImpl<SILValue> &collectedVals,
                                    SubLocationMap &subLocationMap) {
  for (Operand *use : V->getUses()) {
    SILInstruction *user = use->getUser();

    // We only handle addr-instructions which are planned to be used with
    // opaque values. We can still consider to support other addr-instructions
    // like addr-cast instructions. This somehow depends how opaque values will
    // look like.
    switch (user->getKind()) {
      case SILInstructionKind::StructElementAddrInst: {
        auto SEAI = cast<StructElementAddrInst>(user);
        if (!analyzeAddrProjection(SEAI, locIdx, SEAI->getFieldIndex(),
                                collectedVals, subLocationMap))
          return false;
        break;
      }
      case SILInstructionKind::TupleElementAddrInst: {
        auto *TEAI = cast<TupleElementAddrInst>(user);
        if (!analyzeAddrProjection(TEAI, locIdx, TEAI->getFieldIndex(),
                                collectedVals, subLocationMap))
          return false;
        break;
      }
      case SILInstructionKind::BeginAccessInst:
        if (!analyzeLocationUsesRecursively(cast<BeginAccessInst>(user), locIdx,
                                            collectedVals, subLocationMap))
          return false;
        break;
      case SILInstructionKind::StoreInst: {
        auto *SI = cast<StoreInst>(user);
        if (!SI->getSrc()->getType().isTrivial(*SI->getFunction()) &&
            SI->getOwnershipQualifier() == StoreOwnershipQualifier::Trivial) {
          // Storing a trivial value into a non trivial location can happen in
          // case of enums, e.g. store of Optional.none to an Optional<T> where T
          // is not trivial.
          // In such a case it can happen that the Optional<T> is not destoyed.
          // We currently cannot handle such patterns.
          return false;
        }
        break;
      }
      case SILInstructionKind::LoadInst:
      case SILInstructionKind::EndAccessInst:
      case SILInstructionKind::LoadBorrowInst:
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst:
      case SILInstructionKind::BeginApplyInst:
      case SILInstructionKind::DebugValueAddrInst:
      case SILInstructionKind::CopyAddrInst:
      case SILInstructionKind::YieldInst:
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::UnconditionalCheckedCastAddrInst:
      case SILInstructionKind::CheckedCastAddrBranchInst:
      case SILInstructionKind::UncheckedRefCastAddrInst:
        break;
      default:
        return false;
    }
  }
  return true;
}

bool MemoryLocations::analyzeAddrProjection(
    SingleValueInstruction *projection, unsigned parentLocIdx,unsigned fieldNr,
    SmallVectorImpl<SILValue> &collectedVals, SubLocationMap &subLocationMap) {

  if (!shouldTrackLocation(projection->getType(), projection->getFunction()))
    return false;

  unsigned &subLocIdx = subLocationMap[std::make_pair(parentLocIdx, fieldNr)];
  if (subLocIdx == 0) {
    subLocIdx = locations.size();
    assert(subLocIdx > 0);
    locations.push_back(Location(projection, subLocIdx, parentLocIdx));

    Location &parentLoc = locations[parentLocIdx];
    locations.back().selfAndParents |= parentLoc.selfAndParents;

    int idx = (int)parentLocIdx;
    do {
      Location &loc = locations[idx];
      setBitAndResize(loc.subLocations, subLocIdx);
      idx = loc.parentIdx;
    } while (idx >= 0);

    initFieldsCounter(parentLoc);
    assert(parentLoc.numFieldsNotCoveredBySubfields >= 1);
    parentLoc.updateFieldCounters(projection->getType(), -1);

    if (parentLoc.numFieldsNotCoveredBySubfields == 0) {
      int idx = (int)parentLocIdx;
      do {
        Location &loc = locations[idx];
        loc.subLocations.reset(parentLocIdx);
        idx = loc.parentIdx;
      } while (idx >= 0);
    }
  }

  if (!analyzeLocationUsesRecursively(projection, subLocIdx, collectedVals,
                                      subLocationMap)) {
    return false;
  }
  registerProjection(projection, subLocIdx);
  collectedVals.push_back(projection);
  return true;
}

void MemoryLocations::initFieldsCounter(Location &loc) {
  if (loc.numFieldsNotCoveredBySubfields >= 0)
    return;

  assert(loc.numNonTrivialFieldsNotCovered < 0);

  loc.numFieldsNotCoveredBySubfields = 0;
  loc.numNonTrivialFieldsNotCovered = 0;
  SILFunction *function = loc.representativeValue->getFunction();
  SILType ty = loc.representativeValue->getType();
  if (StructDecl *decl = ty.getStructOrBoundGenericStruct()) {
    if (decl->isResilient(function->getModule().getSwiftModule(),
                          function->getResilienceExpansion())) {
      loc.numFieldsNotCoveredBySubfields = INT_MAX;
      return;
    }
    SILModule &module = function->getModule();
    for (VarDecl *field : decl->getStoredProperties()) {
      loc.updateFieldCounters(
          ty.getFieldType(field, module, TypeExpansionContext(*function)), +1);
    }
    return;
  }
  if (auto tupleTy = ty.getAs<TupleType>()) {
    for (unsigned idx = 0, end = tupleTy->getNumElements(); idx < end; ++idx) {
      loc.updateFieldCounters(ty.getTupleElementType(idx), +1);
    }
    return;
  }
  loc.updateFieldCounters(ty, +1);
}


//===----------------------------------------------------------------------===//
//                     MemoryDataflow members
//===----------------------------------------------------------------------===//

MemoryDataflow::MemoryDataflow(SILFunction *function, unsigned numLocations) {
  // Resizing is mandatory! Just adding states with push_back would potentially
  // invalidate previous pointers to states, which are stored in block2State.
  blockStates.resize(function->size());

  unsigned idx = 0;
  unsigned numBits = numLocations;
  for (SILBasicBlock &BB : *function) {
    BlockState *st = &blockStates[idx++];
    st->block = &BB;
    st->entrySet.resize(numBits);
    st->genSet.resize(numBits);
    st->killSet.resize(numBits);
    st->exitSet.resize(numBits);
    block2State[&BB] = st;
  }
}

void MemoryDataflow::entryReachabilityAnalysis() {
  llvm::SmallVector<BlockState *, 16> workList;
  BlockState *entryState = &blockStates[0];
  assert(entryState ==
         block2State[entryState->block->getParent()->getEntryBlock()]);
  entryState->reachableFromEntry = true;
  workList.push_back(entryState);

  while (!workList.empty()) {
    BlockState *state = workList.pop_back_val();
    for (SILBasicBlock *succ : state->block->getSuccessorBlocks()) {
      BlockState *succState = block2State[succ];
      if (!succState->reachableFromEntry) {
        succState->reachableFromEntry = true;
        workList.push_back(succState);
      }
    }
  }
}

void MemoryDataflow::exitReachableAnalysis() {
  llvm::SmallVector<BlockState *, 16> workList;
  for (BlockState &state : blockStates) {
    if (state.block->getTerminator()->isFunctionExiting()) {
      state.exitReachability = ExitReachability::ReachesExit;
      workList.push_back(&state);
    } else if (isa<UnreachableInst>(state.block->getTerminator())) {
      state.exitReachability = ExitReachability::ReachesUnreachable;
      workList.push_back(&state);
    }
  }
  while (!workList.empty()) {
    BlockState *state = workList.pop_back_val();
    for (SILBasicBlock *pred : state->block->getPredecessorBlocks()) {
      BlockState *predState = block2State[pred];
      if (predState->exitReachability < state->exitReachability) {
        // As there are 3 states, each block can be put into the workList 2
        // times maximum.
        predState->exitReachability = state->exitReachability;
        workList.push_back(predState);
      }
    }
  }
}

void MemoryDataflow::solveForward(JoinOperation join) {
  // Pretty standard data flow solving.
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (BlockState &st : blockStates) {
      Bits bits = st.entrySet;
      assert(!bits.empty());
      for (SILBasicBlock *pred : st.block->getPredecessorBlocks()) {
        join(bits, block2State[pred]->exitSet);
      }
      if (firstRound || bits != st.entrySet) {
        changed = true;
        st.entrySet = bits;
        bits |= st.genSet;
        bits.reset(st.killSet);
        st.exitSet = bits;
      }
    }
    firstRound = false;
  } while (changed);
}

void MemoryDataflow::solveForwardWithIntersect() {
  solveForward([](Bits &entry, const Bits &predExit){
    entry &= predExit;
  });
}

void MemoryDataflow::solveForwardWithUnion() {
  solveForward([](Bits &entry, const Bits &predExit){
    entry |= predExit;
  });
}

void MemoryDataflow::solveBackward(JoinOperation join) {
  // Pretty standard data flow solving.
  bool changed = false;
  bool firstRound = true;
  do {
    changed = false;
    for (BlockState &st : llvm::reverse(blockStates)) {
      Bits bits = st.exitSet;
      assert(!bits.empty());
      for (SILBasicBlock *succ : st.block->getSuccessorBlocks()) {
        join(bits, block2State[succ]->entrySet);
      }
      if (firstRound || bits != st.exitSet) {
        changed = true;
        st.exitSet = bits;
        bits |= st.genSet;
        bits.reset(st.killSet);
        st.entrySet = bits;
      }
    }
    firstRound = false;
  } while (changed);
}

void MemoryDataflow::solveBackwardWithIntersect() {
  solveBackward([](Bits &entry, const Bits &predExit){
    entry &= predExit;
  });
}

void MemoryDataflow::solveBackwardWithUnion() {
  solveBackward([](Bits &entry, const Bits &predExit){
    entry |= predExit;
  });
}

void MemoryDataflow::dump() const {
  for (const BlockState &st : blockStates) {
    llvm::dbgs() << "bb" << st.block->getDebugID() << ":\n"
                 << "    entry: " << st.entrySet << '\n'
                 << "    gen:   " << st.genSet << '\n'
                 << "    kill:  " << st.killSet << '\n'
                 << "    exit:  " << st.exitSet << '\n';
  }
}


//===----------------------------------------------------------------------===//
//                          MemoryLifetimeVerifier
//===----------------------------------------------------------------------===//

/// A utility for verifying memory lifetime.
///
/// The MemoryLifetime utility checks the lifetime of memory locations.
/// This is limited to memory locations which are guaranteed to be not aliased,
/// like @in or @inout parameters. Also, alloc_stack locations are handled.
///
/// In addition to verification, the MemoryLifetime class can be used as utility
/// (e.g. base class) for optimizations, which need to compute memory lifetime.
class MemoryLifetimeVerifier {

  using Bits = MemoryLocations::Bits;
  using BlockState = MemoryDataflow::BlockState;

  SILFunction *function;
  MemoryLocations locations;

  /// Issue an error if \p condition is false.
  void require(bool condition, const Twine &complaint,
                               int locationIdx, SILInstruction *where);

  /// Issue an error if any bit in \p wrongBits is set.
  void require(const Bits &wrongBits, const Twine &complaint,
                               SILInstruction *where);

  /// Require that all the subLocation bits of the location, associated with
  /// \p addr, are clear in \p bits.
  void requireBitsClear(const Bits &bits, SILValue addr, SILInstruction *where);

  /// Require that all the subLocation bits of the location, associated with
  /// \p addr, are set in \p bits.
  void requireBitsSet(const Bits &bits, SILValue addr, SILInstruction *where);

  /// Handles locations of the predecessor's terminator, which are only valid
  /// in \p block.
  ///
  /// Example: @out results of try_apply. They are only valid in the
  /// normal-block, but not in the throw-block.
  void setBitsOfPredecessor(Bits &bits, SILBasicBlock *block);

  /// Initializes the data flow bits sets in the block states for all blocks.
  void initDataflow(MemoryDataflow &dataFlow);

  /// Initializes the data flow bits sets in the block state for a single block.
  void initDataflowInBlock(BlockState &state);

  /// Helper function to set bits for function arguments and returns.
  void setFuncOperandBits(BlockState &state, Operand &op,
                          SILArgumentConvention convention,
                          bool isTryApply);

  /// Perform all checks in the function after the data flow has been computed.
  void checkFunction(MemoryDataflow &dataFlow);

  /// Check all instructions in \p block, starting with \p bits as entry set.
  void checkBlock(SILBasicBlock *block, Bits &bits);

  /// Check a function argument against the current live \p bits at the function
  /// call.
  void checkFuncArgument(Bits &bits, Operand &argumentOp,
                          SILArgumentConvention argumentConvention,
                          SILInstruction *applyInst);

public:
  MemoryLifetimeVerifier(SILFunction *function) : function(function) {}

  /// The main entry point to verify the lifetime of all memory locations in
  /// the function.
  void verify();
};


void MemoryLifetimeVerifier::require(bool condition, const Twine &complaint,
                                     int locationIdx, SILInstruction *where) {
  if (condition)
    return;

  llvm::errs() << "SIL memory lifetime failure in @" << function->getName()
               << ": " << complaint << '\n';
  if (locationIdx >= 0) {
    llvm::errs() << "memory location: "
                 << locations.getLocation(locationIdx)->representativeValue;
  }
  llvm::errs() << "at instruction: " << *where << '\n';

  if (DontAbortOnMemoryLifetimeErrors)
    return;

  llvm::errs() << "in function:\n";
  function->print(llvm::errs());
  abort();
}

void MemoryLifetimeVerifier::require(const Bits &wrongBits,
                                const Twine &complaint, SILInstruction *where) {
  require(wrongBits.none(), complaint, wrongBits.find_first(), where);
}

void MemoryLifetimeVerifier::requireBitsClear(const Bits &bits, SILValue addr,
                                             SILInstruction *where) {
  if (auto *loc = locations.getLocation(addr)) {
    require(bits & loc->subLocations,
            "memory is initialized, but shouldn't", where);
  }
}

void MemoryLifetimeVerifier::requireBitsSet(const Bits &bits, SILValue addr,
                                           SILInstruction *where) {
  if (auto *loc = locations.getLocation(addr)) {
    require(~bits & loc->subLocations,
            "memory is not initialized, but should", where);
  }
}

void MemoryLifetimeVerifier::initDataflow(MemoryDataflow &dataFlow) {
  // Initialize the entry and exit sets to all-bits-set. Except for the function
  // entry.
  for (BlockState &st : dataFlow) {
    if (st.block == function->getEntryBlock()) {
      st.entrySet.reset();
      for (SILArgument *arg : function->getArguments()) {
        SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
        if (funcArg->getArgumentConvention() !=
              SILArgumentConvention::Indirect_Out) {
          locations.setBits(st.entrySet, arg);
        }
      }
    } else {
      st.entrySet.set();
    }
    st.exitSet.set();

    // Anything weired can happen in unreachable blocks. So just ignore them.
    // Note: while solving the dataflow, unreachable blocks are implicitly
    // ignored, because their entry/exit sets are all-ones and their gen/kill
    // sets are all-zeroes.
    if (st.reachableFromEntry)
      initDataflowInBlock(st);
  }
}

void MemoryLifetimeVerifier::initDataflowInBlock(BlockState &state) {
  // Initialize the genSet with special cases, like the @out results of an
  // try_apply in the predecessor block.
  setBitsOfPredecessor(state.genSet, state.block);

  for (SILInstruction &I : *state.block) {
    switch (I.getKind()) {
      case SILInstructionKind::LoadInst: {
        auto *LI = cast<LoadInst>(&I);
        switch (LI->getOwnershipQualifier()) {
          case LoadOwnershipQualifier::Take:
            state.killBits(LI->getOperand(), locations);
            break;
          default:
            break;
        }
        break;
      }
      case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
        auto *uccai = cast<UnconditionalCheckedCastAddrInst>(&I);
        state.killBits(uccai->getSrc(), locations);
        state.genBits(uccai->getDest(), locations);
        break;
      }
      case SILInstructionKind::CheckedCastAddrBranchInst: {
        // We handle this in the predecessor code.
        break;
      }
      case SILInstructionKind::UncheckedRefCastAddrInst: {
        auto *urcai = cast<UncheckedRefCastAddrInst>(&I);
        state.killBits(urcai->getSrc(), locations);
        state.genBits(urcai->getDest(), locations);
        break;
      }
      case SILInstructionKind::StoreInst:
        state.genBits(cast<StoreInst>(&I)->getDest(), locations);
        break;
      case SILInstructionKind::CopyAddrInst: {
        auto *CAI = cast<CopyAddrInst>(&I);
        if (CAI->isTakeOfSrc())
          state.killBits(CAI->getSrc(), locations);
        state.genBits(CAI->getDest(), locations);
        break;
      }
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::DeallocStackInst:
        state.killBits(I.getOperand(0), locations);
        break;
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst: {
        FullApplySite FAS(&I);
        for (Operand &op : I.getAllOperands()) {
          if (FAS.isArgumentOperand(op)) {
            setFuncOperandBits(state, op, FAS.getArgumentConvention(op),
                              isa<TryApplyInst>(&I));
          }
        }
        break;
      }
      case SILInstructionKind::YieldInst: {
        auto *YI = cast<YieldInst>(&I);
        for (Operand &op : YI->getAllOperands()) {
          setFuncOperandBits(state, op, YI->getArgumentConventionForOperand(op),
                             /*isTryApply=*/ false);
        }
        break;
      }
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::setBitsOfPredecessor(Bits &bits,
                                                  SILBasicBlock *block) {
  SILBasicBlock *pred = block->getSinglePredecessorBlock();
  if (!pred)
    return;

  if (auto *ccabi =
          dyn_cast<CheckedCastAddrBranchInst>(pred->getTerminator())) {
    if (block == ccabi->getSuccessBB()) {
      switch (ccabi->getConsumptionKind()) {
      case CastConsumptionKind::BorrowAlways:
        llvm_unreachable("Only for SILGenPattern");
      case CastConsumptionKind::CopyOnSuccess:
        locations.setBits(bits, ccabi->getDest());
        break;
      case CastConsumptionKind::TakeOnSuccess:
      case CastConsumptionKind::TakeAlways:
        locations.clearBits(bits, ccabi->getSrc());
        locations.setBits(bits, ccabi->getDest());
        break;
      }
      return;
    }

    switch (ccabi->getConsumptionKind()) {
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("Only for SILGenPattern");
    case CastConsumptionKind::CopyOnSuccess:
    case CastConsumptionKind::TakeOnSuccess:
      break;
    case CastConsumptionKind::TakeAlways:
      locations.clearBits(bits, ccabi->getSrc());
      break;
    }
    return;
  }

  auto *TAI = dyn_cast<TryApplyInst>(pred->getTerminator());

  // @out results of try_apply are only valid in the normal-block, but not in
  // the throw-block.
  if (!TAI || TAI->getNormalBB() != block)
    return;

  FullApplySite FAS(TAI);
  for (Operand &op : TAI->getAllOperands()) {
    if (FAS.isArgumentOperand(op) &&
        FAS.getArgumentConvention(op) == SILArgumentConvention::Indirect_Out) {
      locations.setBits(bits, op.get());
    }
  }
}

void MemoryLifetimeVerifier::setFuncOperandBits(BlockState &state, Operand &op,
                                        SILArgumentConvention convention,
                                        bool isTryApply) {
  switch (convention) {
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_In_Constant:
      state.killBits(op.get(), locations);
      break;
    case SILArgumentConvention::Indirect_Out:
      // try_apply is special, because an @out result is only initialized
      // in the normal-block, but not in the throw-block.
      // We handle @out result of try_apply in setBitsOfPredecessor.
      if (!isTryApply)
        state.genBits(op.get(), locations);
      break;
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
      break;
  }
}

void MemoryLifetimeVerifier::checkFunction(MemoryDataflow &dataFlow) {

  // Collect the bits which we require to be set at function exits.
  Bits expectedReturnBits(locations.getNumLocations());
  Bits expectedThrowBits(locations.getNumLocations());
  for (SILArgument *arg : function->getArguments()) {
    SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
    switch (funcArg->getArgumentConvention()) {
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_In_Guaranteed:
      locations.setBits(expectedReturnBits, funcArg);
      locations.setBits(expectedThrowBits, funcArg);
      break;
    case SILArgumentConvention::Indirect_Out:
      locations.setBits(expectedReturnBits, funcArg);
      break;
    default:
      break;
    }
  }

  const Bits &nonTrivialLocations = locations.getNonTrivialLocations();
  Bits bits(locations.getNumLocations());
  for (BlockState &st : dataFlow) {
    if (!st.reachableFromEntry || !st.exitReachable())
      continue;

    // Check all instructions in the block.
    bits = st.entrySet;
    checkBlock(st.block, bits);

    // Check if there is a mismatch in location lifetime at the merge point.
    for (SILBasicBlock *pred : st.block->getPredecessorBlocks()) {
      BlockState *predState = dataFlow.getState(pred);
      if (predState->reachableFromEntry) {
        require((st.entrySet ^ predState->exitSet) & nonTrivialLocations,
          "lifetime mismatch in predecessors", &*st.block->begin());
      }
    }

    // Check the bits at function exit.
    TermInst *term = st.block->getTerminator();
    assert(bits == st.exitSet || isa<TryApplyInst>(term));
    switch (term->getKind()) {
      case SILInstructionKind::ReturnInst:
      case SILInstructionKind::UnwindInst:
        require(expectedReturnBits & ~st.exitSet,
          "indirect argument is not alive at function return", term);
        require(st.exitSet & ~expectedReturnBits & nonTrivialLocations,
          "memory is initialized at function return but shouldn't", term);
        break;
      case SILInstructionKind::ThrowInst:
        require(expectedThrowBits & ~st.exitSet,
          "indirect argument is not alive at throw", term);
        require(st.exitSet & ~expectedThrowBits & nonTrivialLocations,
          "memory is initialized at throw but shouldn't", term);
        break;
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::checkBlock(SILBasicBlock *block, Bits &bits) {
  setBitsOfPredecessor(bits, block);
  const Bits &nonTrivialLocations = locations.getNonTrivialLocations();

  for (SILInstruction &I : *block) {
    switch (I.getKind()) {
      case SILInstructionKind::LoadInst: {
        auto *LI = cast<LoadInst>(&I);
        requireBitsSet(bits, LI->getOperand(), &I);
        switch (LI->getOwnershipQualifier()) {
          case LoadOwnershipQualifier::Take:
            locations.clearBits(bits, LI->getOperand());
            break;
          case LoadOwnershipQualifier::Copy:
          case LoadOwnershipQualifier::Trivial:
            break;
          case LoadOwnershipQualifier::Unqualified:
            llvm_unreachable("unqualified load shouldn't be in ownership SIL");
        }
        break;
      }
      case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
        auto *uccai = cast<UnconditionalCheckedCastAddrInst>(&I);
        requireBitsSet(bits, uccai->getSrc(), &I);
        locations.clearBits(bits, uccai->getSrc());
        requireBitsClear(bits, uccai->getDest(), &I);
        locations.setBits(bits, uccai->getDest());
        break;
      }
      case SILInstructionKind::UncheckedRefCastAddrInst: {
        auto *uccai = cast<UncheckedRefCastAddrInst>(&I);
        requireBitsSet(bits, uccai->getSrc(), &I);
        locations.clearBits(bits, uccai->getSrc());
        requireBitsClear(bits, uccai->getDest(), &I);
        locations.setBits(bits, uccai->getDest());
        break;
      }
      case SILInstructionKind::CheckedCastAddrBranchInst: {
        auto *ccbai = cast<CheckedCastAddrBranchInst>(&I);
        requireBitsSet(bits, ccbai->getSrc(), &I);
        requireBitsClear(bits, ccbai->getDest(), &I);
        break;
      }
      case SILInstructionKind::StoreInst: {
        auto *SI = cast<StoreInst>(&I);
        switch (SI->getOwnershipQualifier()) {
          case StoreOwnershipQualifier::Init:
            requireBitsClear(bits & nonTrivialLocations, SI->getDest(), &I);
            locations.setBits(bits, SI->getDest());
            break;
          case StoreOwnershipQualifier::Assign:
            requireBitsSet(bits | ~nonTrivialLocations, SI->getDest(), &I);
            break;
          case StoreOwnershipQualifier::Trivial:
            locations.setBits(bits, SI->getDest());
            break;
          case StoreOwnershipQualifier::Unqualified:
            llvm_unreachable("unqualified store shouldn't be in ownership SIL");
        }
        break;
      }
      case SILInstructionKind::CopyAddrInst: {
        auto *CAI = cast<CopyAddrInst>(&I);
        requireBitsSet(bits, CAI->getSrc(), &I);
        if (CAI->isTakeOfSrc())
          locations.clearBits(bits, CAI->getSrc());
        if (CAI->isInitializationOfDest()) {
          requireBitsClear(bits & nonTrivialLocations, CAI->getDest(), &I);
        } else {
          requireBitsSet(bits | ~nonTrivialLocations, CAI->getDest(), &I);
        }
        locations.setBits(bits, CAI->getDest());
        break;
      }
      case SILInstructionKind::DestroyAddrInst: {
        SILValue opVal = cast<DestroyAddrInst>(&I)->getOperand();
        requireBitsSet(bits | ~nonTrivialLocations, opVal, &I);
        locations.clearBits(bits, opVal);
        break;
      }
      case SILInstructionKind::EndBorrowInst: {
        if (SILValue orig = cast<EndBorrowInst>(&I)->getSingleOriginalValue())
          requireBitsSet(bits, orig, &I);
        break;
      }
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst: {
        FullApplySite FAS(&I);
        for (Operand &op : I.getAllOperands()) {
          if (FAS.isArgumentOperand(op))
            checkFuncArgument(bits, op, FAS.getArgumentConvention(op), &I);
        }
        break;
      }
      case SILInstructionKind::YieldInst: {
        auto *YI = cast<YieldInst>(&I);
        for (Operand &op : YI->getAllOperands()) {
          checkFuncArgument(bits, op, YI->getArgumentConventionForOperand(op),
                             &I);
        }
        break;
      }
      case SILInstructionKind::DebugValueAddrInst:
        requireBitsSet(bits, cast<DebugValueAddrInst>(&I)->getOperand(), &I);
        break;
      case SILInstructionKind::DeallocStackInst: {
        SILValue opVal = cast<DeallocStackInst>(&I)->getOperand();
        requireBitsClear(bits & nonTrivialLocations, opVal, &I);
        // Needed to clear any bits of trivial locations (which are not required
        // to be zero).
        locations.clearBits(bits, opVal);
        break;
      }
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::checkFuncArgument(Bits &bits, Operand &argumentOp,
                         SILArgumentConvention argumentConvention,
                         SILInstruction *applyInst) {
  switch (argumentConvention) {
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_In_Constant:
      requireBitsSet(bits, argumentOp.get(), applyInst);
      locations.clearBits(bits, argumentOp.get());
      break;
    case SILArgumentConvention::Indirect_Out:
      requireBitsClear(bits & locations.getNonTrivialLocations(),
                       argumentOp.get(), applyInst);
      locations.setBits(bits, argumentOp.get());
      break;
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
      requireBitsSet(bits, argumentOp.get(), applyInst);
      break;
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
      break;
  }
}

void MemoryLifetimeVerifier::verify() {
  // First step: handle memory locations which (potentially) span multiple
  // blocks.
  locations.analyzeLocations(function);
  if (locations.getNumLocations() > 0) {
    MemoryDataflow dataFlow(function, locations.getNumLocations());
    dataFlow.entryReachabilityAnalysis();
    dataFlow.exitReachableAnalysis();
    initDataflow(dataFlow);
    dataFlow.solveForwardWithIntersect();
    checkFunction(dataFlow);
  }
  // Second step: handle single-block locations.
  locations.handleSingleBlockLocations([this](SILBasicBlock *block) {
    Bits bits(locations.getNumLocations());
    checkBlock(block, bits);
  });
}

void swift::verifyMemoryLifetime(SILFunction *function) {
  MemoryLifetimeVerifier verifier(function);
  verifier.verify();
}

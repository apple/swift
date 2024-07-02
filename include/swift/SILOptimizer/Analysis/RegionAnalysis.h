//===--- RegionAnalysis.h -------------------------------------------------===//
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

#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_REGIONANALYSIS_H
#define SWIFT_SILOPTIMIZER_ANALYSIS_REGIONANALYSIS_H

#include "swift/SIL/BasicBlockData.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"

#include <optional>
#include <variant>

namespace swift {

class RegionAnalysisFunctionInfo;
class RegionAnalysisValueMap;

namespace regionanalysisimpl {

#ifndef NDEBUG
/// Global bool set only when asserts are enabled to ease debugging by causing
/// unknown pattern errors to cause an assert so we drop into the debugger.
extern bool AbortOnUnknownPatternMatchError;
#endif

using TransferringOperandSetFactory = Partition::TransferringOperandSetFactory;
using Element = PartitionPrimitives::Element;
using Region = PartitionPrimitives::Region;

/// Return the ApplyIsolationCrossing for a specific \p inst if it
/// exists. Returns std::nullopt otherwise.
std::optional<ApplyIsolationCrossing>
getApplyIsolationCrossing(SILInstruction *inst);

// This is our PImpl type that we use to hide all of the internal details of
// the computation.
class PartitionOpTranslator;

class BlockPartitionState {
  friend RegionAnalysisFunctionInfo;

  /// Set if this block in the next iteration needs to be visited.
  bool needsUpdate = false;

  /// Set if this block is live.
  ///
  /// If the block is not live, then we shouldnt try to emit diagnostics for it
  /// since we will not have performed dataflow upon it.
  bool isLive = false;

  /// The partition of elements into regions at the top of the block.
  Partition entryPartition;

  /// The partition of elements into regions at the bottom of the block.
  Partition exitPartition;

  /// The basic block that this state belongs to.
  SILBasicBlock *basicBlock;

  /// The vector of PartitionOps that are used to perform the dataflow in this
  /// block.
  std::vector<PartitionOp> blockPartitionOps = {};

  TransferringOperandSetFactory &ptrSetFactory;

  TransferringOperandToStateMap &transferringOpToStateMap;

  BlockPartitionState(SILBasicBlock *basicBlock,
                      PartitionOpTranslator &translator,
                      TransferringOperandSetFactory &ptrSetFactory,
                      IsolationHistory::Factory &isolationHistoryFactory,
                      TransferringOperandToStateMap &transferringOpToStateMap);

public:
  bool getLiveness() const { return isLive; }

  ArrayRef<PartitionOp> getPartitionOps() const { return blockPartitionOps; }

  const Partition &getEntryPartition() const { return entryPartition; }

  const Partition &getExitPartition() const { return exitPartition; }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const;

private:
  /// Recomputes the exit partition from the entry partition, and returns
  /// whether this changed the exit partition.
  ///
  /// NOTE: This method ignored errors that arise. We process separately later
  /// to discover if an error occured.
  bool recomputeExitFromEntry(PartitionOpTranslator &translator);
};

class TrackableValue;
class TrackableValueState;
class RepresentativeValue;

enum class TrackableValueFlag {
  /// Base value that says a value is uniquely represented and is
  /// non-sendable. Example: an alloc_stack of a non-Sendable type that isn't
  /// captured by a closure.
  None = 0x0,

  /// Set to true if this TrackableValue's representative is not uniquely
  /// represented so may have aliases. Example: a value that isn't an
  /// alloc_stack.
  isMayAlias = 0x1,

  /// Set to true if this TrackableValue's representative is Sendable.
  isSendable = 0x2,
};

using TrackedValueFlagSet = OptionSet<TrackableValueFlag>;

} // namespace regionanalysisimpl

class regionanalysisimpl::TrackableValueState {
  friend RegionAnalysisValueMap;

  unsigned id;
  TrackedValueFlagSet flagSet = {TrackableValueFlag::isMayAlias};
  std::optional<SILIsolationInfo> regionInfo = {};

public:
  TrackableValueState(unsigned newID) : id(newID) {}

  bool isMayAlias() const {
    return flagSet.contains(TrackableValueFlag::isMayAlias);
  }

  bool isNoAlias() const { return !isMayAlias(); }

  bool isSendable() const {
    return flagSet.contains(TrackableValueFlag::isSendable);
  }

  bool isNonSendable() const { return !isSendable(); }

  SILIsolationInfo getIsolationRegionInfo() const {
    if (!regionInfo) {
      return SILIsolationInfo::getDisconnected(false);
    }

    return *regionInfo;
  }

  SILIsolationInfo::Kind getIsolationRegionInfoKind() const {
    return getIsolationRegionInfo().getKind();
  }

  ActorIsolation getActorIsolation() const {
    return getIsolationRegionInfo().getActorIsolation();
  }

  void setDisconnectedNonisolatedUnsafe() {
    auto oldRegionInfo = getIsolationRegionInfo();
    assert(oldRegionInfo.isDisconnected());
    regionInfo = oldRegionInfo.withUnsafeNonIsolated();
  }

  Element getID() const { return Element(id); }

  void addFlag(TrackableValueFlag flag) { flagSet |= flag; }

  void removeFlag(TrackableValueFlag flag) { flagSet -= flag; }

  void print(llvm::raw_ostream &os) const {
    os << "TrackableValueState[id: " << id
       << "][is_no_alias: " << (isNoAlias() ? "yes" : "no")
       << "][is_sendable: " << (isSendable() ? "yes" : "no")
       << "][region_value_kind: ";
    getIsolationRegionInfo().printForOneLineLogging(os);
    os << "].";
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  bool hasIsolationRegionInfo() const { return bool(regionInfo); }

  /// Set the isolation region info for this TrackableValueState. Private so it
  /// can only be used by RegionAnalysisValueMap::getTrackableValue.
  void setIsolationRegionInfo(SILIsolationInfo newRegionInfo) {
    assert(!regionInfo.has_value() &&
           "Can only call setIsolationRegionInfo once!\n");
    regionInfo = newRegionInfo;
  }
};

/// The representative value of the equivalence class that makes up a tracked
/// value.
///
/// We use a wrapper struct here so that we can inject "fake" actor isolated
/// values into the regions of values that become merged into an actor by
/// calling a function without a non-sendable result.
class regionanalysisimpl::RepresentativeValue {
  friend llvm::DenseMapInfo<RepresentativeValue>;

  using InnerType = PointerUnion<SILValue, SILInstruction *>;

  /// If this is set to a SILValue then it is the actual represented value. If
  /// it is set to a SILInstruction, then this is a "fake" representative value
  /// used to inject actor isolatedness. The instruction stored is the
  /// instruction that introduced the actor isolated-ness.
  InnerType value;

public:
  RepresentativeValue() : value() {}
  RepresentativeValue(SILValue value) : value(value) {}
  RepresentativeValue(SILInstruction *actorRegionInst)
      : value(actorRegionInst) {}

  operator bool() const { return bool(value); }

  void print(llvm::raw_ostream &os) const {
    if (auto *inst = value.dyn_cast<SILInstruction *>()) {
      os << "ActorRegionIntroducingInst: " << *inst;
      return;
    }

    os << *value.get<SILValue>();
  }

  SILValue getValue() const { return value.get<SILValue>(); }
  SILValue maybeGetValue() const { return value.dyn_cast<SILValue>(); }
  bool hasRegionIntroducingInst() const { return value.is<SILInstruction *>(); }
  SILInstruction *getActorRegionIntroducingInst() const {
    return value.get<SILInstruction *>();
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  RepresentativeValue(InnerType value) : value(value) {}
};

/// A tuple consisting of a base value and its value state.
///
/// DISCUSSION: We are computing regions among equivalence classes of values
/// with GEPs like struct_element_addr being considered equivalent from a value
/// perspective to their underlying base value.
///
/// Example:
///
/// ```
/// %0 = alloc_stack $Struct
/// %1 = struct_element_addr %0 : $Struct.childField
/// %2 = struct_element_addr %1 : $ChildField.grandchildField
/// ```
///
/// In the above example, %2 will be mapped to %0 by our value mapping.
class regionanalysisimpl::TrackableValue {
  RepresentativeValue representativeValue;
  TrackableValueState valueState;

public:
  TrackableValue(RepresentativeValue representativeValue,
                 TrackableValueState valueState)
      : representativeValue(representativeValue), valueState(valueState) {}

  bool isMayAlias() const { return valueState.isMayAlias(); }

  bool isNoAlias() const { return !isMayAlias(); }

  bool isSendable() const { return valueState.isSendable(); }

  bool isNonSendable() const { return !isSendable(); }

  SILIsolationInfo getIsolationRegionInfo() const {
    return valueState.getIsolationRegionInfo();
  }

  Element getID() const { return Element(valueState.getID()); }

  /// Return the representative value of this equivalence class of values.
  RepresentativeValue getRepresentative() const { return representativeValue; }

  TrackableValueState getValueState() const { return valueState; }

  /// Returns true if this TrackableValue is an alloc_stack from a sending
  /// parameter.
  bool isSendingParameter() const;

  void printIsolationInfo(SmallString<64> &outString) const {
    llvm::raw_svector_ostream os(outString);
    getIsolationRegionInfo().printForDiagnostics(os);
  }

  void print(llvm::raw_ostream &os) const {
    os << "TrackableValue. State: ";
    valueState.print(os);
    os << "\n    Rep Value: ";
    getRepresentative().print(os);
  }

  void printVerbose(llvm::raw_ostream &os) const {
    os << "TrackableValue. State: ";
    valueState.print(os);
    os << "\n    Rep Value: " << getRepresentative();
  }

  SWIFT_DEBUG_DUMP {
    print(llvm::dbgs());
    llvm::dbgs() << '\n';
  }
};

class RegionAnalysis;

class RegionAnalysisValueMap {
  friend regionanalysisimpl::PartitionOpTranslator;

public:
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TrackableValue = regionanalysisimpl::TrackableValue;
  using TrackableValueState = regionanalysisimpl::TrackableValueState;
  using RepresentativeValue = regionanalysisimpl::RepresentativeValue;

private:
  /// A map from the representative of an equivalence class of values to their
  /// TrackableValueState. The state contains both the unique value id for the
  /// equivalence class of values as well as whether we determined if they are
  /// uniquely identified and sendable.
  ///
  /// nodeIDMap stores unique IDs for all SILNodes corresponding to
  /// non-Sendable values. Implicit conversion from SILValue used pervasively.
  /// ensure getUnderlyingTrackedValue is called on SILValues before entering
  /// into this map
  llvm::DenseMap<RepresentativeValue, TrackableValueState>
      equivalenceClassValuesToState;
  llvm::DenseMap<unsigned, RepresentativeValue> stateIndexToEquivalenceClass;

  SILFunction *fn;

public:
  RegionAnalysisValueMap(SILFunction *fn) : fn(fn) { }

  /// Returns the value for this instruction if it isn't a fake "represenative
  /// value" to inject actor isolatedness. Asserts in such a case.
  SILValue getRepresentative(Element trackableValueID) const;

  /// Returns the value for this instruction. If it is a fake "representative
  /// value" returns an empty SILValue.
  SILValue maybeGetRepresentative(Element trackableValueID) const;

  /// Returns the fake "representative value" for this element if it
  /// exists. Returns nullptr otherwise.
  SILInstruction *maybeGetActorIntroducingInst(Element trackableValueID) const;

  SILIsolationInfo getIsolationRegion(Element trackableValueID) const;
  SILIsolationInfo getIsolationRegion(SILValue trackableValueID) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  TrackableValue
  getTrackableValue(SILValue value,
                    bool isAddressCapturedByPartialApply = false) const;

  /// An actor introducing inst is an instruction that doesn't have any
  /// non-Sendable parameters and produces a new value that has to be actor
  /// isolated.
  ///
  /// This is just for looking up the ValueIsolationRegionInfo for a
  /// instructionInst if we have one. So it is a find like function.
  std::optional<TrackableValue> getTrackableValueForActorIntroducingInst(
      SILInstruction *introducingInst) const;

private:
  std::optional<TrackableValue> getValueForId(Element id) const;
  std::optional<TrackableValue> tryToTrackValue(SILValue value) const;
  TrackableValue
  getActorIntroducingRepresentative(SILInstruction *introducingInst,
                                    SILIsolationInfo isolation) const;
  bool valueHasID(SILValue value, bool dumpIfHasNoID = false);
  Element lookupValueID(SILValue value);

  /// Initialize a TrackableValue with a SILIsolationInfo that we already know
  /// instead of inferring.
  ///
  /// If we successfully initialize \p value with \p info, returns
  /// {TrackableValue(), true}. If we already had a TrackableValue, we return
  /// {TrackableValue(), false}.
  std::pair<TrackableValue, bool>
  initializeTrackableValue(SILValue value, SILIsolationInfo info) const;
};

class RegionAnalysisFunctionInfo {
  using BlockPartitionState = regionanalysisimpl::BlockPartitionState;
  using PartitionOpTranslator = regionanalysisimpl::PartitionOpTranslator;
  using TransferringOperandSetFactory =
      regionanalysisimpl::TransferringOperandSetFactory;
  using BasicBlockData = BasicBlockData<BlockPartitionState>;

  llvm::BumpPtrAllocator allocator;

  SILFunction *fn;

  RegionAnalysisValueMap valueMap;

  // This is treated as a lazy pimpl. We allocate it using the bump ptr
  // allocator when we allocate everything.
  PartitionOpTranslator *translator;

  TransferringOperandSetFactory ptrSetFactory;

  IsolationHistory::Factory isolationHistoryFactory;

  TransferringOperandToStateMap transferringOpToStateMap;

  // We make this optional to prevent an issue that we have seen on windows when
  // capturing a field in a closure that is used to initialize a different
  // field.
  std::optional<BasicBlockData> blockStates;

  PostOrderFunctionInfo *pofi;

  /// Set to true if we have already processed our regions.
  bool solved;

  /// Set to true if this is a function that we know how to process regions for.
  ///
  /// DISCUSSION: We do not support if the correct features are not enabled, if
  /// the function doesn't have a parent module, or if the function doesn't have
  /// ownership.
  bool supportedFunction;

public:
  using LazyType = LazyFunctionInfo<RegionAnalysis, RegionAnalysisFunctionInfo>;

  RegionAnalysisFunctionInfo(SILFunction *fn, PostOrderFunctionInfo *pofi);
  ~RegionAnalysisFunctionInfo();

  BlockPartitionState &getPartitionState(SILBasicBlock *block) const {
    assert(supportedFunction &&
           "Cannot getPartitionState for a non-supported function");
    // Lazily run the dataflow.
    if (!solved)
      const_cast<RegionAnalysisFunctionInfo *>(this)->runDataflow();
    return *blockStates->get(block).get();
  }

  SILFunction *getFunction() const { return fn; }

  bool isSupportedFunction() const { return supportedFunction; }

  using iterator = BasicBlockData::iterator;
  using const_iterator = BasicBlockData::const_iterator;
  using reverse_iterator = BasicBlockData::reverse_iterator;
  using const_reverse_iterator = BasicBlockData::const_reverse_iterator;

  iterator begin() {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->begin();
  }
  iterator end() {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->end();
  }
  const_iterator begin() const {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->begin();
  }
  const_iterator end() const {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->end();
  }

  reverse_iterator rbegin() {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->rbegin();
  }
  reverse_iterator rend() {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->rend();
  }
  const_reverse_iterator rbegin() const {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->rbegin();
  }
  const_reverse_iterator rend() const {
    assert(supportedFunction && "Unsupported Function?!");
    return blockStates->rend();
  }

  using range = llvm::iterator_range<iterator>;
  using const_range = llvm::iterator_range<const_iterator>;
  using reverse_range = llvm::iterator_range<reverse_iterator>;
  using const_reverse_range = llvm::iterator_range<const_reverse_iterator>;

  range getRange() { return {begin(), end()}; }
  const_range getRange() const { return {begin(), end()}; }
  reverse_range getReverseRange() { return {rbegin(), rend()}; }
  const_reverse_range getReverseRange() const { return {rbegin(), rend()}; }

  TransferringOperandSetFactory &getOperandSetFactory() {
    assert(supportedFunction && "Unsupported Function?!");
    return ptrSetFactory;
  }

  RegionAnalysisValueMap &getValueMap() {
    assert(supportedFunction && "Unsupported Function?!");
    return valueMap;
  }

  IsolationHistory::Factory &getIsolationHistoryFactory() {
    assert(supportedFunction && "Unsupported Function?!");
    return isolationHistoryFactory;
  }

  TransferringOperandToStateMap &getTransferringOpToStateMap() {
    assert(supportedFunction && "Unsupported Function?!");
    return transferringOpToStateMap;
  }

  bool isClosureCaptured(SILValue value, Operand *op);

  static SILValue getUnderlyingTrackedValue(SILValue value);

private:
  void runDataflow();
};

class RegionAnalysis final
    : public FunctionAnalysisBase<RegionAnalysisFunctionInfo> {
  PostOrderAnalysis *poa;

public:
  RegionAnalysis()
      : FunctionAnalysisBase<RegionAnalysisFunctionInfo>(
            SILAnalysisKind::Region) {}

  RegionAnalysis(const RegionAnalysis &) = delete;
  RegionAnalysis &operator=(const RegionAnalysis &) = delete;

  static SILAnalysisKind getAnalysisKind() { return SILAnalysisKind::Region; }

  static bool classof(const SILAnalysis *s) {
    return s->getKind() == SILAnalysisKind::Region;
  }

  virtual void initialize(SILPassManager *PM) override;

  std::unique_ptr<RegionAnalysisFunctionInfo>
  newFunctionAnalysis(SILFunction *f) override {
    return std::make_unique<RegionAnalysisFunctionInfo>(f, poa->get(f));
  }

  bool shouldInvalidate(SILAnalysis::InvalidationKind k) override {
    // Invalidate if we invalidate anything.
    return k & InvalidationKind::Everything;
  }
};

} // namespace swift

namespace llvm {

inline llvm::raw_ostream &
operator<<(llvm::raw_ostream &os,
           const swift::regionanalysisimpl::RepresentativeValue &value) {
  value.print(os);
  return os;
}

template <>
struct DenseMapInfo<swift::regionanalysisimpl::RepresentativeValue> {
  using RepresentativeValue = swift::regionanalysisimpl::RepresentativeValue;
  using InnerType = RepresentativeValue::InnerType;
  using InnerDenseMapInfo = DenseMapInfo<InnerType>;

  static RepresentativeValue getEmptyKey() {
    return RepresentativeValue(InnerDenseMapInfo::getEmptyKey());
  }
  static RepresentativeValue getTombstoneKey() {
    return RepresentativeValue(InnerDenseMapInfo::getTombstoneKey());
  }

  static unsigned getHashValue(RepresentativeValue value) {
    return InnerDenseMapInfo::getHashValue(value.value);
  }

  static bool isEqual(RepresentativeValue LHS, RepresentativeValue RHS) {
    return InnerDenseMapInfo::isEqual(LHS.value, RHS.value);
  }
};

} // namespace llvm

#endif

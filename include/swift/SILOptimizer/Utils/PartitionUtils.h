//===--- PartitionUtils.h -------------------------------------------------===//
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

#ifndef SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H

#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILIsolationInfo.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <variant>

#define DEBUG_TYPE "transfer-non-sendable"

namespace swift {

namespace PartitionPrimitives {

#ifndef NDEBUG
extern bool REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING;
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)                                  \
  do {                                                                         \
    if (PartitionPrimitives::REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING) {    \
      LLVM_DEBUG(__VA_ARGS__);                                                 \
    }                                                                          \
  } while (0);
#else
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)
#endif

struct Element {
  unsigned num;

  explicit Element(int num) : num(num) {}

  bool operator==(const Element &other) const { return num == other.num; }
  bool operator<(const Element &other) const { return num < other.num; }

  operator unsigned() const { return num; }
};

struct Region {
  unsigned num;

  explicit Region(unsigned num) : num(num) {}

  bool operator==(const Region &other) const { return num == other.num; }
  bool operator<(const Region &other) const { return num < other.num; }

  operator unsigned() const { return num; }
};

} // namespace PartitionPrimitives

} // namespace swift

namespace llvm {

template <>
struct DenseMapInfo<swift::PartitionPrimitives::Region> {
  using Region = swift::PartitionPrimitives::Region;

  static Region getEmptyKey() {
    return Region(DenseMapInfo<unsigned>::getEmptyKey());
  }
  static Region getTombstoneKey() {
    return Region(DenseMapInfo<unsigned>::getTombstoneKey());
  }

  static unsigned getHashValue(Region region) {
    return DenseMapInfo<unsigned>::getHashValue(region);
  }
  static bool isEqual(Region LHS, Region RHS) { return LHS == RHS; }
};

} // namespace llvm

namespace swift {

class Partition;
class TransferringOperandToStateMap;

/// A persistent data structure that is used to "rewind" partition history so
/// that we can discover when values become part of the same region.
///
/// NOTE: This does not track whether or not values are transferred. This is
/// because from the perspective of determining when two values become part of
/// the same region, that information is not important. To unroll history, a
/// Partition must have no transfers to use this. NOTE: There is a method that
/// takes a Partition and produces a new Partition that does not have any
/// transfers.
class IsolationHistory {
public:
  class Factory;

private:
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  class Node;

  // TODO: This shouldn't need to be a friend.
  friend class Partition;
  friend TransferringOperandToStateMap;

  /// First node in the immutable linked list.
  Node *head = nullptr;
  Factory *factory = nullptr;

  IsolationHistory(Factory *factory) : head(nullptr), factory(factory) {}

public:
  IsolationHistory(const IsolationHistory &otherIsolation)
      : head(otherIsolation.head), factory(otherIsolation.factory) {}

  IsolationHistory &operator=(const IsolationHistory &otherIsolation) {
    assert(factory == otherIsolation.factory);
    head = otherIsolation.head;
    return *this;
  }

  Node *getHead() const { return head; }

  /// Push a node that signals the end of a new sequence of history nodes that
  /// should execute together. Must be explicitly ended by a push sequence
  /// end. Is non-rentrant, so one cannot have multiple sequence starts.
  ///
  /// \p loc the SILLocation that identifies the instruction that the "package"
  /// of history nodes that this sequence boundary ends is associated with.
  Node *pushHistorySequenceBoundary(SILLocation loc);

  /// Push onto the history list that \p value should be added into its own
  /// independent region.
  Node *pushNewElementRegion(Element element);

  /// Push onto the history that \p value should be removed from a region and
  /// that element is the last element in that region (so the region is empty
  /// afterwards).
  void pushRemoveLastElementFromRegion(Element element);

  /// Push onto the history that \p element should be removed from a region that
  /// contains \p otherElementInOldRegion.
  void pushRemoveElementFromRegion(Element otherElementInOldRegion,
                                   Element element);

  /// \p elementToMergeInto is the element whose region we merge \p otherRegions
  /// into.
  void pushMergeElementRegions(Element elementToMergeInto,
                               ArrayRef<Element> otherRegions);

  /// Assign \p elementToMerge's region to \p elementToMergeInto's region.
  void pushAssignElementRegions(Element elementToMergeInto,
                                Element elementToMerge);

  /// Push that \p other should be merged into this region.
  void pushCFGHistoryJoin(Node *otherNode);

  /// Push the top node of \p history as a CFG history join.
  void pushCFGHistoryJoin(IsolationHistory history) {
    return pushCFGHistoryJoin(history.getHead());
  }

  Node *pop();
};

class IsolationHistory::Node final
    : private llvm::TrailingObjects<IsolationHistory::Node, Element> {
  friend IsolationHistory;
  friend TrailingObjects;

public:
  enum Kind {
    /// Add a new element to its own region. The region will only consist of
    /// element.
    AddNewRegionForElement,

    /// Remove an element from a region which it is the only element of.
    RemoveLastElementFromRegion,

    /// Remove an element from a region which still has elements remaining.
    ///
    /// This is different from RemoveLastElementFromRegion since we store the
    /// other element.
    RemoveElementFromRegion,

    /// Given two elements, data and otherData, merge otherData into data's
    /// region.
    MergeElementRegions,

    /// At a CFG merge point, we merged two histories. We need to visit it
    /// recursively.
    CFGHistoryJoin,

    /// Signals that a sequence boundary has been found in the history and if we
    /// are processing a sequence, should stop processing.
    ///
    /// Clients may want to ensure that a set of history elements are pushed or
    /// popped together since the effects happen at the same time.
    /// HistorySequenceStart
    /// signifies that.
    SequenceBoundary,
  };

private:
  Kind kind;
  Node *parent;

  /// Child node. Never set on construction.
  Node *child = nullptr;

  /// Contains:
  ///
  /// 1. Node * if we have a CFGHistoryJoin.
  /// 2. A SILLocation if we have a SequenceBoundary.
  /// 3. An element otherwise.
  std::variant<Element, Node *, SILLocation> subject;

  /// Number of additional element arguments stored in the tail allocated array.
  unsigned numAdditionalElements;

  /// Access the tail allocated buffer of additional element arguments.
  MutableArrayRef<Element> getAdditionalElementArgs() {
    return {getTrailingObjects<Element>(), numAdditionalElements};
  }

  Node(Kind kind, Node *parent)
      : kind(kind), parent(parent), subject(nullptr) {}
  Node(Kind kind, Node *parent, SILLocation loc)
      : kind(kind), parent(parent), subject(loc) {}
  Node(Kind kind, Node *parent, Element value)
      : kind(kind), parent(parent), subject(value), numAdditionalElements(0) {}
  Node(Kind kind, Node *parent, Element primaryElement,
       std::initializer_list<Element> restOfTheElements)
      : kind(kind), parent(parent), subject(primaryElement),
        numAdditionalElements(restOfTheElements.size()) {
    unsigned writeIndex = 0;
    for (Element restElt : restOfTheElements) {
      if (primaryElement == restElt) {
        continue;
      }

      getAdditionalElementArgs()[writeIndex] = restElt;
      ++writeIndex;
    }

    // Set writeIndex to n - 1.
    numAdditionalElements = writeIndex;
  }

  Node(Kind kind, Node *parent, Element lhsValue, ArrayRef<Element> rhsValue)
      : kind(kind), parent(parent), subject(lhsValue),
        numAdditionalElements(rhsValue.size()) {
    std::uninitialized_copy(rhsValue.begin(), rhsValue.end(),
                            getAdditionalElementArgs().data());
  }

  Node(Kind kind, Node *parent, Node *node)
      : kind(kind), parent(parent), subject(node), numAdditionalElements(0) {}

public:
  Kind getKind() const { return kind; }

  Node *getParent() const { return parent; }

  Node *getChild() const { return child; }
  void setChild(Node *newChild) { child = newChild; }

  Element getFirstArgAsElement() const {
    assert(kind != CFGHistoryJoin);
    assert(std::holds_alternative<Element>(subject));
    return std::get<Element>(subject);
  }

  Node *getFirstArgAsNode() const {
    assert(kind == CFGHistoryJoin);
    assert(std::holds_alternative<Node *>(subject));
    return std::get<Node *>(subject);
  }

  ArrayRef<Element> getAdditionalElementArgs() const {
    assert(kind == MergeElementRegions || kind == RemoveElementFromRegion);
    return const_cast<Node *>(this)->getAdditionalElementArgs();
  }

  bool isHistorySequenceBoundary() const {
    return getKind() == SequenceBoundary;
  }

  /// If this node is a history sequence join, return its node. Otherwise,
  /// return nullptr.
  Node *getHistorySequenceJoin() const {
    if (kind != CFGHistoryJoin)
      return nullptr;
    return getFirstArgAsNode();
  }

  std::optional<SILLocation> getHistoryBoundaryLoc() const {
    if (kind != SequenceBoundary)
      return {};
    return std::get<SILLocation>(subject);
  }
};

class IsolationHistory::Factory {
  friend IsolationHistory;
  using Node = IsolationHistory::Node;

  llvm::BumpPtrAllocator &allocator;

public:
  Factory(llvm::BumpPtrAllocator &allocator) : allocator(allocator) {}

  Factory(IsolationHistory::Factory &&other) = delete;
  Factory &operator=(IsolationHistory::Factory &&other) = delete;
  Factory(const IsolationHistory::Factory &other) = delete;
  Factory &operator=(const IsolationHistory::Factory &other) = delete;

  /// Returns a new isolation history without any history.
  IsolationHistory get() { return IsolationHistory(this); }
};

struct TransferringOperandState {
  /// The dynamic isolation info of the region of value when we transferred.
  ///
  /// This will contain the isolated value if we found one.
  SILDynamicMergedIsolationInfo isolationInfo;

  /// The dynamic isolation history at this point.
  IsolationHistory isolationHistory;

  /// Set to true if the element associated with the operand's vlaue is closure
  /// captured by the user. In such a case, if our element is a sendable var of
  /// a non-Sendable type, we cannot access it since we could race against an
  /// assignment to the var in a closure.
  bool isClosureCaptured;

  TransferringOperandState(IsolationHistory history)
      : isolationInfo(), isolationHistory(history), isClosureCaptured(false) {}
};

class TransferringOperandToStateMap {
  llvm::SmallDenseMap<Operand *, TransferringOperandState> internalMap;
  IsolationHistory::Factory &isolationHistoryFactory;

public:
  TransferringOperandToStateMap(
      IsolationHistory::Factory &isolationHistoryFactory)
      : isolationHistoryFactory(isolationHistoryFactory) {}
  TransferringOperandState &get(Operand *op) const {
    auto *self = const_cast<TransferringOperandToStateMap *>(this);
    auto history = IsolationHistory(&isolationHistoryFactory);
    return self->internalMap.try_emplace(op, TransferringOperandState(history))
        .first->getSecond();
  }
};

} // namespace swift

namespace swift {

/// PartitionOpKind represents the different kinds of PartitionOps that
/// SILInstructions can be translated to
enum class PartitionOpKind : uint8_t {
  /// Assign one value to the region of another, takes two args, second arg
  /// must already be tracked with a non-transferred region
  Assign,

  /// Assign one value to a fresh region, takes one arg.
  AssignFresh,

  /// Merge the regions of two values, takes two args, both must be from
  /// non-transferred regions.
  Merge,

  /// Transfer the region of a value if not already transferred, takes one arg.
  Transfer,

  /// Due to an async let or something like that a value that was transferred is
  /// no longer transferred.
  UndoTransfer,

  /// Require the region of a value to be non-transferred, takes one arg.
  Require,

  /// Emit an error saying that the given instruction was not understood for
  /// some reason and that a bug should be filed. It expects some sort of
  /// Element number since in most cases we need to define a value for later
  /// potential uses of the value (e.x.: an alloc_stack that we emit an unknown
  /// pattern error will have later uses that will use the value... without
  /// defining the value, the dataflow will assert).
  ///
  /// This is used if we need to reject the program and do not want to assert.
  UnknownPatternError,
};

/// PartitionOp represents a primitive operation that can be performed on
/// Partitions. This is part of the TransferNonSendable SIL pass workflow:
/// first SILBasicBlocks are compiled to vectors of PartitionOps, then a fixed
/// point partition is found over the CFG.
class PartitionOp {
  using Element = PartitionPrimitives::Element;

private:
  PartitionOpKind opKind;
  llvm::SmallVector<Element, 2> opArgs;

  /// Record the SILInstruction that this PartitionOp was generated from, if
  /// generated during compilation from a SILBasicBlock
  PointerUnion<SILInstruction *, Operand *> source;

  // TODO: can the following declarations be merged?
  PartitionOp(PartitionOpKind opKind, Element arg1,
              SILInstruction *sourceInst = nullptr)
      : opKind(opKind), opArgs({arg1}), source(sourceInst) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            sourceInst) &&
           "Transfer needs a sourceInst");
  }

  PartitionOp(PartitionOpKind opKind, Element arg1, Operand *sourceOperand)
      : opKind(opKind), opArgs({arg1}), source(sourceOperand) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            bool(sourceOperand)) &&
           "Transfer needs a sourceInst");
  }

  PartitionOp(PartitionOpKind opKind, Element arg1, Element arg2,
              SILInstruction *sourceInst = nullptr)
      : opKind(opKind), opArgs({arg1, arg2}), source(sourceInst) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            sourceInst) &&
           "Transfer needs a sourceInst");
  }

  PartitionOp(PartitionOpKind opKind, SILInstruction *sourceInst)
      : opKind(opKind), opArgs(), source(sourceInst) {}

  friend class Partition;

public:
  static PartitionOp Assign(Element tgt, Element src,
                            SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Assign, tgt, src, sourceInst);
  }

  static PartitionOp AssignFresh(Element tgt,
                                 SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::AssignFresh, tgt, sourceInst);
  }

  static PartitionOp Transfer(Element tgt, Operand *transferringOp) {
    return PartitionOp(PartitionOpKind::Transfer, tgt, transferringOp);
  }

  static PartitionOp UndoTransfer(Element tgt,
                                  SILInstruction *untransferringInst) {
    return PartitionOp(PartitionOpKind::UndoTransfer, tgt, untransferringInst);
  }

  static PartitionOp Merge(Element tgt1, Element tgt2,
                           SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Merge, tgt1, tgt2, sourceInst);
  }

  static PartitionOp Require(Element tgt,
                             SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Require, tgt, sourceInst);
  }

  static PartitionOp UnknownPatternError(Element elt,
                                         SILInstruction *sourceInst) {
    return PartitionOp(PartitionOpKind::UnknownPatternError, elt, sourceInst);
  }

  bool operator==(const PartitionOp &other) const {
    return opKind == other.opKind && opArgs == other.opArgs &&
           source == other.source;
  };

  bool operator<(const PartitionOp &other) const {
    if (opKind != other.opKind)
      return opKind < other.opKind;
    if (opArgs != other.opArgs)
      return opArgs < other.opArgs;
    return source < other.source;
  }

  PartitionOpKind getKind() const { return opKind; }

  ArrayRef<Element> getOpArgs() const { return opArgs; }

  SILInstruction *getSourceInst() const {
    if (source.is<Operand *>())
      return source.get<Operand *>()->getUser();
    return source.get<SILInstruction *>();
  }

  bool hasSourceInst() const { return source.is<SILInstruction *>(); }

  Operand *getSourceOp() const { return source.get<Operand *>(); }

  SILLocation getSourceLoc() const { return getSourceInst()->getLoc(); }

  void print(llvm::raw_ostream &os, bool extraSpace = false) const;

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

/// A map from Element -> Region that represents the current partition set.
class Partition {
public:
  /// A class defined in PartitionUtils unittest used to grab state from
  /// Partition without exposing it to other users.
  struct PartitionTester;

  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSet = ImmutablePointerSet<Operand *>;
  using TransferringOperandSetFactory = ImmutablePointerSetFactory<Operand *>;
  using IsolationHistoryNode = IsolationHistory::Node;

private:
  /// A map from a region number to a instruction that consumes it.
  ///
  /// All we care is that we ever track a single SILInstruction for a region
  /// since we are fine with emitting a single error per value and letting the
  /// user recompile. If this is an ask for in the future, we can use a true
  /// multi map here. The implication of this is that when we are performing
  /// dataflow we use a union operation to combine CFG elements and just take
  /// the first instruction that we see.
  llvm::SmallMapVector<Region, TransferringOperandSet *, 2>
      regionToTransferredOpMap;

  /// Label each index with a non-negative (unsigned) label if it is associated
  /// with a valid region.
  std::map<Element, Region> elementToRegionMap;

  /// Track a label that is guaranteed to be strictly larger than all in use,
  /// and therefore safe for use as a fresh label.
  Region freshLabel = Region(0);

  /// An immutable data structure that we use to push/pop isolation history.
  IsolationHistory history;

  /// In a canonical partition, all regions are labelled with the smallest index
  /// of any member. Certain operations like join and equals rely on
  /// canonicality so when it's invalidated this boolean tracks that, and it
  /// must be reestablished by a call to canonicalize().
  bool canonical;

public:
  Partition(IsolationHistory history)
      : elementToRegionMap({}), history(history), canonical(true) {}

  /// 1-arg constructor used when canonicality will be immediately invalidated,
  /// so set to false to begin with
  Partition(IsolationHistory history, bool canonical)
      : elementToRegionMap({}), history(history), canonical(canonical) {}

  /// Return a new Partition that has a single region containing the elements of
  /// \p indices.
  static Partition singleRegion(SILLocation loc, ArrayRef<Element> indices,
                                IsolationHistory inputHistory);

  /// Return a new Partition that has each element of \p indices in their own
  /// region.
  static Partition separateRegions(SILLocation loc, ArrayRef<Element> indices,
                                   IsolationHistory inputHistory);

  /// Test two partititons for equality by first putting them in canonical form
  /// then comparing for exact equality.
  ///
  /// Runs in linear time.
  static bool equals(Partition &fst, Partition &snd) {
    fst.canonicalize();
    snd.canonicalize();

    return fst.elementToRegionMap == snd.elementToRegionMap &&
           fst.regionToTransferredOpMap.size() ==
               snd.regionToTransferredOpMap.size() &&
           llvm::all_of(
               fst.regionToTransferredOpMap,
               [&snd](const std::pair<Region, TransferringOperandSet *> &p) {
                 auto sndIter = snd.regionToTransferredOpMap.find(p.first);
                 return sndIter != snd.regionToTransferredOpMap.end() &&
                        sndIter->second == p.second;
               });
  }

  bool isTrackingElement(Element val) const {
    return elementToRegionMap.count(val);
  }

  /// Mark val as transferred.
  void markTransferred(Element val,
                       TransferringOperandSet *transferredOperandSet);

  /// If val was marked as transferred, unmark it as transfer. Returns true if
  /// we found that \p val was transferred. We return false otherwise.
  bool undoTransfer(Element val);

  /// If \p newElt is not being tracked, create a new region for \p newElt. If
  /// \p newElt is already being tracked, remove it from its old region as well.
  ///
  /// \arg updateHistory internal parameter used to determine if we should
  /// update the history. External users shouldn't use this
  void trackNewElement(Element newElt, bool updateHistory = true);

  /// Assigns \p oldElt to the region associated with \p newElt.
  void assignElement(Element oldElt, Element newElt, bool updateHistory = true);

  bool areElementsInSameRegion(Element firstElt, Element secondElt) const {
    return elementToRegionMap.at(firstElt) == elementToRegionMap.at(secondElt);
  }

  Region getRegion(Element elt) const { return elementToRegionMap.at(elt); }

  using iterator = std::map<Element, Region>::iterator;
  iterator begin() { return elementToRegionMap.begin(); }
  iterator end() { return elementToRegionMap.end(); }
  llvm::iterator_range<iterator> range() { return {begin(), end()}; }

  void clearTransferState() { regionToTransferredOpMap.clear(); }

  Partition removingTransferState() const {
    Partition p = *this;
    p.clearTransferState();
    return p;
  }

  /// Rewind one PartitionOp worth of history from the partition.
  ///
  /// If we rewind through a join, the joined isolation history before merging
  /// is inserted into \p foundJoinedHistories which should be processed
  /// afterwards if the current linear history does not find what one is looking
  /// for.
  ///
  /// NOTE: This can only be used if one has cleared transfer state using
  /// Partition::clearTransferState or constructed a new Partiton using
  /// Partition::withoutTransferState(). This is because history rewinding
  /// doesn't use transfer information so just to be careful around potential
  /// invariants being broken, we just require the elimination of the transfer
  /// information.
  ///
  /// \returns true if there is more history that can be popped.
  bool popHistory(SmallVectorImpl<IsolationHistory> &foundJoinedHistories);

  /// Returns true if this value has any isolation history stored.
  bool hasHistory() const { return bool(history.getHead()); }

  /// Returns the number of nodes of stored history.
  ///
  /// NOTE: Do not use this in real code... only intended to be used in testing
  /// code.
  unsigned historySize() const {
    unsigned count = 0;
    auto *head = history.getHead();
    if (!head)
      return count;
    ++count;

    while ((head = head->getParent()))
      ++count;

    return count;
  }

  /// Return a copy of our isolation history.
  IsolationHistory getIsolationHistory() const { return history; }

  /// Construct the partition corresponding to the union of the two passed
  /// partitions.
  ///
  /// NOTE: snd is passed in as mutable since we may canonicalize snd. We will
  /// not perform any further mutations to snd.
  ///
  /// Runs in quadratic time.
  static Partition join(const Partition &fst, Partition &snd);

  /// Return a vector of the transferred values in this partition.
  std::vector<Element> getTransferredVals() const {
    // For effeciency, this could return an iterator not a vector.
    std::vector<Element> transferredVals;
    for (auto [i, _] : elementToRegionMap)
      if (isTransferred(i))
        transferredVals.push_back(i);
    return transferredVals;
  }

  /// Return a vector of the non-transferred regions in this partition, each
  /// represented as a vector of values.
  std::vector<std::vector<Element>> getNonTransferredRegions() const {
    // For effeciency, this could return an iterator not a vector.
    std::map<Region, std::vector<Element>> buckets;

    for (auto [i, label] : elementToRegionMap)
      buckets[label].push_back(i);

    std::vector<std::vector<Element>> doubleVec;

    for (auto [_, bucket] : buckets)
      doubleVec.push_back(bucket);

    return doubleVec;
  }

  void dump_labels() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "Partition";
    if (canonical)
      llvm::dbgs() << "(canonical)";
    llvm::dbgs() << "(fresh=" << freshLabel << "){";
    for (const auto &[i, label] : elementToRegionMap)
      llvm::dbgs() << "[" << i << ": " << label << "] ";
    llvm::dbgs() << "}\n";
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpVerbose()) { printVerbose(llvm::dbgs()); }

  void printVerbose(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpHistory()) { printHistory(llvm::dbgs()); }
  void printHistory(llvm::raw_ostream &os) const;

  /// See docs on \p history.pushHistorySequenceBoundary().
  IsolationHistoryNode *pushHistorySequenceBoundary(SILLocation loc) {
    return history.pushHistorySequenceBoundary(loc);
  }

  bool isTransferred(Element val) const {
    auto iter = elementToRegionMap.find(val);
    if (iter == elementToRegionMap.end())
      return false;
    return regionToTransferredOpMap.count(iter->second);
  }

  /// Return the instruction that transferred \p val's region or nullptr
  /// otherwise.
  TransferringOperandSet *getTransferred(Element val) const {
    auto iter = elementToRegionMap.find(val);
    if (iter == elementToRegionMap.end())
      return nullptr;
    auto iter2 = regionToTransferredOpMap.find(iter->second);
    if (iter2 == regionToTransferredOpMap.end())
      return nullptr;
    auto *set = iter2->second;
    assert(!set->empty());
    return set;
  }

  /// Validate that all regions in the regionToTransferredOpMap exist in the
  /// elementToRegionMap.
  ///
  /// Asserts when NDEBUG is set. Does nothing otherwise.
  void validateRegionToTransferredOpMapRegions() const {
#ifndef NDEBUG
    llvm::SmallSet<Region, 8> regions;
    for (auto [eltNo, regionNo] : elementToRegionMap) {
      regions.insert(regionNo);
    }
    for (auto [regionNo, opSet] : regionToTransferredOpMap) {
      assert(regions.contains(regionNo) && "Region doesn't exist?!");
    }
#endif
  }

  /// Used only in assertions, check that Partitions promised to be canonical
  /// are actually canonical
  bool is_canonical_correct() const;

  /// Merge the regions of two indices while maintaining canonicality. Returns
  /// the final region used.
  ///
  /// This runs in linear time.
  Region merge(Element fst, Element snd, bool updateHistory = true);

private:
  /// Pop one history node. Multiple history nodes can make up one PartitionOp
  /// worth of history, so this is called by popHistory.
  ///
  /// Returns true if we succesfully popped a single history node.
  bool popHistoryOnce(SmallVectorImpl<IsolationHistory> &foundJoinHistoryNodes);

  /// A canonical region is defined to have its region number as equal to the
  /// minimum element number of all of its assigned element numbers. This
  /// routine goes through the element -> region map and transforms the
  /// partition state to restore this property.
  ///
  /// This runs in linear time.
  void canonicalize();

  /// Walk the elementToRegionMap updating all elements in the region of \p
  /// targetElement will be changed to now point at \p newRegion.
  void horizontalUpdate(Element targetElement, Region newRegion,
                        SmallVectorImpl<Element> &mergedElements);

  /// Push onto the history list that \p element should be added into its own
  /// independent region.
  IsolationHistoryNode *pushNewElementRegion(Element element) {
    return history.pushNewElementRegion(element);
  }

  /// Push onto the history that \p element should be removed from the region it
  /// belongs to and that \p element is the last element in that region.
  void pushRemoveLastElementFromRegion(Element element) {
    history.pushRemoveLastElementFromRegion(element);
  }

  /// Push onto the history that \p elementToRemove should be removed from the
  /// region which \p elementFromOldRegion belongs to.
  void pushRemoveElementFromRegion(Element elementFromOldRegion,
                                   Element elementToRemove) {
    history.pushRemoveElementFromRegion(elementFromOldRegion, elementToRemove);
  }

  /// Push that \p other should be merged into this region.
  void pushCFGHistoryJoin(IsolationHistory otherHistory) {
    if (auto *head = otherHistory.head)
      history.pushCFGHistoryJoin(head);
  }

  /// NOTE: Assumes that \p elementToMergeInto and \p otherRegions are disjoint.
  void pushMergeElementRegions(Element elementToMergeInto,
                               ArrayRef<Element> otherRegions) {
    history.pushMergeElementRegions(elementToMergeInto, otherRegions);
  }

  /// Remove a single element without touching the region to transferring inst
  /// multimap. Assumes that the element is never the last element in a region.
  ///
  /// Just a helper routine.
  void removeElement(Element e) {
    // We added an element to its own region... so we should remove it and it
    // should be the last element in the region.
    bool result = elementToRegionMap.erase(e);
    canonical = false;
    assert(result && "Failed to erase?!");
  }
};

/// A data structure that applies a series of PartitionOps to a single Partition
/// that it modifies.
///
/// Callers use CRTP to modify its behavior. Please see the definition below of
/// a "blank" subclass PartitionOpEvaluatorBaseImpl for a description of the
/// methods needing to be implemented by other CRTP subclasses.
template <typename Impl>
struct PartitionOpEvaluator {
private:
  Impl &asImpl() { return *reinterpret_cast<Impl *>(this); }
  const Impl &asImpl() const { return *reinterpret_cast<const Impl *>(this); }

public:
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSetFactory =
      Partition::TransferringOperandSetFactory;

protected:
  TransferringOperandSetFactory &ptrSetFactory;
  TransferringOperandToStateMap &operandToStateMap;

  Partition &p;

public:
  PartitionOpEvaluator(Partition &p,
                       TransferringOperandSetFactory &ptrSetFactory,
                       TransferringOperandToStateMap &operandToStateMap)
      : ptrSetFactory(ptrSetFactory), operandToStateMap(operandToStateMap),
        p(p) {}

  /// Call shouldEmitVerboseLogging on our CRTP subclass.
  bool shouldEmitVerboseLogging() const {
    return asImpl().shouldEmitVerboseLogging();
  }

  /// Call handleUnknownCodePattern on our CRTP subclass.
  void handleUnknownCodePattern(const PartitionOp &op) const {
    return asImpl().handleUnknownCodePattern(op);
  }

  /// Call handleLocalUseAfterTransfer on our CRTP subclass.
  void handleLocalUseAfterTransfer(const PartitionOp &op, Element elt,
                                   Operand *transferringOp) const {
    return asImpl().handleLocalUseAfterTransfer(op, elt, transferringOp);
  }

  /// Call handleTransferNonTransferrable on our CRTP subclass.
  void handleTransferNonTransferrable(
      const PartitionOp &op, Element elt,
      SILDynamicMergedIsolationInfo isolationRegionInfo) const {
    return asImpl().handleTransferNonTransferrable(op, elt,
                                                   isolationRegionInfo);
  }
  /// Just call our CRTP subclass.
  void handleTransferNonTransferrable(
      const PartitionOp &op, Element elt, Element otherElement,
      SILDynamicMergedIsolationInfo isolationRegionInfo) const {
    return asImpl().handleTransferNonTransferrable(op, elt, otherElement,
                                                   isolationRegionInfo);
  }

  /// Call isActorDerived on our CRTP subclass.
  bool isActorDerived(Element elt) const {
    return asImpl().isActorDerived(elt);
  }

  SILIsolationInfo getIsolationRegionInfo(Element elt) const {
    return asImpl().getIsolationRegionInfo(elt);
  }

  /// Compute the isolation region info for all elements in \p region.
  ///
  /// The bool result is if it is captured by a closure element. That only is
  /// computed if \p sourceOp is non-null.
  std::optional<std::pair<SILDynamicMergedIsolationInfo, bool>>
  getIsolationRegionInfo(Region region, Operand *sourceOp) const {
    bool isClosureCapturedElt = false;
    std::optional<SILDynamicMergedIsolationInfo> isolationRegionInfo =
        SILDynamicMergedIsolationInfo();

    for (const auto &pair : p.range()) {
      if (pair.second == region) {
        isolationRegionInfo =
            isolationRegionInfo->merge(getIsolationRegionInfo(pair.first));
        if (!isolationRegionInfo)
          return {};
        if (sourceOp)
          isClosureCapturedElt |= isClosureCaptured(pair.first, sourceOp);
      }
    }

    return {{isolationRegionInfo.value(), isClosureCapturedElt}};
  }

  /// Overload of \p getIsolationRegionInfo without an Operand.
  SILIsolationInfo getIsolationRegionInfo(Region region) const {
    return getIsolationRegionInfo(region, nullptr).first;
  }

  bool isTaskIsolatedDerived(Element elt) const {
    return asImpl().isTaskIsolatedDerived(elt);
  }

  /// Call isClosureCaptured on our CRTP subclass.
  bool isClosureCaptured(Element elt, Operand *op) const {
    return asImpl().isClosureCaptured(elt, op);
  }

  /// Some evaluators pass in mock instructions that one cannot call getLoc()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILLocation getLoc(SILInstruction *inst) { return Impl::getLoc(inst); }

  /// Some evaluators pass in mock operands that one cannot call getLoc()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILLocation getLoc(Operand *op) { return Impl::getLoc(op); }

  /// Some evaluators pass in mock operands that one cannot call getUser()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILIsolationInfo getIsolationInfo(const PartitionOp &partitionOp) {
    return Impl::getIsolationInfo(partitionOp);
  }

  std::optional<Element> getElement(SILValue value) const {
    return asImpl().getElement(value);
  }

  SILValue getRepresentative(SILValue value) const {
    return asImpl().getRepresentative(value);
  }

  /// Apply \p op to the partition op.
  void apply(const PartitionOp &op) const {
    if (shouldEmitVerboseLogging()) {
      REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "Applying: ";
                                       op.print(llvm::dbgs()));
      REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    Before: ";
                                       p.print(llvm::dbgs()));
    }
    SWIFT_DEFER {
      if (shouldEmitVerboseLogging()) {
        REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    After:  ";
                                         p.print(llvm::dbgs()));
      }
      assert(p.is_canonical_correct());
    };

    // Set the boundary so that as we push, this shows when to stop processing
    // for this PartitionOp.
    SILLocation loc = op.hasSourceInst() ? getLoc(op.getSourceInst())
                                         : getLoc(op.getSourceOp());
    p.pushHistorySequenceBoundary(loc);

    switch (op.getKind()) {
    case PartitionOpKind::Assign:
      assert(op.getOpArgs().size() == 2 &&
             "Assign PartitionOp should be passed 2 arguments");
      assert(p.isTrackingElement(op.getOpArgs()[1]) &&
             "Assign PartitionOp's source argument should be already tracked");
      p.assignElement(op.getOpArgs()[0], op.getOpArgs()[1]);
      return;
    case PartitionOpKind::AssignFresh:
      assert(op.getOpArgs().size() == 1 &&
             "AssignFresh PartitionOp should be passed 1 argument");

      p.trackNewElement(op.getOpArgs()[0]);
      return;
    case PartitionOpKind::Transfer: {
      // NOTE: We purposely do not check here if a transferred value is already
      // transferred. Callers are expected to put a require for that
      // purpose. This ensures that if we pass the same argument multiple times
      // to the same transferring function as weakly transferred arguments, we
      // do not get an error.
      assert(op.getOpArgs().size() == 1 &&
             "Transfer PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "Transfer PartitionOp's argument should already be tracked");

      // Before we do any further work, see if we have a nonisolated(unsafe)
      // element. In such a case, this is also not a real transfer point.
      Element transferredElement = op.getOpArgs()[0];
      if (getIsolationRegionInfo(transferredElement).isUnsafeNonIsolated()) {
        return;
      }

      // Otherwise, we need to merge our isolation region info with the
      // isolation region info of everything else in our region. This is the
      // dynamic isolation region info found by the dataflow.
      Region transferredRegion = p.getRegion(transferredElement);
      bool isClosureCapturedElt = false;
      SILDynamicMergedIsolationInfo transferredRegionIsolation;
      auto pairOpt =
          getIsolationRegionInfo(transferredRegion, op.getSourceOp());
      if (!pairOpt) {
        handleUnknownCodePattern(op);
        return;
      }
      std::tie(transferredRegionIsolation, isClosureCapturedElt) = *pairOpt;

      // If we merged anything, we need to handle a transfer non-transferrable
      // unless our value has the same isolation info as our callee.
      auto calleeIsolationInfo = getIsolationInfo(op);
      if (!(calleeIsolationInfo &&
            transferredRegionIsolation.hasSameIsolation(calleeIsolationInfo)) &&
          !transferredRegionIsolation.isDisconnected()) {
        return handleTransferNonTransferrableHelper(op, op.getOpArgs()[0],
                                                    transferredRegionIsolation);
      }

      // Next see if we are disconnected and have the same isolation. In such a
      // case, we do not transfer since the disconnected value is allowed to be
      // resued after we return.
      if (transferredRegionIsolation.isDisconnected() && calleeIsolationInfo &&
          transferredRegionIsolation.hasSameIsolation(calleeIsolationInfo))
        return;

      // Mark op.getOpArgs()[0] as transferred.
      TransferringOperandState &state = operandToStateMap.get(op.getSourceOp());
      state.isClosureCaptured |= isClosureCapturedElt;
      if (auto newInfo =
              state.isolationInfo.merge(transferredRegionIsolation)) {
        state.isolationInfo = *newInfo;
      } else {
        handleUnknownCodePattern(op);
      }
      assert(state.isolationInfo && "Cannot have unknown");
      state.isolationHistory.pushCFGHistoryJoin(p.getIsolationHistory());
      auto *ptrSet = ptrSetFactory.get(op.getSourceOp());
      p.markTransferred(op.getOpArgs()[0], ptrSet);
      return;
    }
    case PartitionOpKind::UndoTransfer: {
      assert(op.getOpArgs().size() == 1 &&
             "UndoTransfer PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "UndoTransfer PartitionOp's argument should already be tracked");

      // Mark op.getOpArgs()[0] as not transferred.
      p.undoTransfer(op.getOpArgs()[0]);
      return;
    }
    case PartitionOpKind::Merge:
      assert(op.getOpArgs().size() == 2 &&
             "Merge PartitionOp should be passed 2 arguments");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             p.isTrackingElement(op.getOpArgs()[1]) &&
             "Merge PartitionOp's arguments should already be tracked");

      p.merge(op.getOpArgs()[0], op.getOpArgs()[1]);
      return;
    case PartitionOpKind::Require:
      assert(op.getOpArgs().size() == 1 &&
             "Require PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "Require PartitionOp's argument should already be tracked");
      if (auto *transferredOperandSet = p.getTransferred(op.getOpArgs()[0])) {
        for (auto transferredOperand : transferredOperandSet->data()) {
          handleLocalUseAfterTransferHelper(op, op.getOpArgs()[0],
                                            transferredOperand);
        }
      }
      return;
    case PartitionOpKind::UnknownPatternError:
      // Begin tracking the specified element in case we have a later use.
      p.trackNewElement(op.getOpArgs()[0]);

      // Then emit an unknown code pattern error.
      handleUnknownCodePattern(op);
      return;
    }

    llvm_unreachable("Covered switch isn't covered?!");
  }

  void apply(std::initializer_list<PartitionOp> ops) {
    for (auto &o : ops)
      apply(o);
  }

  /// Provides a way for subclasses to disable the error squelching
  /// functionality.
  ///
  /// Used by the unittests.
  bool shouldTryToSquelchErrors() const {
    return asImpl().shouldTryToSquelchErrors();
  }

private:
  bool isConvertFunctionFromSendableType(SILValue equivalenceClassRep) const {
    SILValue valueToTest = equivalenceClassRep;
    while (true) {
      if (auto *i = dyn_cast<ThinToThickFunctionInst>(valueToTest)) {
        valueToTest = i->getOperand();
        continue;
      }
      if (auto *i = dyn_cast<ConvertEscapeToNoEscapeInst>(valueToTest)) {
        valueToTest = i->getOperand();
        continue;
      }
      break;
    }

    auto *cvi = dyn_cast<ConvertFunctionInst>(valueToTest);
    if (!cvi)
      return false;

    return cvi->getOperand()->getType().isSendable(
        equivalenceClassRep->getFunction());
  }

  // Private helper that squelches the error if our transfer instruction and our
  // use have the same isolation.
  void handleLocalUseAfterTransferHelper(const PartitionOp &op, Element elt,
                                         Operand *transferringOp) const {
    if (shouldTryToSquelchErrors()) {
      if (auto isolationInfo = getIsolationInfo(op)) {
        if (isolationInfo.isActorIsolated() &&
            isolationInfo.hasSameIsolation(
                SILIsolationInfo::get(transferringOp->getUser())))
          return;
      }

      if (SILValue equivalenceClassRep =
              getRepresentative(transferringOp->get())) {

        // If we have a temporary that is initialized with an unsafe nonisolated
        // value... squelch the error like if we were that value.
        //
        // TODO: This goes away with opaque values.
        if (auto *asi = dyn_cast<AllocStackInst>(equivalenceClassRep)) {
          if (SILValue value = getInitOfTemporaryAllocStack(asi)) {
            if (auto elt = getElement(value)) {
              SILIsolationInfo eltIsolationInfo = getIsolationRegionInfo(*elt);
              if (eltIsolationInfo.isUnsafeNonIsolated()) {
                return;
              }
            }
          }
        }

        // See if we have a convert function from a `@Sendable` type. In this
        // case, we want to squelch the error.
        if (isConvertFunctionFromSendableType(equivalenceClassRep))
          return;
      }

      // If our instruction does not have any isolation info associated with it,
      // it must be nonisolated. See if our function has a matching isolation to
      // our transferring operand. If so, we can squelch this.
      if (auto functionIsolation =
              transferringOp->getUser()->getFunction()->getActorIsolation()) {
        if (functionIsolation.isActorIsolated() &&
            SILIsolationInfo::get(transferringOp->getUser())
                .hasSameIsolation(functionIsolation))
          return;
      }
    }

    // Ok, we actually need to emit a call to the callback.
    return handleLocalUseAfterTransfer(op, elt, transferringOp);
  }

  // Private helper that squelches the error if our transfer instruction and our
  // use have the same isolation.
  void handleTransferNonTransferrableHelper(
      const PartitionOp &op, Element elt,
      SILDynamicMergedIsolationInfo dynamicMergedIsolationInfo) const {
    if (shouldTryToSquelchErrors()) {
      if (SILValue equivalenceClassRep =
              getRepresentative(op.getSourceOp()->get())) {
        // If we have a temporary that is initialized with an unsafe nonisolated
        // value... squelch the error like if we were that value.
        //
        // TODO: This goes away with opaque values.
        if (auto *asi = dyn_cast<AllocStackInst>(equivalenceClassRep)) {
          if (SILValue value = getInitOfTemporaryAllocStack(asi)) {
            if (auto elt = getElement(value)) {
              SILIsolationInfo eltIsolationInfo = getIsolationRegionInfo(*elt);
              if (eltIsolationInfo.isUnsafeNonIsolated()) {
                return;
              }
            }
          }
        }

        // See if we have a convert function from a `@Sendable` type. In this
        // case, we want to squelch the error.
        if (isConvertFunctionFromSendableType(equivalenceClassRep))
          return;
      }
    }

    // Ok, we actually need to emit a call to the callback.
    return handleTransferNonTransferrable(op, elt, dynamicMergedIsolationInfo);
  }
};

/// A base implementation that can be used to default initialize CRTP
/// subclasses. Only used to implement base functionality for subclass
/// CRTPs. For true basic evaluation, use PartitionOpEvaluatorBasic below.
template <typename Subclass>
struct PartitionOpEvaluatorBaseImpl : PartitionOpEvaluator<Subclass> {
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSetFactory =
      Partition::TransferringOperandSetFactory;
  using Super = PartitionOpEvaluator<Subclass>;

  PartitionOpEvaluatorBaseImpl(Partition &workingPartition,
                               TransferringOperandSetFactory &ptrSetFactory,
                               TransferringOperandToStateMap &operandToStateMap)
      : Super(workingPartition, ptrSetFactory, operandToStateMap) {}

  /// Should we emit extra verbose logging statements when evaluating
  /// PartitionOps.
  bool shouldEmitVerboseLogging() const { return true; }

  /// A function called if we discover a transferred value was used after it
  /// was transferred.
  ///
  /// The arguments passed to the closure are:
  ///
  /// 1. The PartitionOp that required the element to be alive.
  ///
  /// 2. The element in the PartitionOp that was asked to be alive.
  ///
  /// 3. The operand of the instruction that originally transferred the
  /// region. Can be used to get the immediate value transferred or the
  /// transferring instruction.
  void handleLocalUseAfterTransfer(const PartitionOp &op, Element elt,
                                   Operand *transferringOp) const {}

  /// This is called if we detect a never transferred element that was passed to
  /// a transfer instruction.
  void handleTransferNonTransferrable(
      const PartitionOp &op, Element elt,
      SILDynamicMergedIsolationInfo regionInfo) const {}

  void handleTransferNonTransferrable(
      const PartitionOp &op, Element elt, Element otherElement,
      SILDynamicMergedIsolationInfo isolationRegionInfo) const {}

  /// Used to signify an "unknown code pattern" has occured while performing
  /// dataflow.
  ///
  /// DISCUSSION: Our dataflow cannot emit errors itself so this is a callback
  /// to our user so that we can emit that error as we process.
  void handleUnknownCodePattern(const PartitionOp &op) const {}

  /// This is used to determine if an element is actor derived. If we determine
  /// that a region containing such an element is transferred, we emit an error
  /// since actor regions cannot be transferred.
  bool isActorDerived(Element elt) const { return false; }

  /// This is used to determine if an element is in the same region as a task
  /// isolated value.
  bool isTaskIsolatedDerived(Element elt) const { return false; }

  /// Returns the information about \p elt's isolation that we ascertained from
  /// SIL and the AST.
  SILIsolationInfo getIsolationRegionInfo(Element elt) const {
    return SILIsolationInfo();
  }

  /// If we are able to, return the element associated with \p value. If
  /// unsupported, returns none.
  std::optional<Element> getElement(SILValue value) const { return {}; }

  /// If supported, returns the representative in \p value's equivalence
  /// class. Returns an empty SILValue if this is unsupported or if it does not
  /// have one.
  SILValue getRepresentative(SILValue value) const { return SILValue(); }

  /// Check if the representative value of \p elt is closure captured at \p
  /// op.
  ///
  /// NOTE: We actually just use the user of \p op in our callbacks. The reason
  /// why we do not just pass in that SILInstruction is that then we would need
  /// to access the instruction in the evaluator which creates a problem when
  /// since the operand we pass in is a dummy operand.
  bool isClosureCaptured(Element elt, Operand *op) const { return false; }

  /// By default squelch errors.
  bool shouldTryToSquelchErrors() const { return true; }

  static SILLocation getLoc(SILInstruction *inst) { return inst->getLoc(); }
  static SILLocation getLoc(Operand *op) { return op->getUser()->getLoc(); }
  static SILIsolationInfo getIsolationInfo(const PartitionOp &partitionOp) {
    return SILIsolationInfo::get(partitionOp.getSourceInst());
  }
};

/// A subclass of PartitionOpEvaluatorBaseImpl that doesn't have any special
/// behavior.
struct PartitionOpEvaluatorBasic final
    : PartitionOpEvaluatorBaseImpl<PartitionOpEvaluatorBasic> {
  PartitionOpEvaluatorBasic(Partition &workingPartition,
                            TransferringOperandSetFactory &ptrSetFactory,
                            TransferringOperandToStateMap &operandToStateMap)
      : PartitionOpEvaluatorBaseImpl(workingPartition, ptrSetFactory,
                                     operandToStateMap) {}
};

} // namespace swift

#endif // SWIFT_PARTITIONUTILS_H

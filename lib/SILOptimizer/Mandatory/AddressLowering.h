//===--- AddressLowering.h - Lower SIL address-only types. ----------------===//
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

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

namespace swift {

/// Track an opaque value's storage. An opaque value is a SILValue with
/// address-only type. Stages in the storage life-cycle:
///
/// 1. Unallocated
///
/// 2. Allocated. Either (a) it is a root value where 'storageAddress' is an
/// alloc_stack, or (b) it is a projection where 'projectedStorageID' refers to
/// the parent ValueStorage, which recursively leads to a root value with a
/// valid 'storageAddress'.
///
/// 3. Materialized. 'storageAddress' is valid. Address projections have been
/// emitted at the point that this value is defined.
///
/// 4. Rewritten. The definition of this opaque value is fully translated
/// into lowered SIL. Instructions are typically materialized and rewritten at
/// the same time. An indirect result, however, is materialized as soon as its
/// alloc_stack is emitted, but only rewritten once the call itself is
/// rewritten.
///
/// A projection may project out of an operand's definition (def-projection).
/// After allocation, before materialization or rewriting, we may have:
///
///   %result_addr = alloc_stack       // storage for %result
///   %result = apply : $() -> @out T
///   %extract = struct_extract %result // def-projection of %result
///
/// Or, a projection may project into a composing use (use-projection):
///
///   %struct_addr = alloc_stack      // storage for %struct
///   %result = apply : $() -> @out T // use-projection of %struct at operand #0
///   %struct = struct (%result)
///
/// A phi-projection is a use projection that projects its entire value
/// through a phi rather than into a composing use. It has an invalid
/// 'projectedOperandNum':
///
///     %result = apply : $() -> @out T // use-projection of %phi
///     br bb1(%result)
///   bb1(%phi : @owned $T)
///
/// Operations that destructively reuse storage (open_existential_value,
/// unchecked_enum_data, and switch_enum), are not considered storage
/// projections. Instead, these values have no ValueStorage but are rewritten to
/// directly reuse their operand's storage.
///
/// To materialize projections, address lowering follows the original def-use
/// edges for opaque values. Consequently, values that have storage cannot
/// be removed from SIL or from the storage map until rewriting is
/// complete. Mapped values can, however, be substituted on-the-fly by emitting
/// a place-holder value and updating the map entry. This works because the
/// value storage map holds no direct references to any SIL entities, such as
/// Operands or SILValues.
///
/// An opaque value's storage will be a def-projection if it's the result of
/// some disaggregation.  If %o = disaggregate %p then %o's storage will be
/// a def-projection out of %p's storage.
///
/// An opaque value's storage _may_ be a use-projection if it's an operand of
/// some aggregation.  If %p = aggregate %o, then %o's storage may be a
/// use-projection out of %p's storage.
///
/// Projections naturally form chains.  A value's storage may be a projection
/// out of the storage of some other value's storage which is itself a
/// projection out of a third value's storage.  This can happen in three ways:
///
/// (1) %o -def-> %p -def-> %q
///       %p = disaggregate %q
///       %o = disaggregate %p
/// (2) %o -use-> %p -use-> %q
///       %p = aggregate %o
///       %q = aggregate %p
/// (3) %o -def-> %p -use-> %q
///       %p = ...
///       cond_br left, right
///     left:
///       %o = disaggregate %p
///     right:
///       %q = aggregate %p
///
///     Branching like this is actually necessary.  It's not legal to aggregate
///     guaranteed opaque values since doing so changes representation which
///     implies a copy.
///
/// It is NOT possible to have links like
///
/// (4) %o -use-> %p -def-> %q
///
///     The reason is that the links mean contradictory things:
///       %o -use-> %p means %p = aggregate %o
///       %p -def-> %q means %p = disaggregate %q
///     There is no overlap between the "aggregate" and the "disaggregate"
///     opcodes.
///
/// This means that any chain of projections looks like
///
///    %d_0 -def-> ... -def-> %d_N -use-> %u_0 -use-> ... -use-> %u_M
///
/// a sequence (possibly empty) of def projections followed by a sequence
/// (possibly emtpy) of use projections [projection_chain_structure].
struct ValueStorage {
  enum : uint32_t { InvalidID = uint32_t(~0) };
  enum : uint16_t { InvalidOper = uint16_t(~0) };

  /// The final address of this storage after rewriting the SIL. For values
  /// linked to their own storage, this is set during storage allocation to an
  /// alloc_stack or indirect function argument. For projections, it is only set
  /// after materialization (during instruction rewriting).
  SILValue storageAddress;

  /// The latest instruction which opens an archetype involved in the value's
  /// type.  Just a cache of getLatestOpeningInst(value).
  mutable llvm::Optional<SILInstruction *> latestOpeningInst = llvm::None;

  /// When either isDefProjection or isUseProjection is set, this refers to the
  /// storage whose "def" this value projects out of or whose operand this
  /// storage projects into via its "use.
  uint32_t projectedStorageID = InvalidID;

  /// For use-projections, identifies the operand index of the composing use.
  /// Only valid for non-phi use projections.
  uint16_t projectedOperandNum = InvalidOper;

  /// Projection out of a storage def. e.g. this value is a destructure.
  unsigned isDefProjection : 1;

  /// Projection into a composing use or phi. e.g. this value is used by a
  /// struct, tuple, enum, or branch.
  unsigned isUseProjection : 1;

  // The definition of this value is fully translated to lowered SIL.
  unsigned isRewritten : 1;

  // This is a use-projection which performs an initialization side-effect,
  // either into an enum or an existential.
  //
  // Tracked to avoid projecting enums/existentials across phis, which would
  // result in piecewise initialization.
  //
  // Note that the corresponding value is the payload, not the
  // enum instruction.
  unsigned initializes : 1;

  ValueStorage(SILValue storageAddress): storageAddress(storageAddress) {
    isDefProjection = false;
    isUseProjection = false;
    isRewritten = false;
    initializes = false;

    // The initial storage address is only valid when the value is effectively
    // already rewritten.
    if (storageAddress) {
      isRewritten = true;
    }
  }

  bool isAllocated() const {
    return storageAddress || isUseProjection || isDefProjection;
  }

  bool isProjection() const { return isUseProjection || isDefProjection; }

  bool isPhiProjection() const {
    return isUseProjection && projectedOperandNum == InvalidOper;
  }

  bool isComposingUseProjection() const {
    return isUseProjection && projectedOperandNum != InvalidOper;
  }

  void markRewritten() {
    assert(storageAddress);
    isRewritten = true;
  }

  SILValue getMaterializedAddress() const {
    assert(isRewritten && "storage has not been materialized");
    return storageAddress;
  }

#ifndef NDEBUG
  void dump() const;
#endif
};

/// Map each opaque/resilient SILValue to its abstract storage.
/// Iteration guarantees RPO order.
///
/// Mapped values are expected to be created in a single RPO pass. "erase" is
/// unsupported. Values must be replaced using 'replaceValue()'.
class ValueStorageMap {
public:
  struct ValueStoragePair {
    SILValue value;
    ValueStorage storage;
    ValueStoragePair(SILValue v, ValueStorage s) : value(v), storage(s) {}
#ifndef NDEBUG
    void dump() const;
#endif
  };

private:
  typedef std::vector<ValueStoragePair> ValueVector;
  // Hash of values to ValueVector indices.
  typedef llvm::DenseMap<SILValue, unsigned> ValueHashMap;

  ValueVector valueVector;
  ValueHashMap valueHashMap;

  // True after valueVector is done growing, so ValueStorage references will no
  // longer be invalidated.
  SWIFT_ASSERT_ONLY_DECL(bool stableStorage = false);

public:
  class ProjectionIterator {
  public:
    using This = ProjectionIterator;
    using iterator_category = std::forward_iterator_tag;
    using value_type = ValueStoragePair const *;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type *;
    using reference = value_type &;

  protected:
    value_type Cur;
    ValueStorageMap const &Map;

  public:
    explicit ProjectionIterator(value_type cur, ValueStorageMap const &map)
        : Cur(cur), Map(map) {}
    ValueStoragePair const *operator->() const { return Cur; }
    ValueStoragePair const *operator*() const { return Cur; }

    ValueStorage const &getStorage() const { return Cur->storage; }
    SILValue getValue() const { return Cur->value; }
    This &operator++() {
      assert(Cur && "incrementing past end()!");
      if (Cur->storage.isProjection())
        Cur = &Map.getProjectedStorage(Cur->storage);
      else
        Cur = nullptr;
      return *this;
    }

    This operator++(int unused) {
      This copy = *this;
      ++*this;
      return copy;
    }

    friend bool operator==(This lhs, This rhs) { return lhs.Cur == rhs.Cur; }
    friend bool operator!=(This lhs, This rhs) { return !(lhs == rhs); }
  };

  ProjectionIterator projection_begin(SILValue value) const {
    return ProjectionIterator(&valueVector[getOrdinal(value)], *this);
  }
  ProjectionIterator projection_end() const {
    return ProjectionIterator(nullptr, *this);
  }
  /// Returns projections of the specified value from the inside out, starting
  /// from the projection for the value and walking outwards to its storage
  /// root.
  iterator_range<ProjectionIterator> getProjections(SILValue value) const {
    if (!contains(value))
      return {projection_end(), projection_end()};
    return {projection_begin(value), projection_end()};
  }

  friend class ProjectionIterator;

  bool empty() const { return valueVector.empty(); }

  void clear() {
    valueVector.clear();
    valueHashMap.clear();
  }

  /// Iterate over value storage in RPO order. Once we begin erasing
  /// instructions, some entries could become invalid. ValueStorage validity can
  /// be checked with valueStorageMap.contains(value).
  ValueVector::iterator begin() { return valueVector.begin(); }

  ValueVector::iterator end() { return valueVector.end(); }

  ValueVector::reverse_iterator rbegin() { return valueVector.rbegin(); }

  ValueVector::reverse_iterator rend() { return valueVector.rend(); }

  bool contains(SILValue value) const {
    return valueHashMap.find(value) != valueHashMap.end();
  }

  unsigned getOrdinal(SILValue value) const {
    auto hashIter = valueHashMap.find(value);
    assert(hashIter != valueHashMap.end() && "Missing SILValue");
    return hashIter->second;
  }

  ValueStoragePair &operator[](uint32_t index) { return valueVector[index]; }

  ValueStoragePair const &operator[](uint32_t index) const {
    return valueVector[index];
  }

  ValueStorage &getStorage(SILValue value) {
    return valueVector[getOrdinal(value)].storage;
  }
  const ValueStorage &getStorage(SILValue value) const {
    return valueVector[getOrdinal(value)].storage;
  }

  const ValueStorage *getStorageOrNull(SILValue value) const {
    auto iter = valueHashMap.find(value);
    if (iter == valueHashMap.end())
      return nullptr;

    return &valueVector[iter->second].storage;
  }

  void setStable() { SWIFT_ASSERT_ONLY(stableStorage = true); }

  /// Given storage for a projection, return the projected storage by following
  /// single level of projected storage. The returned storage may
  /// recursively be a another projection.
  const ValueStoragePair &
  getProjectedStorage(const ValueStorage &storage) const {
    assert(storage.isProjection());
    return valueVector[storage.projectedStorageID];
  }

  ValueStoragePair &getProjectedStorage(const ValueStorage &storage) {
    assert(storage.isProjection());
    return valueVector[storage.projectedStorageID];
  }

  /// Return the non-projection storage that the given storage ultimately refers
  /// to by following all projections.
  const ValueStorage &getBaseStorage(SILValue value) {
    ValueStorage const *last = nullptr;
    for (auto *pair : getProjections(value)) {
      last = &pair->storage;
    }
    return *last;
  }

  void setStorageAddress(SILValue value, SILValue addr) {
    auto &storage = getStorage(value);
    assert(!storage.storageAddress || storage.storageAddress == addr);
    storage.storageAddress = addr;
  }

  /// Insert a value in the map, creating a ValueStorage object for it. This
  /// must be called in RPO order.
  void insertValue(SILValue value, SILValue storageAddress);

  /// Replace a value that is mapped to storage with another value. This allows
  /// limited rewriting of original opaque values. For example, block
  /// arguments can be replaced with fake loads in order to rewrite their
  /// corresponding terminator.
  void replaceValue(SILValue oldValue, SILValue newValue);

  /// Record a storage projection from the source of the given operand into its
  /// use (e.g. struct_extract, tuple_extract, switch_enum).
  void recordDefProjection(Operand *oper, SILValue projectedValue);

  /// Record a storage projection from the use of the given operand into the
  /// operand's source. (e.g. Any value used by a struct, tuple, or enum may
  /// project storage into its use).
  void recordComposingUseProjection(Operand *oper, SILValue userValue);

  // Mark a phi operand value as coalesced with the phi storage.
  void recordPhiUseProjection(Operand *oper, SILPhiArgument *phi);

  /// Return true \p oper projects into its use's aggregate storage.
  bool isComposingUseProjection(Operand *oper) const;

#ifndef NDEBUG
  void dumpProjections(SILValue value);
  void dump();
#endif
};

} // namespace swift

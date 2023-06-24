//===--- FrozenMultiMap.h ----------------------------------*- C++ --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// A 2 stage multi-map for use in contexts where one is iterating over an IR in
/// a read only way and then wants to use a multi-map for post-processing that
/// can support iteration in insertion order.
///
/// In the first stage, the multi-map can be initialized with new elements in a
/// purely additive fashion, but methods that rely on the map being frozen
/// (find, getRange(), erase, etc) assert.
///
/// Once the user has finished iterating over the IR, the map is frozen and then
/// can only be used for map operations (find), erasing operations (erase), and
/// deterministic range iteration.
///
/// This is accomplished by the internal implementation of the frozen multi map
/// just being an array of (key, value) pairs that when we freeze, we sort using
/// a stable sort on the key. Since we use a stable sort on the key, we know
/// that all of the individual multimap value arrays are still in insertion
/// order helping us avoid non-determinism like one must deal with when using
/// other maps.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_FROZENMULTIMAP_H
#define SWIFT_BASIC_FROZENMULTIMAP_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>

namespace swift {

template <typename Key, typename Value,
          typename VectorStorage = std::vector<std::pair<Key, Optional<Value>>>>
class FrozenMultiMap {
  VectorStorage storage;
  bool frozen = false;

private:
  struct PairToSecondElt;
  struct PairWithTypeErasedOptionalSecondElt;

public:
  using PairToSecondEltRange =
      TransformRange<ArrayRef<std::pair<Key, Optional<Value>>>,
                     PairToSecondElt>;

  FrozenMultiMap() = default;

  void insert(const Key &key, const Value &value) {
    assert(!isFrozen() && "Can not insert new keys once map is frozen");
    storage.emplace_back(key, value);
  }

  Optional<PairToSecondEltRange> find(const Key &key) const {
    assert(isFrozen() &&
           "Can not perform a find operation until the map is frozen");
    // Since our array is sorted, we need to first find the first pair with our
    // inst as the first element.
    auto start = std::lower_bound(
        storage.begin(), storage.end(), std::make_pair(key, llvm::None),
        [&](const std::pair<Key, Optional<Value>> &p1,
            const std::pair<Key, Optional<Value>> &p2) {
          return p1.first < p2.first;
        });

    // If we did not find that first element or that key has a .none value
    // (signaling that we erased it), return None.
    if (start == storage.end() || start->first != key ||
        !start->second.has_value()) {
      return None;
    }

    // Ok, we found our first element. Now scan forward until we find a pair
    // whose instruction is not our own instruction.
    auto end = find_if_not(start, storage.end(),
                           [&](const std::pair<Key, Optional<Value>> &pair) {
                             return pair.first == key;
                           });
    unsigned count = std::distance(start, end);
    ArrayRef<std::pair<Key, Optional<Value>>> slice(&*start, count);
    return PairToSecondEltRange(slice, PairToSecondElt());
  }

  bool erase(const Key &key) {
    assert(isFrozen() &&
           "Can not perform an erase operation until the map is frozen");
    // Since our array is sorted, we need to first find the first pair with our
    // inst as the first element.
    auto start = std::lower_bound(
        storage.begin(), storage.end(), std::make_pair(key, Value()),
        [&](const std::pair<Key, Optional<Value>> &p1,
            const std::pair<Key, Optional<Value>> &p2) {
          return p1.first < p2.first;
        });

    // If we did not find that first element or that key has a .none value
    // (signaling that we erased it), return false.
    if (start == storage.end() || start->first != key ||
        !start->second.has_value()) {
      return false;
    }

    // Ok, we found our element. Just set its value to .none to signal it was
    // destroyed and then return true.
    start->second = None;
    return true;
  }

  bool isFrozen() const { return frozen; }

  /// Set this map into its frozen state. This stable sorts our internal array
  /// to create our map like context.
  ///
  /// After this, one can only use map like operations and non-mutable vector
  /// operations instead of full mutable/non-mutable vector operations.
  void setFrozen() {
    std::stable_sort(storage.begin(), storage.end(),
                     [&](const std::pair<Key, Optional<Value>> &lhs,
                         const std::pair<Key, Optional<Value>> &rhs) {
                       // Only compare the first entry so that we preserve
                       // insertion order.
                       return lhs.first < rhs.first;
                     });
    frozen = true;
  }

  /// Unfreeze the map, so one can go back to using mutable vector
  /// operations. After one calls this until one freezes the map again, one
  /// cannot use map operations.
  ///
  /// This allows one to incrementally update the map.
  void unfreeze() { frozen = false; }

  /// Reset the frozen multimap in an unfrozen state with its storage cleared.
  void reset() {
    storage.clear();
    frozen = false;
  }

  unsigned size() const { return storage.size(); }
  bool empty() const { return storage.empty(); }

  struct iterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<Key, Optional<PairToSecondEltRange>>;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;    
    using base_iterator = typename decltype(storage)::iterator;

    FrozenMultiMap &map;
    base_iterator baseIter;
    Optional<std::pair<Key, Optional<PairToSecondEltRange>>> currentValue;

    iterator(FrozenMultiMap &map, base_iterator iter)
        : map(map), baseIter(iter), currentValue() {
      updateCurrentValue();
    }

    void updateCurrentValue() {
      base_iterator end = map.storage.end();

      // If we are end, set currentValue to be None.
      if (baseIter == end) {
        currentValue = None;
        return;
      }

      // Otherwise, determine the next range that we are visiting.
      auto rangeEnd =
          std::find_if_not(std::next(baseIter), end,
                           [&](const std::pair<Key, Optional<Value>> &elt) {
                             return elt.first == baseIter->first;
                           });

      Optional<PairToSecondEltRange> resultRange;
      if (baseIter->second.has_value()) {
        unsigned count = std::distance(baseIter, rangeEnd);
        ArrayRef<std::pair<Key, Optional<Value>>> slice(&*baseIter, count);
        resultRange.emplace(slice, PairToSecondElt());
      }
      currentValue = {baseIter->first, resultRange};
    }

    iterator &operator++() {
      baseIter =
          std::find_if_not(std::next(baseIter), map.storage.end(),
                           [&](const std::pair<Key, Optional<Value>> &elt) {
                             return elt.first == baseIter->first;
                           });
      updateCurrentValue();
      return *this;
    }

    iterator operator++(int) {
      auto tmp = *this;
      baseIter = std::find_if_not(std::next(baseIter), map.storage.end(),
                                  [&](const std::pair<Key, Value> &elt) {
                                    return elt.first == baseIter->first;
                                  });
      updateCurrentValue();
      return tmp;
    }

    std::pair<Key, Optional<PairToSecondEltRange>> operator*() const {
      return *currentValue;
    }

    bool operator==(const iterator &RHS) const {
      return baseIter == RHS.baseIter;
    }

    bool operator!=(const iterator &RHS) const {
      return baseIter != RHS.baseIter;
    }
  };

  struct ToNonErasedValues {
    Optional<std::pair<Key, Optional<PairToSecondEltRange>>>
    operator()(std::pair<Key, Optional<PairToSecondEltRange>> pair) const {
      if (!pair.second.has_value())
        return None;
      return pair;
    }
  };

  using IgnoringErasedValueRangeType =
      OptionalTransformRange<llvm::iterator_range<iterator>, ToNonErasedValues>;
  using RangeType = TransformRange<IgnoringErasedValueRangeType,
                                   PairWithTypeErasedOptionalSecondElt>;

  /// Return a range of (key, ArrayRef<Value>) pairs. The keys are guaranteed to
  /// be in key sorted order and the ArrayRef<Value> are in insertion order.
  ///
  /// The range skips all erased (key, ArrayRef<Value>) entries.
  RangeType getRange() const {
    assert(isFrozen() &&
           "Can not create range until data structure is frozen?!");
    auto *self = const_cast<FrozenMultiMap *>(this);
    iterator iter1 = iterator(*self, self->storage.begin());
    iterator iter2 = iterator(*self, self->storage.end());
    auto baseRange = llvm::make_range(iter1, iter2);
    auto optRange = makeOptionalTransformRange(baseRange, ToNonErasedValues());
    return makeTransformRange(optRange, PairWithTypeErasedOptionalSecondElt());
  }

  /// Returns true if all values for all keys have been deleted.
  ///
  /// This is intended to be used in use cases where a frozen multi map is
  /// filled up with a multi-map and then as we process keys, we delete values
  /// we have handled. In certain cases, one wishes to validate after processing
  /// that all values for all keys were properly handled. One cannot perform
  /// this operation with getRange() in a nice way.
  bool allValuesHaveBeenDeleted() const {
    return llvm::all_of(storage, [](const std::pair<Key, Optional<Value>> &pair) {
      return !pair.second.hasValue();
    });
  }

  typename VectorStorage::iterator vector_begin() {
    assert(isFrozen() && "Can only call this in map mode");
    return storage.begin();
  }

  typename VectorStorage::iterator vector_end() {
    assert(isFrozen() && "Can only call this in map mode");
    return storage.end();
  }

  typename VectorStorage::const_iterator vector_begin() const {
    assert(isFrozen() && "Can only call this in map mode");
    return storage.begin();
  }

  typename VectorStorage::const_iterator vector_end() const {
    assert(isFrozen() && "Can only call this in map mode");
    return storage.end();
  }
};

template <typename Key, typename Value, typename Storage>
struct FrozenMultiMap<Key, Value, Storage>::PairToSecondElt {
  PairToSecondElt() {}

  Value operator()(const std::pair<Key, Optional<Value>> &pair) const {
    return *pair.second;
  }
};

template <typename Key, typename Value, typename Storage>
struct FrozenMultiMap<Key, Value,
                      Storage>::PairWithTypeErasedOptionalSecondElt {
  PairWithTypeErasedOptionalSecondElt() {}

  std::pair<Key, PairToSecondEltRange>
  operator()(const std::pair<Key, Optional<PairToSecondEltRange>> &pair) const {
    return std::make_pair(pair.first, *pair.second);
  }
};

template <typename Key, typename Value, unsigned SmallSize>
using SmallFrozenMultiMap =
    FrozenMultiMap<Key, Value,
                   SmallVector<std::pair<Key, Optional<Value>>, SmallSize>>;

} // namespace swift

#endif

//===--- BlotSetVector.h ----------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_BASIC_BLOTSETVECTOR_H
#define SWIFT_BASIC_BLOTSETVECTOR_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>

namespace swift {

/// This is a set container with the following properties:
///
/// 1. Fast insertion-order iteration.
///
/// 2. Stable index offsets for all values after all operations including
/// erase.
///
/// This contrasts with SetVector where index offsets are not stable due to
/// usage of std::vector::erase(). In contrast, BlotSetVector uses the `blot
/// operation' (a) which trades memory for runtime and index offset stability.
///
/// 3. Fast replacement of a value v1 with a second value v2 guaranteeing that
/// v2 is placed into the same array index as v1, just deleting v1 if v2 is
/// already in the array.
///
/// This is important if one has other data structures referring to v1 via v1's
/// index in the vector that one wishes to now refer to v2.
///
/// 4. Fast deletion via the 'blot' operation.
///
/// 5. Fast insertion.
///
/// (a) The `blot operation' is leaving the value in the set vector, but marking
/// the value as being dead.
template <typename ValueT,
          typename VectorT = std::vector<llvm::Optional<ValueT>>,
          typename MapT = llvm::DenseMap<ValueT, unsigned>>
class BlotSetVector {
  VectorT vector;
  MapT map;

public:
  /// Construct an empty BlotSetVector.
  BlotSetVector() {}

  bool empty() const { return vector.empty(); }

  unsigned size() const { return vector.size(); }

  using iterator = typename VectorT::iterator;
  using const_iterator = typename VectorT::const_iterator;
  iterator begin() { return vector.begin(); }
  iterator end() { return vector.end(); }
  const_iterator begin() const { return vector.begin(); }
  const_iterator end() const { return vector.end(); }

  ArrayRef<llvm::Optional<ValueT>> getArray() const { return vector; }

  iterator_range<const_iterator> getRange() const {
    return {begin(), end()};
  }

  using const_reverse_iterator = typename VectorT::const_reverse_iterator;
  const_reverse_iterator rbegin() const { return vector.rbegin(); }
  const_reverse_iterator rend() const { return vector.rend(); }
  iterator_range<const_reverse_iterator> getReverseRange() const {
    return {rbegin(), rend()};
  }

  const llvm::Optional<ValueT> &operator[](unsigned n) const {
    assert(n < vector.size() && "Out of range!");
    return vector[n];
  }

  /// Grow the set vector so that it can contain at least \p capacity items
  /// before resizing again.
  ///
  /// \param capacity The minimum size that the set vector will be able to grow
  ///                 to before a resize is required to insert additional items.
  void reserve(unsigned capacity) {
    vector.reserve(capacity);
    map.reserve(capacity);
  }

  /// Insert \p value into the SetVector if it is not there already.
  ///
  /// \param value The item to insert if not already present.
  ///
  /// \returns Both the index of the item and whether it was inserted in a pair.
  ///          If the item was already present, its preexisting index along with
  ///          false will be returned.  If the item was newly added, its new
  ///          index along with true will be returned.
  std::pair<unsigned, bool> insert(const ValueT &value) {
    auto iterator = map.find(value);
    if (iterator != map.end())
      return {iterator->second, false};

    unsigned index = vector.size();
    map[value] = index;
    vector.push_back(value);
    return {index, true};
  }

  bool count(const ValueT &value) const { return map.count(value); }

  /// Replace \p value1 with \p value2 placing \p value2 into the position in
  /// the array where value1 used to be. If \p value2 is already in the set,
  /// this just erases \p value1.
  void replace(const ValueT &value1, const ValueT &value2) {
    auto iterator1 = map.find(value1);
    assert(iterator1 != map.end() && "Cannot replace value that is not in set");
    unsigned index1 = iterator1->second;
    map.erase(value1);

    auto iterator2 = map.find(value2);
    if (iterator2 != map.end()) {
      vector[index1] = llvm::None;
      return;
    }

    map[value2] = index1;
    vector[index1] = value2;
  }

  /// Erase the value \p value if it is in the set. Returns true if value was
  /// successfully erased and false otherwise.
  bool erase(const ValueT &value) {
    auto iterator = map.find(value);
    if (iterator == map.end())
      return false;
    unsigned index = iterator->second;
    map.erase(iterator);
    vector[index] = llvm::None;
    return true;
  }

  /// Return the last element of the blot map vector. Will be None if blotted.
  llvm::Optional<ValueT> pop_back_val() {
    auto result = vector.pop_back_val();
    if (!result)
      return result;
    map.erase(*result);
    return result;
  }

  /// Attempt to lookup the index of \p value. Returns None upon failure and the
  /// value on success.
  llvm::Optional<unsigned> getIndex(const ValueT &value) {
    auto iterator = map.find(value);
    if (iterator == map.end())
      return llvm::None;
    return iterator->second;
  }

  /// Clear the backing vector and map.
  void clear() {
    vector.clear();
    map.clear();
  }
};

template <typename ValueT, unsigned N>
class SmallBlotSetVector
    : public BlotSetVector<ValueT, llvm::SmallVector<llvm::Optional<ValueT>, N>,
                           llvm::SmallDenseMap<ValueT, unsigned, N>> {
public:
  SmallBlotSetVector() {}
};

} // namespace swift

#endif // SWIFT_BASIC_BLOTSETVECTOR_H

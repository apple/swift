//===--- MultiMapCache.h --------------------------------------------------===//
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

#ifndef SWIFT_BASIC_MULTIMAPCACHE_H
#define SWIFT_BASIC_MULTIMAPCACHE_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {

/// A write-once multi-map cache that can be small. It uses a DenseMap
/// internally, so it can be used as a cache without needing to be frozen like
/// FrozenMultiMap (which uses only a vector internally). The cached value is
/// computed by a passed in std::function. The std::function is able to map
/// multiple values to a specific key via the out array.
///
/// NOTE: We store the (size, length) of each ArrayRef<ValueTy> instead of
/// storing the ArrayRef to avoid data invalidation issues caused by SmallVector
/// switching from small to large representations.
///
/// For an example of a subclass implementation see:
/// unittests/Basic/MultiMapCacheTest.cpp.
template <
    typename KeyTy, typename ValueTy,
    typename MapTy =
        llvm::DenseMap<KeyTy, llvm::Optional<std::tuple<unsigned, unsigned>>>,
    typename VectorTy = std::vector<ValueTy>, typename VectorTyImpl = VectorTy>
class MultiMapCache {
  std::function<bool(const KeyTy &, VectorTyImpl &)> function;
  MapTy valueToDataOffsetIndexMap;
  VectorTy data;

  constexpr static unsigned ArrayStartOffset = 0;
  constexpr static unsigned ArrayLengthOffset = 1;

public:
  MultiMapCache(std::function<bool(const KeyTy &, VectorTyImpl &)> function)
      : function(function) {}

  void clear() {
    valueToDataOffsetIndexMap.clear();
    data.clear();
  }

  bool empty() const { return valueToDataOffsetIndexMap.empty(); }
  unsigned size() const { return valueToDataOffsetIndexMap.size(); }

  llvm::Optional<ArrayRef<ValueTy>> get(const KeyTy &key) {
    auto iter = valueToDataOffsetIndexMap.try_emplace(key, llvm::None);

    // If we already have a cached value, just return the cached value.
    if (!iter.second) {
      return iter.first->second.transform(
          [&](std::tuple<unsigned, unsigned> startLengthRange) {
            return llvm::makeArrayRef(data).slice(
                std::get<ArrayStartOffset>(startLengthRange),
                std::get<ArrayLengthOffset>(startLengthRange));
          });
    }

    // Otherwise, try to compute the value. If we failed conservatively, return
    // None. The iter value already had None by default set to it, this just
    // makes it so that we do not need to have a memory dependency and can just
    // exit.
    unsigned initialOffset = data.size();

    // We assume that constructValuesForKey /only/ inserts to the end of data
    // and does not inspect any other values in the data array.
    if (!function(key, data)) {
      return llvm::None;
    }

    // Otherwise, compute our length, compute our initial ArrayRef<ValueTy>,
    // update the map with the start, length, and return the resulting ArrayRef.
    unsigned length = data.size() - initialOffset;
    iter.first->second = std::make_tuple(initialOffset, length);
    auto result = llvm::makeArrayRef(data).slice(initialOffset, length);
    return result;
  }
};

template <typename KeyTy, typename ValueTy>
using SmallMultiMapCache =
    MultiMapCache<KeyTy, ValueTy,
                  llvm::SmallDenseMap<
                      KeyTy, llvm::Optional<std::tuple<unsigned, unsigned>>, 8>,
                  SmallVector<ValueTy, 32>, SmallVectorImpl<ValueTy>>;

} // namespace swift

#endif

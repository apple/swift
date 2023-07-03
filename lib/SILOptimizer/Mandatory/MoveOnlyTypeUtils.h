//===--- MoveOnlyTypeUtils.h ----------------------------------------------===//
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
///
/// \file
///
/// This file contains utilities for manipulating types as used by the move
/// checker.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYTYPEUTILS_H
#define SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYTYPEUTILS_H

#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/SILBuilder.h"

namespace swift {
namespace siloptimizer {

struct TypeOffsetSizePair {
  SubElementOffset startOffset = 0;
  TypeSubElementCount size = 0;

  TypeOffsetSizePair() : startOffset(0), size(0) {}
  TypeOffsetSizePair(SILType baseType, SILFunction *fn)
      : startOffset(0), size(baseType, fn) {}
  TypeOffsetSizePair(SubElementOffset offset, TypeSubElementCount size)
      : startOffset(offset), size(size) {}
  TypeOffsetSizePair(SILValue projection, SILValue base)
      : startOffset(*SubElementOffset::compute(projection, base)),
        size(TypeSubElementCount(projection)) {}
  TypeOffsetSizePair(TypeTreeLeafTypeRange leafTypeRange)
      : startOffset(leafTypeRange.startEltOffset), size(leafTypeRange.size()) {}

  IntRange<unsigned> getRange() const {
    return range(startOffset, getEndOffset());
  }

  SubElementOffset getEndOffset() const {
    return SubElementOffset(startOffset + size);
  }

  bool operator==(const TypeOffsetSizePair &other) const {
    return startOffset == other.startOffset && size == other.size;
  }

  bool operator!=(const TypeOffsetSizePair &other) const {
    return !(*this == other);
  }

  /// Given an ancestor offset \p ancestorOffset and a type called \p
  /// ancestorType, walk one level towards this current type which is assumed to
  /// be a child type of \p ancestorType.
  llvm::Optional<std::pair<TypeOffsetSizePair, SILType>>
  walkOneLevelTowardsChild(TypeOffsetSizePair ancestorOffsetSize,
                           SILType ancestorType, SILFunction *fn) const;

  /// Given an ancestor offset \p ancestorOffset and a type called \p
  /// ancestorType, walk one level towards this current type inserting on value,
  /// the relevant projection.
  llvm::Optional<std::pair<TypeOffsetSizePair, SILValue>>
  walkOneLevelTowardsChild(SILBuilderWithScope &builder, SILLocation loc,
                           TypeOffsetSizePair ancestorOffsetSize,
                           SILValue ancestorValue) const;

  /// Given an ancestor offset \p ancestorOffset and a type called \p
  /// ancestorType, walk one level towards this current type which is assumed to
  /// be a child type of \p ancestorType.
  void constructPathString(SILType targetType,
                           TypeOffsetSizePair ancestorOffsetSize,
                           SILType ancestorType, SILFunction *fn,
                           llvm::raw_ostream &os) const;
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     const TypeOffsetSizePair &other) {
  return os << "(startOffset: " << other.startOffset << ", size: " << other.size
            << ")";
}

} // namespace siloptimizer
} // namespace swift

#endif

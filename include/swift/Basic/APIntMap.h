//===--- APIntMap.h - A map with APInts as the keys -------------*- C++ -*-===//
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
//
// LLVM does not allow arbitrary APInts to be the keys of a DenseMap because
// APInts are only comparable if they have the same bit-width.  This map
// implementation assumes that its keys will always be constrained to their
// minimum width, so it's not a general-purpose structure, but it does work.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_APINTMAP_H
#define SWIFT_BASIC_APINTMAP_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "swift/Basic/LLVM.h"

namespace swift {

struct WidthPreservingAPIntDenseMapInfo {
  // For the special values, we use -1 with a bit-width that isn't minimal
  // for the value, then use a parser that always produces values with
  // minimal bit-widths so that we don't get a conflict.
  static inline APInt getEmptyKey() {
    return APInt::getAllOnes(/*bitwidth*/2);
  }
  static inline APInt getTombstoneKey() {
    return APInt::getAllOnes(/*bitwidth*/3);
  }

  static unsigned getHashValue(const APInt &Key) {
    return llvm::DenseMapInfo<APInt>::getHashValue(Key);
  }

  static bool isEqual(const APInt &LHS, const APInt &RHS) {
    return llvm::DenseMapInfo<APInt>::isEqual(LHS, RHS);
  }
};

template <class Value>
using APIntMap = llvm::DenseMap<APInt, Value, WidthPreservingAPIntDenseMapInfo>;

}

#endif

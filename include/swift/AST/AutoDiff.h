//===--- AutoDiff.h - Swift Automatic Differentiation ---------------------===//
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
//  SWIFT_ENABLE_TENSORFLOW
//  This file defines AST support for automatic differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_AUTODIFF_H
#define SWIFT_AST_AUTODIFF_H

#include "ASTContext.h"

namespace swift {

enum class AutoDiffMode {
  Forward, Reverse
};

struct AutoDiffIndexParameter {
  SourceLoc loc;
  unsigned index;
};

class AutoDiffParameter {
public:
  enum class Kind { Index, Self };

private:
  SourceLoc Loc;
  Kind Kind;
  union Value {
    struct { unsigned Index; }; // Index
    struct {};                  // Self
    Value(unsigned index) : Index(index) {}
    Value() {}
  } V;

public:
  AutoDiffParameter(SourceLoc loc, enum Kind kind, Value value)
    : Loc(loc), Kind(kind), V(value) {}

  static AutoDiffParameter getIndexParameter(SourceLoc loc, unsigned index) {
    return { loc, Kind::Index, { index } };
  }

  static AutoDiffParameter getSelfParameter(SourceLoc loc) {
    return { loc, Kind::Self, {} };
  }

  unsigned getIndex() const {
    assert(Kind == Kind::Index);
    return V.Index;
  }

  enum Kind getKind() const {
    return Kind;
  }

  SourceLoc getLoc() const {
    return Loc;
  }

  bool isEqual(AutoDiffParameter other) const {
    if (getKind() == other.getKind() && getKind() == Kind::Index)
      return getIndex() == other.getIndex();
    return getKind() == other.getKind() && getKind() == Kind::Self;
  }
};

/// SIL-level automatic differentiation configuration.
struct SILReverseAutoDiffConfiguration {
  unsigned sourceIndex;
  ArrayRef<unsigned> parameterIndices;
  bool seedable;
  bool preservingResult;

  /// Returns the "master" configuration, which all variants with the same
  /// parameter indices can derive from.
  static
  SILReverseAutoDiffConfiguration getMaster(unsigned sourceIndex,
                                            ArrayRef<unsigned> paramIndices) {
    return { sourceIndex, paramIndices,
             /*seedable*/ true, /*preservingResult*/ true };
  }

  bool isEqual(const SILReverseAutoDiffConfiguration &other) const {
    return sourceIndex == other.sourceIndex &&
           parameterIndices.equals(other.parameterIndices) &&
           seedable == other.seedable &&
           preservingResult == other.preservingResult;
  }

  bool isMaster() const {
    return seedable && preservingResult;
  }
};

} // end namespace swift

namespace llvm {

using swift::SILReverseAutoDiffConfiguration;

template<typename T> struct DenseMapInfo;

template<> struct DenseMapInfo<SILReverseAutoDiffConfiguration> {
  static SILReverseAutoDiffConfiguration getEmptyKey() {
    return { DenseMapInfo<unsigned>::getEmptyKey(),
             DenseMapInfo<ArrayRef<unsigned>>::getEmptyKey(),
             static_cast<bool>(DenseMapInfo<unsigned>::getEmptyKey()),
             static_cast<bool>(DenseMapInfo<unsigned>::getEmptyKey()) };
  }

  static SILReverseAutoDiffConfiguration getTombstoneKey() {
    return { DenseMapInfo<unsigned>::getTombstoneKey(),
             DenseMapInfo<ArrayRef<unsigned>>::getTombstoneKey(),
             static_cast<bool>(DenseMapInfo<unsigned>::getTombstoneKey()),
             static_cast<bool>(DenseMapInfo<unsigned>::getTombstoneKey()) };
  }

  static unsigned getHashValue(const SILReverseAutoDiffConfiguration &Val) {
    unsigned paramHash = ~1U;
    for (auto i : Val.parameterIndices)
      paramHash = hash_combine(paramHash,
                               DenseMapInfo<unsigned>::getHashValue(i));
    return hash_combine(
      paramHash,
      DenseMapInfo<unsigned>::getHashValue(Val.seedable),
      DenseMapInfo<unsigned>::getHashValue(Val.preservingResult)
    );
  }

  static bool isEqual(const SILReverseAutoDiffConfiguration &LHS,
                      const SILReverseAutoDiffConfiguration &RHS) {
    auto numParams = LHS.parameterIndices.size();
    if (numParams != RHS.parameterIndices.size())
      return false;
    for (unsigned i = 0; i < numParams; i++)
      if (LHS.parameterIndices[i] != RHS.parameterIndices[i])
        return false;
    return LHS.seedable == RHS.seedable &&
           LHS.preservingResult == LHS.preservingResult;
  }
};

} // end namespace llvm

#endif // SWIFT_AST_AUTODIFF_H

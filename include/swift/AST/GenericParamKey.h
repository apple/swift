//===--- GenericParamKey.h - Key for generic parameters ---------*- C++ -*-===//
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

#ifndef SWIFT_AST_GENERICPARAMKEY_H
#define SWIFT_AST_GENERICPARAMKEY_H

#include "llvm/ADT/DenseMapInfo.h"
#include "swift/AST/Type.h"

namespace swift {

class GenericTypeParamDecl;
class GenericTypeParamType;

/// A fully-abstracted generic type parameter key, maintaining only the depth
/// and index of the generic parameter.
struct GenericParamKey {
  unsigned ParameterPack : 1;
  unsigned Depth : 15;
  unsigned Index : 16;

  GenericParamKey(bool isParameterPack, unsigned depth, unsigned index)
      : ParameterPack(isParameterPack), Depth(depth), Index(index) {}

  GenericParamKey(const GenericTypeParamDecl *d);
  GenericParamKey(const GenericTypeParamType *d);

  friend bool operator==(GenericParamKey lhs, GenericParamKey rhs) {
    return lhs.ParameterPack == rhs.ParameterPack &&
           lhs.Depth == rhs.Depth &&
           lhs.Index == rhs.Index;
  }

  friend bool operator!=(GenericParamKey lhs, GenericParamKey rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(GenericParamKey lhs, GenericParamKey rhs) {
    return lhs.Depth < rhs.Depth ||
      (lhs.Depth == rhs.Depth && lhs.Index < rhs.Index);
  }

  friend bool operator>(GenericParamKey lhs, GenericParamKey rhs) {
    return rhs < lhs;
  }

  friend bool operator<=(GenericParamKey lhs, GenericParamKey rhs) {
    return !(rhs < lhs);
  }

  friend bool operator>=(GenericParamKey lhs, GenericParamKey rhs) {
    return !(lhs < rhs);
  }

  /// Function object type that can be used to provide an ordering of
  /// generic type parameter keys with themselves, generic type parameter
  /// declarations, and generic type parameter types.
  struct Ordering {
    bool operator()(GenericParamKey lhs, GenericParamKey rhs) const {
      return lhs < rhs;
    }

    bool operator()(GenericParamKey lhs,
                    const GenericTypeParamDecl *rhs) const {
      return (*this)(lhs, GenericParamKey(rhs));
    }

    bool operator()(const GenericTypeParamDecl *lhs,
                    GenericParamKey rhs) const {
      return (*this)(GenericParamKey(lhs), rhs);
    }

    bool operator()(GenericParamKey lhs,
                    const GenericTypeParamType *rhs) const {
      return (*this)(lhs, GenericParamKey(rhs));
    }

    bool operator()(const GenericTypeParamType *lhs,
                    GenericParamKey rhs) const {
      return (*this)(GenericParamKey(lhs), rhs);
    }
  };


  /// Find the index that this key would have into an array of
  /// generic type parameters
  unsigned findIndexIn(ArrayRef<GenericTypeParamType *> genericParams) const;
};

} // end namespace swift

namespace llvm {

template<>
struct DenseMapInfo<swift::GenericParamKey> {
  static inline swift::GenericParamKey getEmptyKey() {
    return {true, 0xFFFF, 0xFFFF};
  }
  static inline swift::GenericParamKey getTombstoneKey() {
    return {true, 0xFFFE, 0xFFFE};
  }

  static inline unsigned getHashValue(swift::GenericParamKey k) {
    return DenseMapInfo<unsigned>::getHashValue(
        k.Depth << 16 | k.Index | ((k.ParameterPack ? 1 : 0) << 30));
  }
  static bool isEqual(swift::GenericParamKey a,
                      swift::GenericParamKey b) {
    return a.ParameterPack == b.ParameterPack &&
           a.Depth == b.Depth &&
           a.Index == b.Index;
  }
};
  
} // end namespace llvm

#endif // SWIFT_AST_GENERICPARAMKEY_H

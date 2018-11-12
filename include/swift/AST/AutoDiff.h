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
#include "llvm/ADT/SmallBitVector.h"

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
    return { loc, Kind::Index, index };
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

  bool isEqual(const AutoDiffParameter &other) const {
    if (getKind() == other.getKind() && getKind() == Kind::Index)
      return getIndex() == other.getIndex();
    return getKind() == other.getKind() && getKind() == Kind::Self;
  }
};

class AnyFunctionType;

/// Differentiability of a function specifies the differentiation mode,
/// parameter indices at which the function is differentiable with respect to,
/// and indices of results which can be differentiated.
class Differentiability {
private:
  // The differentiation mode.
  AutoDiffMode mode;
  // Differentiable with respect to `self`, applicable to methods only.
  bool wrtSelf;
  // Indices of parameters that are differentiable with respect to.
  llvm::SmallBitVector parameterIndices;
  // Indices of results that are differentiable.
  llvm::SmallBitVector resultIndices;

public:
  Differentiability(AutoDiffMode mode,
                    bool wrtSelf,
                    llvm::SmallBitVector parameterIndices,
                    llvm::SmallBitVector resultIndices);

  Differentiability(AutoDiffMode mode, AnyFunctionType *type);

  AutoDiffMode getMode() const {
    return mode;
  }

  bool isWithRespectToSelf() const {
    return wrtSelf;
  }

  const llvm::SmallBitVector &getParameterIndices() const {
    return parameterIndices;
  }

  const llvm::SmallBitVector &getResultIndices() const {
    return resultIndices;
  }
};

/// SIL-level automatic differentiation indices. Consists of a source index,
/// i.e. index of the dependent result to differentiate from, and parameter
/// indices, i.e. index of independent parameters to differentiate with
/// respect to.
struct SILAutoDiffIndices {
  /// The index of the dependent result to differentiate from.
  unsigned source;
  /// Indices of independent parameters to differentiate with respect to.
  llvm::SmallBitVector parameters;

  /// Creates a set of AD indices from the given source index and a bit vector
  /// representing parameter indices.
  /*implicit*/ SILAutoDiffIndices(unsigned source,
                                  llvm::SmallBitVector parameters)
      : source(source), parameters(parameters) {}

  /// Creates a set of AD indices from the given source index and an array of
  /// parameter indices. Elements in `parameters` must be acending integers.
  /*implicit*/ SILAutoDiffIndices(unsigned source,
                                  ArrayRef<unsigned> parameters);

  bool operator==(const SILAutoDiffIndices &other) const;

  /// Queries whether the function's parameter with index `parameterIndex` is
  /// one of the parameters to differentiate with respect to.
  bool isWrtParameter(unsigned parameterIndex) const {
    return parameterIndex < parameters.size() &&
           parameters.test(parameterIndex);
  }

  void print(llvm::raw_ostream &s = llvm::outs()) const {
    s << "(source=" << source << " parameters=(";
    interleave(parameters.set_bits(),
               [&s](unsigned p) { s << p; }, [&s]{ s << ' '; });
    s << "))";
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &s,
                                     const SILAutoDiffIndices &indices) {
  indices.print(s);
  return s;
}

/// Flags to define the semantics and the type signature of a gradient function.
enum class SILGradientFlags : unsigned {
  /// The gradient function is seedable, i.e. able to take a back-propagated
  /// adjoint value as the last parameter.
  Seedable = 1 << 0,
  
  /// The gradient function is preserving the result of the original function.
  PreservingResult = 1 << 1,
  
  /// The adjoint computation is "delayed". We say that the adjoint computation
  /// is delayed when when it's returned as a thunk.
  Delayed = 1 << 2
};
using SILGradientOptions = OptionSet<SILGradientFlags>;
static inline SILGradientOptions operator|(SILGradientFlags lhs,
                                           SILGradientFlags rhs) {
  return SILGradientOptions(unsigned(lhs) | unsigned(rhs));
}

/// SIL-level automatic differentiation configuration.
struct SILAutoDiffConfig {
  SILAutoDiffIndices indices;
  SILGradientOptions options;

  /*implicit*/
  SILAutoDiffConfig(const SILAutoDiffIndices &indices,
                    SILGradientOptions options)
    : indices(indices), options(options) {}

  /*implicit*/
  SILAutoDiffConfig(const SILAutoDiffIndices &indices,
                    bool seedable, bool preservingResult)
    : SILAutoDiffConfig(indices, getCanonicalGradientOptions()) {}

  unsigned getSourceIndex() const {
    return indices.source;
  }

  llvm::SmallBitVector getParameterIndices() const {
    return indices.parameters;
  }

  bool isSeedable() const {
    return options.contains(SILGradientFlags::Seedable);
  }

  bool isPreservingResult() const {
    return options.contains(SILGradientFlags::PreservingResult);
  }

  bool isDelayed() const {
    return options.contains(SILGradientFlags::Delayed);
  }

  // FIXME: The master configuration should have all three gradient options
  // enabled, that is, the canonical gradient should return a delayed gradient
  // function. We need to handle this here as well as within the
  // differentiation pass.
  static SILGradientOptions getCanonicalGradientOptions() {
    return SILGradientFlags::Seedable | SILGradientFlags::PreservingResult;
  }

  /// Returns the "master" configuration, which all variants with the same
  /// parameter indices can derive from.
  static
  SILAutoDiffConfig getMaster(
      const SILAutoDiffIndices &indices) {
    return {
      indices,
      getCanonicalGradientOptions()
    };
  }

  SILAutoDiffConfig getWithCanonicalOptions() const {
    return getMaster(indices);
  }

  bool isMaster() const {
    return options.toRaw() == getCanonicalGradientOptions().toRaw();
  }

  bool operator==(const SILAutoDiffConfig &other) const {
    return indices == other.indices &&
           options.toRaw() == other.options.toRaw();
  }
};

/// The kind of an associated function in the `autodiff_function` and
/// `autodiff_extract` instructions in SIL.
enum class SILAutoDiffAssociatedFunctionKind {
  // The primal function in legacy reverse-mode.
  LegacyPrimal,
  // The adjoint function in legacy reverse-mode.
  LegacyAdjoint,
  // The vector-Jacobian products operator.
  JVP,
  // The differential, the result of JVP except that it has boxed arguments
  // (closure captures) for partial application within JVP.
  Differential,
  // The Jacobian-vector products operator.
  VJP,
  // The pullback, the result of JVP except that it has boxed arguments
  // (closure captures) for partial application within VJP.
  Pullback
};

/// Automatic differentiation utility namespace.
namespace autodiff {

/// Returns the required number of associated functions per differentiation
/// order.
unsigned getNumAutoDiffAssociatedFunctionsPerOrder(bool isLegacyReverseMode);

/// Returns the offset for an associated function at a specific differentiation
/// order.
/// This is used for both ordering in the `autodiff_function` instruction and
/// ABI layout.
///
/// |---------------------------------------------------------------|
/// | 1. Standard mode.                                             |
/// |---------------------------------------------------------------|
/// |              Order 1               Order 2                 ...|
/// |----------| |-----|----|-----|----| |-----|----|-----|----| ...|
/// | Original | | JVP | DF | VJP | PB | | JVP | DF | VJP | PB | ...|
/// |----------| |-----|----|-----|----| |-----|----|-----|----| ...|
/// |---------------------------------------------------------------|
/// | 2. Legacy reverse mode.                                       |
/// |---------------------------------------------------------------|
/// |              Order 1                                          |
/// |----------| |--------|---------|                               |
/// | Original | | Primal | Adjoint |                               |
/// |----------| |--------|---------|                               |
/// |---------------------------------------------------------------|
unsigned
getOffsetForAutoDiffAssociatedFunction(unsigned order,
                                       SILAutoDiffAssociatedFunctionKind kind);

} // end namespace autodiff

class BuiltinFloatType;
class NominalTypeDecl;
class StructDecl;
class TupleType;
class EnumDecl;

/// A type that represents the tangent space of a differentiable type.
class TangentSpace {
public:
  /// A tangent space kind.
  enum class Kind {
    /// `Builtin.FP<...>`.
    BuiltinRealScalar,
    /// A type that conforms to `FloatingPoint`.
    RealScalar,
    /// A type that conforms to `VectorNumeric` where the associated
    /// `ScalarElement` conforms to `FloatingPoint`.
    RealVector,
    /// A product of tangent spaces as a struct.
    ProductStruct,
    /// A product of tangent spaces as a tuple.
    ProductTuple,
    /// A sum of tangent spaces.
    Sum
  };

private:
  Kind kind;
  union Value {
    // BuiltinRealScalar
    BuiltinFloatType *builtinFPType;
    // RealScalar or RealVector
    NominalTypeDecl *realNominalType;
    // ProductStruct
    StructDecl *structDecl;
    // ProductTuple
    TupleType *tupleType;
    // Sum
    EnumDecl *enumDecl;

    Value(BuiltinFloatType *builtinFP) : builtinFPType(builtinFP) {}
    Value(NominalTypeDecl *nominal) : realNominalType(nominal) {}
    Value(StructDecl *structDecl) : structDecl(structDecl) {}
    Value(TupleType *tupleType) : tupleType(tupleType) {}
    Value(EnumDecl *enumDecl) : enumDecl(enumDecl) {}
  } value;

  TangentSpace(Kind kind, Value value)
      : kind(kind), value(value) {}

public:
  TangentSpace() = delete;

  static TangentSpace
  getBuiltinRealScalarSpace(BuiltinFloatType *builtinFP) {
    return {Kind::BuiltinRealScalar, builtinFP};
  }
  static TangentSpace getRealScalarSpace(NominalTypeDecl *typeDecl) {
    return {Kind::RealScalar, typeDecl};
  }
  static TangentSpace getRealVectorSpace(NominalTypeDecl *typeDecl) {
    return {Kind::RealVector, typeDecl};
  }
  static TangentSpace getProductStruct(StructDecl *structDecl) {
    return {Kind::ProductStruct, structDecl};
  }
  static TangentSpace getProductTuple(TupleType *tupleTy) {
    return {Kind::ProductTuple, tupleTy};
  }
  static TangentSpace getSum(EnumDecl *enumDecl) {
    return {Kind::Sum, enumDecl};
  }

  bool isBuiltinRealScalarSpace() const {
    return kind == Kind::BuiltinRealScalar;
  }
  bool isRealScalarSpace() const { return kind == Kind::RealScalar; }
  bool isRealVectorSpace() const { return kind == Kind::RealVector; }
  bool isProductStruct() const { return kind == Kind::ProductStruct; }
  bool isProductTuple() const { return kind == Kind::ProductTuple; }

  Kind getKind() const { return kind; }
  BuiltinFloatType *getBuiltinRealScalarSpace() const {
    assert(kind == Kind::BuiltinRealScalar);
    return value.builtinFPType;
  }
  NominalTypeDecl *getRealScalarSpace() const {
    assert(kind == Kind::RealScalar);
    return value.realNominalType;
  }
  NominalTypeDecl *getRealVectorSpace() const {
    assert(kind == Kind::RealVector);
    return value.realNominalType;
  }
  NominalTypeDecl *getRealScalarOrVectorSpace() const {
    assert(kind == Kind::RealScalar || kind == Kind::RealVector);
    return value.realNominalType;
  }
  StructDecl *getProductStruct() const {
    assert(kind == Kind::ProductStruct);
    return value.structDecl;
  }
  TupleType *getProductTuple() const {
    assert(kind == Kind::ProductTuple);
    return value.tupleType;
  }
  EnumDecl *getSum() const {
    assert(kind == Kind::Sum);
    return value.enumDecl;
  }
};

} // end namespace swift

namespace llvm {

using swift::SILAutoDiffIndices;
using swift::SILAutoDiffConfig;
using swift::SILGradientFlags;
using swift::OptionSet;

template<typename T> struct DenseMapInfo;

template<> struct DenseMapInfo<SILAutoDiffIndices> {
  static SILAutoDiffIndices getEmptyKey() {
    return { DenseMapInfo<unsigned>::getEmptyKey(), SmallBitVector() };
  }

  static SILAutoDiffIndices getTombstoneKey() {
    return { DenseMapInfo<unsigned>::getTombstoneKey(),
             SmallBitVector(sizeof(intptr_t), true) };
  }

  static unsigned getHashValue(const SILAutoDiffIndices &Val) {
    auto params = Val.parameters.set_bits();
    unsigned combinedHash =
      hash_combine(~1U, DenseMapInfo<unsigned>::getHashValue(Val.source),
                   hash_combine_range(params.begin(), params.end()));
    return combinedHash;
  }

  static bool isEqual(const SILAutoDiffIndices &LHS,
                      const SILAutoDiffIndices &RHS) {
    return LHS == RHS;
  }
};

template<> struct DenseMapInfo<SILAutoDiffConfig> {
  static SILAutoDiffConfig getEmptyKey() {
    return { DenseMapInfo<SILAutoDiffIndices>::getEmptyKey(), None };
  }

  static SILAutoDiffConfig getTombstoneKey() {
    return {
      DenseMapInfo<SILAutoDiffIndices>::getTombstoneKey(),
      SILGradientFlags::Delayed
    };
  }

  static unsigned getHashValue(const SILAutoDiffConfig &Val) {
    return hash_combine(
      DenseMapInfo<SILAutoDiffIndices>::getHashValue(Val.indices),
      DenseMapInfo<unsigned>::getHashValue(Val.options.toRaw())
    );
  }

  static bool isEqual(const SILAutoDiffConfig &LHS,
                      const SILAutoDiffConfig &RHS) {
    return DenseMapInfo<SILAutoDiffIndices>
             ::isEqual(LHS.indices, RHS.indices) &&
           LHS.options.toRaw() == RHS.options.toRaw();
  }
};

} // end namespace llvm

#endif // SWIFT_AST_AUTODIFF_H

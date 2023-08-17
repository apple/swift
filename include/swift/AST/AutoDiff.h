//===--- AutoDiff.h - Swift automatic differentiation utilities -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines utilities for automatic differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_AUTODIFF_H
#define SWIFT_AST_AUTODIFF_H

#include <cstdint>

#include "swift/AST/GenericSignature.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/IndexSubset.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Demangling/Demangle.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

namespace swift {

class AbstractFunctionDecl;
class AnyFunctionType;
class SourceFile;
class SILFunctionType;
class TupleType;
class VarDecl;

/// A function type differentiability kind.
enum class DifferentiabilityKind : uint8_t {
  NonDifferentiable = 0,
  // '@differentiable(_forward)', rejected by parser.
  Forward = 1,
  // '@differentiable(reverse)', supported.
  Reverse = 2,
  // '@differentiable', unsupported.
  Normal = 3,
  // '@differentiable(_linear)', unsupported.
  Linear = 4,
};

/// The kind of an linear map.
struct AutoDiffLinearMapKind {
  enum innerty : uint8_t {
    // The differential function.
    Differential = 0,
    // The pullback function.
    Pullback = 1
  } rawValue;

  AutoDiffLinearMapKind() = default;
  AutoDiffLinearMapKind(innerty rawValue) : rawValue(rawValue) {}
  operator innerty() const { return rawValue; }
};

/// The kind of a derivative function.
struct AutoDiffDerivativeFunctionKind {
  enum innerty : uint8_t {
    // The Jacobian-vector products function.
    JVP = 0,
    // The vector-Jacobian products function.
    VJP = 1
  } rawValue;

  AutoDiffDerivativeFunctionKind() = default;
  AutoDiffDerivativeFunctionKind(innerty rawValue) : rawValue(rawValue) {}
  AutoDiffDerivativeFunctionKind(AutoDiffLinearMapKind linMapKind)
      : rawValue(static_cast<innerty>(linMapKind.rawValue)) {}
  explicit AutoDiffDerivativeFunctionKind(StringRef string);
  operator innerty() const { return rawValue; }
  AutoDiffLinearMapKind getLinearMapKind() const {
    return (AutoDiffLinearMapKind::innerty)rawValue;
  }
  DifferentiabilityKind getMinimalDifferentiabilityKind() const {
    switch (rawValue) {
    case JVP: return DifferentiabilityKind::Forward;
    case VJP: return DifferentiabilityKind::Reverse;
    }
  }
};

/// A component of a SIL `@differentiable` function-typed value.
struct NormalDifferentiableFunctionTypeComponent {
  enum innerty : unsigned { Original = 0, JVP = 1, VJP = 2 } rawValue;

  NormalDifferentiableFunctionTypeComponent() = default;
  NormalDifferentiableFunctionTypeComponent(innerty rawValue)
      : rawValue(rawValue) {}
  NormalDifferentiableFunctionTypeComponent(
      AutoDiffDerivativeFunctionKind kind);
  explicit NormalDifferentiableFunctionTypeComponent(unsigned rawValue)
      : NormalDifferentiableFunctionTypeComponent((innerty)rawValue) {}
  explicit NormalDifferentiableFunctionTypeComponent(StringRef name);
  operator innerty() const { return rawValue; }

  /// Returns the derivative function kind, if the component is a derivative
  /// function.
  llvm::Optional<AutoDiffDerivativeFunctionKind>
  getAsDerivativeFunctionKind() const;
};

/// A component of a SIL `@differentiable(_linear)` function-typed value.
struct LinearDifferentiableFunctionTypeComponent {
  enum innerty : unsigned {
    Original = 0,
    Transpose = 1,
  } rawValue;

  LinearDifferentiableFunctionTypeComponent() = default;
  LinearDifferentiableFunctionTypeComponent(innerty rawValue)
      : rawValue(rawValue) {}
  explicit LinearDifferentiableFunctionTypeComponent(unsigned rawValue)
      : LinearDifferentiableFunctionTypeComponent((innerty)rawValue) {}
  explicit LinearDifferentiableFunctionTypeComponent(StringRef name);
  operator innerty() const { return rawValue; }
};

/// A derivative function configuration, uniqued in `ASTContext`.
/// Identifies a specific derivative function given an original function.
class AutoDiffDerivativeFunctionIdentifier : public llvm::FoldingSetNode {
  const AutoDiffDerivativeFunctionKind kind;
  IndexSubset *const parameterIndices;
  GenericSignature derivativeGenericSignature;

  AutoDiffDerivativeFunctionIdentifier(
      AutoDiffDerivativeFunctionKind kind, IndexSubset *parameterIndices,
      GenericSignature derivativeGenericSignature)
      : kind(kind), parameterIndices(parameterIndices),
        derivativeGenericSignature(derivativeGenericSignature) {}

public:
  AutoDiffDerivativeFunctionKind getKind() const { return kind; }
  IndexSubset *getParameterIndices() const { return parameterIndices; }
  GenericSignature getDerivativeGenericSignature() const {
    return derivativeGenericSignature;
  }

  static AutoDiffDerivativeFunctionIdentifier *
  get(AutoDiffDerivativeFunctionKind kind, IndexSubset *parameterIndices,
      GenericSignature derivativeGenericSignature, ASTContext &C);

  void Profile(llvm::FoldingSetNodeID &ID) {
    ID.AddInteger(kind);
    ID.AddPointer(parameterIndices);
    auto derivativeCanGenSig =
        derivativeGenericSignature.getCanonicalSignature();
    ID.AddPointer(derivativeCanGenSig.getPointer());
  }
};

/// The kind of a differentiability witness function.
struct DifferentiabilityWitnessFunctionKind {
  enum innerty : uint8_t {
    // The Jacobian-vector products function.
    JVP = 0,
    // The vector-Jacobian products function.
    VJP = 1,
    // The transpose function.
    Transpose = 2
  } rawValue;

  DifferentiabilityWitnessFunctionKind() = default;
  DifferentiabilityWitnessFunctionKind(innerty rawValue) : rawValue(rawValue) {}
  explicit DifferentiabilityWitnessFunctionKind(unsigned rawValue)
      : rawValue(static_cast<innerty>(rawValue)) {}
  explicit DifferentiabilityWitnessFunctionKind(StringRef name);
  operator innerty() const { return rawValue; }

  llvm::Optional<AutoDiffDerivativeFunctionKind>
  getAsDerivativeFunctionKind() const;
};

/// The kind of a declaration generated by the differentiation transform.
enum class AutoDiffGeneratedDeclarationKind : uint8_t {
  LinearMapStruct,
  BranchingTraceEnum
};

/// Identifies an autodiff derivative function configuration:
/// - Parameter indices.
/// - Result indices.
/// - Derivative generic signature (optional).
struct AutoDiffConfig {
  IndexSubset *parameterIndices;
  IndexSubset *resultIndices;
  GenericSignature derivativeGenericSignature;

  /*implicit*/ AutoDiffConfig() = default;
  /*implicit*/ AutoDiffConfig(
      IndexSubset *parameterIndices, IndexSubset *resultIndices,
      GenericSignature derivativeGenericSignature = GenericSignature())
      : parameterIndices(parameterIndices), resultIndices(resultIndices),
        derivativeGenericSignature(derivativeGenericSignature) {}

  /// Returns true if `parameterIndex` is a differentiability parameter index.
  bool isWrtParameter(unsigned parameterIndex) const {
    return parameterIndex < parameterIndices->getCapacity() &&
           parameterIndices->contains(parameterIndex);
  }

  /// Returns true if `resultIndex` is a differentiability result index.
  bool isWrtResult(unsigned resultIndex) const {
    return resultIndex < resultIndices->getCapacity() &&
           resultIndices->contains(resultIndex);
  }

  AutoDiffConfig withGenericSignature(GenericSignature signature) const {
    return AutoDiffConfig(parameterIndices, resultIndices, signature);
  }

  // TODO(https://github.com/apple/swift/issues/52204): Use principled mangling for AD-generated symbols.
  std::string mangle() const {
    std::string result = "src_";
    interleave(
        resultIndices->getIndices(),
        [&](unsigned idx) { result += llvm::utostr(idx); },
        [&] { result += '_'; });
    result += "_wrt_";
    llvm::interleave(
        parameterIndices->getIndices(),
        [&](unsigned idx) { result += llvm::utostr(idx); },
        [&] { result += '_'; });
    return result;
  }

  void print(llvm::raw_ostream &s = llvm::outs()) const;
  SWIFT_DEBUG_DUMP;
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &s,
                                     const AutoDiffConfig &config) {
  config.print(s);
  return s;
}

/// A semantic function result type: either a formal function result type or a
/// semantic result (an `inout`) parameter type. Used in derivative function type
/// calculation.
struct AutoDiffSemanticFunctionResultType {
  Type type;
  unsigned index : 30;
  bool isSemanticResultParameter : 1;

  AutoDiffSemanticFunctionResultType(Type t, unsigned idx, bool param)
    : type(t), index(idx), isSemanticResultParameter(param) { }
};

/// Key for caching SIL derivative function types.
struct SILAutoDiffDerivativeFunctionKey {
  SILFunctionType *originalType;
  IndexSubset *parameterIndices;
  IndexSubset *resultIndices;
  AutoDiffDerivativeFunctionKind kind;
  CanGenericSignature derivativeFnGenSig;
  bool isReabstractionThunk;
};

class ParsedAutoDiffParameter {
public:
  enum class Kind { Named, Ordered, Self };

private:
  SourceLoc loc;
  Kind kind;
  union Value {
    struct { Identifier name; } Named;
    struct { unsigned index; } Ordered;
    struct {} self;
    Value(Identifier name) : Named({name}) {}
    Value(unsigned index) : Ordered({index}) {}
    Value() {}
  } value;

public:
  ParsedAutoDiffParameter(SourceLoc loc, Kind kind, Value value)
    : loc(loc), kind(kind), value(value) {}

  ParsedAutoDiffParameter(SourceLoc loc, Kind kind, unsigned index)
    : loc(loc), kind(kind), value(index) {}

  static ParsedAutoDiffParameter getNamedParameter(SourceLoc loc,
                                                   Identifier name) {
    return { loc, Kind::Named, name };
  }

  static ParsedAutoDiffParameter getOrderedParameter(SourceLoc loc,
                                                     unsigned index) {
    return { loc, Kind::Ordered, index };
  }

  static ParsedAutoDiffParameter getSelfParameter(SourceLoc loc) {
    return { loc, Kind::Self, {} };
  }

  Identifier getName() const {
    assert(kind == Kind::Named);
    return value.Named.name;
  }

  unsigned getIndex() const {
    return value.Ordered.index;
  }

  Kind getKind() const {
    return kind;
  }

  SourceLoc getLoc() const {
    return loc;
  }

  bool isEqual(const ParsedAutoDiffParameter &other) const {
    if (getKind() != other.getKind())
      return false;
    if (getKind() == Kind::Named)
      return getName() == other.getName();
    return getKind() == Kind::Self;
  }
};

/// The tangent space of a type.
///
/// For `Differentiable`-conforming types:
/// - The tangent space is the `TangentVector` associated type.
///
/// For tuple types:
/// - The tangent space is a tuple of the elements' tangent space types, for the
///   elements that have a tangent space.
///
/// Other types have no tangent space.
class TangentSpace {
public:
  /// A tangent space kind.
  enum class Kind {
    /// The `TangentVector` associated type of a `Differentiable`-conforming
    /// type.
    TangentVector,
    /// A product of tangent spaces as a tuple.
    Tuple
  };

private:
  Kind kind;
  union Value {
    // TangentVector
    Type tangentVectorType;
    // Tuple
    TupleType *tupleType;

    Value(Type tangentVectorType) : tangentVectorType(tangentVectorType) {}
    Value(TupleType *tupleType) : tupleType(tupleType) {}
  } value;

  TangentSpace(Kind kind, Value value) : kind(kind), value(value) {}

public:
  TangentSpace() = delete;

  static TangentSpace getTangentVector(Type tangentVectorType) {
    return {Kind::TangentVector, tangentVectorType};
  }
  static TangentSpace getTuple(TupleType *tupleTy) {
    return {Kind::Tuple, tupleTy};
  }

  bool isTangentVector() const { return kind == Kind::TangentVector; }
  bool isTuple() const { return kind == Kind::Tuple; }

  Kind getKind() const { return kind; }
  Type getTangentVector() const {
    assert(kind == Kind::TangentVector);
    return value.tangentVectorType;
  }
  TupleType *getTuple() const {
    assert(kind == Kind::Tuple);
    return value.tupleType;
  }

  /// Get the tangent space type.
  Type getType() const;

  /// Get the tangent space canonical type.
  CanType getCanonicalType() const;

  /// Get the underlying nominal type declaration of the tangent space type.
  NominalTypeDecl *getNominal() const;
};

/// A derivative function type calculation error.
class DerivativeFunctionTypeError
    : public llvm::ErrorInfo<DerivativeFunctionTypeError> {
public:
  enum class Kind {
    /// Original function type has no semantic results.
    NoSemanticResults,
    /// Differentiability parmeter indices are empty.
    NoDifferentiabilityParameters,
    /// A differentiability parameter does not conform to `Differentiable`.
    NonDifferentiableDifferentiabilityParameter,
    /// The original result type does not conform to `Differentiable`.
    NonDifferentiableResult
  };

  static const char ID;
  /// The original function type.
  AnyFunctionType *functionType;
  /// The error kind.
  Kind kind;

  /// The type and index of a differentiability parameter or result.
  using TypeAndIndex = std::pair<Type, unsigned>;

private:
  union Value {
    TypeAndIndex typeAndIndex;
    Value(TypeAndIndex typeAndIndex) : typeAndIndex(typeAndIndex) {}
    Value() {}
  } value;

public:
  explicit DerivativeFunctionTypeError(AnyFunctionType *functionType, Kind kind)
      : functionType(functionType), kind(kind), value(Value()) {
    assert(kind == Kind::NoSemanticResults ||
           kind == Kind::NoDifferentiabilityParameters);
  };

  explicit DerivativeFunctionTypeError(AnyFunctionType *functionType, Kind kind,
                                       TypeAndIndex nonDiffTypeAndIndex)
      : functionType(functionType), kind(kind), value(nonDiffTypeAndIndex) {
    assert(kind == Kind::NonDifferentiableDifferentiabilityParameter ||
           kind == Kind::NonDifferentiableResult);
  };

  TypeAndIndex getNonDifferentiableTypeAndIndex() const {
    assert(kind == Kind::NonDifferentiableDifferentiabilityParameter ||
           kind == Kind::NonDifferentiableResult);
    return value.typeAndIndex;
  }

  void log(raw_ostream &OS) const override;

  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};

/// Describes the "tangent stored property" corresponding to an original stored
/// property in a `Differentiable`-conforming type.
///
/// The tangent stored property is the stored property in the `TangentVector`
/// struct of the `Differentiable`-conforming type, with the same name as the
/// original stored property and with the original stored property's
/// `TangentVector` type.
struct TangentPropertyInfo {
  struct Error {
    enum class Kind {
      /// The original property is `@noDerivative`.
      NoDerivativeOriginalProperty,
      /// The nominal parent type does not conform to `Differentiable`.
      NominalParentNotDifferentiable,
      /// The original property's type does not conform to `Differentiable`.
      OriginalPropertyNotDifferentiable,
      /// The parent `TangentVector` type is not a struct.
      ParentTangentVectorNotStruct,
      /// The parent `TangentVector` struct does not declare a stored property
      /// with the same name as the original property.
      TangentPropertyNotFound,
      /// The tangent property's type is not equal to the original property's
      /// `TangentVector` type.
      TangentPropertyWrongType,
      /// The tangent property is not a stored property.
      TangentPropertyNotStored
    };

    /// The error kind.
    Kind kind;

  private:
    union Value {
      Type type;
      Value(Type type) : type(type) {}
      Value() {}
    } value;

  public:
    Error(Kind kind) : kind(kind), value() {
      assert(kind == Kind::NoDerivativeOriginalProperty ||
             kind == Kind::NominalParentNotDifferentiable ||
             kind == Kind::OriginalPropertyNotDifferentiable ||
             kind == Kind::ParentTangentVectorNotStruct ||
             kind == Kind::TangentPropertyNotFound ||
             kind == Kind::TangentPropertyNotStored);
    };

    Error(Kind kind, Type type) : kind(kind), value(type) {
      assert(kind == Kind::TangentPropertyWrongType);
    };

    Type getType() const {
      assert(kind == Kind::TangentPropertyWrongType);
      return value.type;
    }

    friend bool operator==(const Error &lhs, const Error &rhs);
  };

  /// The tangent stored property.
  VarDecl *tangentProperty = nullptr;

  /// An optional error.
  llvm::Optional<Error> error = llvm::None;

private:
  TangentPropertyInfo(VarDecl *tangentProperty, llvm::Optional<Error> error)
      : tangentProperty(tangentProperty), error(error) {}

public:
  TangentPropertyInfo(VarDecl *tangentProperty)
      : TangentPropertyInfo(tangentProperty, llvm::None) {}

  TangentPropertyInfo(Error::Kind errorKind)
      : TangentPropertyInfo(nullptr, Error(errorKind)) {}

  TangentPropertyInfo(Error::Kind errorKind, Type errorType)
      : TangentPropertyInfo(nullptr, Error(errorKind, errorType)) {}

  /// Returns `true` iff this tangent property info is valid.
  bool isValid() const { return tangentProperty && !error; }

  explicit operator bool() const { return isValid(); }

  friend bool operator==(const TangentPropertyInfo &lhs,
                         const TangentPropertyInfo &rhs) {
    return lhs.tangentProperty == rhs.tangentProperty && lhs.error == rhs.error;
  }
};

void simple_display(llvm::raw_ostream &OS, TangentPropertyInfo info);

/// The key type used for uniquing `SILDifferentiabilityWitness` in `SILModule`.
struct SILDifferentiabilityWitnessKey {
  StringRef originalFunctionName;
  DifferentiabilityKind kind;
  AutoDiffConfig config;

  void print(llvm::raw_ostream &s = llvm::outs()) const;
};

inline llvm::raw_ostream &operator<<(
    llvm::raw_ostream &s, const SILDifferentiabilityWitnessKey &key) {
  key.print(s);
  return s;
}

/// Returns `true` iff differentiable programming is enabled.
bool isDifferentiableProgrammingEnabled(SourceFile &SF);

/// Automatic differentiation utility namespace.
namespace autodiff {

/// Given a function type, collects its semantic result types in type order
/// into `result`: first, the formal result type (if non-`Void`), followed by
/// `inout` parameter types.
///
/// The function type may have at most two parameter lists.
void getFunctionSemanticResults(
    const AnyFunctionType *functionType,
    const IndexSubset *parameterIndices,
    SmallVectorImpl<AutoDiffSemanticFunctionResultType> &resultTypes);

/// Returns the indices of semantic results for a given function.
IndexSubset *getFunctionSemanticResultIndices(
    const AnyFunctionType *functionType,
    const IndexSubset *parameterIndices);

IndexSubset *getFunctionSemanticResultIndices(
    const AbstractFunctionDecl *AFD,
    const IndexSubset *parameterIndices);

/// Returns the lowered SIL parameter indices for the given AST parameter
/// indices and `AnyFunctionType`.
///
/// Notable lowering-related changes:
/// - AST tuple parameter types are exploded when lowered to SIL.
/// - AST curried `Self` parameter types become the last parameter when lowered
///   to SIL.
///
/// Examples:
///
///   AST function type: (A, B, C) -> R
///   AST parameter indices: 101, {A, C}
///   Lowered SIL function type: $(A, B, C) -> R
///   Lowered SIL parameter indices: 101
///
///   AST function type: (Self) -> (A, B, C) -> R
///   AST parameter indices: 1010, {Self, B}
///   Lowered SIL function type: $(A, B, C, Self) -> R
///   Lowered SIL parameter indices: 0101
///
///   AST function type: (A, (B, C), D) -> R
///   AST parameter indices: 110, {A, (B, C)}
///   Lowered SIL function type: $(A, B, C, D) -> R
///   Lowered SIL parameter indices: 1110
///
/// Note:
/// - The AST function type must not be curried unless it is a method.
///   Otherwise, the behavior is undefined.
IndexSubset *getLoweredParameterIndices(IndexSubset *astParameterIndices,
                                        AnyFunctionType *functionType);

/// "Constrained" derivative generic signatures require all differentiability
/// parameters to conform to the `Differentiable` protocol.
///
/// "Constrained" transpose generic signatures additionally require all
/// linearity parameters to satisfy `Self == Self.TangentVector`.
///
/// Returns the "constrained" derivative/transpose generic signature given:
/// - An original SIL function type.
/// - Differentiability/linearity parameter indices.
/// - A possibly "unconstrained" derivative/transpose generic signature.
GenericSignature getConstrainedDerivativeGenericSignature(
    SILFunctionType *originalFnTy, IndexSubset *diffParamIndices,
    GenericSignature derivativeGenSig, LookupConformanceFn lookupConformance,
    bool isTranspose = false);

/// Retrieve config from the function name of a variant of
/// `Builtin.applyDerivative`, e.g. `Builtin.applyDerivative_jvp_arity2`.
/// Returns true if the function name is parsed successfully.
bool getBuiltinApplyDerivativeConfig(
    StringRef operationName, AutoDiffDerivativeFunctionKind &kind,
    unsigned &arity, bool &rethrows);

/// Retrieve config from the function name of a variant of
/// `Builtin.applyTranspose`, e.g. `Builtin.applyTranspose_arity2`.
/// Returns true if the function name is parsed successfully.
bool getBuiltinApplyTransposeConfig(
  StringRef operationName, unsigned &arity, bool &rethrows);

/// Retrieve config from the function name of a variant of
/// `Builtin.differentiableFunction` or `Builtin.linearFunction`, e.g.
/// `Builtin.differentiableFunction_arity1_throws`.
/// Returns true if the function name is parsed successfully.
bool getBuiltinDifferentiableOrLinearFunctionConfig(
    StringRef operationName, unsigned &arity, bool &throws);

/// Retrieve config from the function name of a variant of
/// `Builtin.differentiableFunction` or `Builtin.linearFunction`, e.g.
/// `Builtin.differentiableFunction_arity1_throws`.
/// Returns true if the function name is parsed successfully.
bool getBuiltinDifferentiableOrLinearFunctionConfig(
    StringRef operationName, unsigned &arity, bool &throws);

/// Returns the SIL differentiability witness generic signature given the
/// original declaration's generic signature and the derivative generic
/// signature.
///
/// In general, the differentiability witness generic signature is equal to the
/// derivative generic signature.
///
/// Edge case, if two conditions are satisfied:
/// 1. The derivative generic signature is equal to the original generic
///    signature.
/// 2. The derivative generic signature has *all concrete* generic parameters
///    (i.e. all generic parameters are bound to concrete types via same-type
///    requirements).
///
/// Then the differentiability witness generic signature is `nullptr`.
///
/// Both the original and derivative declarations are lowered to SIL functions
/// with a fully concrete type and no generic signature, so the
/// differentiability witness should similarly have no generic signature.
GenericSignature
getDifferentiabilityWitnessGenericSignature(GenericSignature origGenSig,
                                            GenericSignature derivativeGenSig);

} // end namespace autodiff

} // end namespace swift

namespace swift {
namespace Demangle {

AutoDiffFunctionKind
getAutoDiffFunctionKind(AutoDiffDerivativeFunctionKind kind);

AutoDiffFunctionKind getAutoDiffFunctionKind(AutoDiffLinearMapKind kind);

MangledDifferentiabilityKind
getMangledDifferentiabilityKind(DifferentiabilityKind kind);

} // end namespace autodiff
} // end namespace swift

namespace llvm {

using swift::AutoDiffConfig;
using swift::AutoDiffDerivativeFunctionKind;
using swift::CanGenericSignature;
using swift::GenericSignature;
using swift::IndexSubset;
using swift::SILAutoDiffDerivativeFunctionKey;
using swift::SILFunctionType;
using swift::DifferentiabilityKind;
using swift::SILDifferentiabilityWitnessKey;

template <typename T, typename Enable> struct DenseMapInfo;

template <> struct DenseMapInfo<AutoDiffConfig> {
  static AutoDiffConfig getEmptyKey() {
    auto *ptr = llvm::DenseMapInfo<void *>::getEmptyKey();
    // The `derivativeGenericSignature` component must be `nullptr` so that
    // `getHashValue` and `isEqual` do not try to call
    // `GenericSignatureImpl::getCanonicalSignature()` on an invalid pointer.
    return {static_cast<IndexSubset *>(ptr), static_cast<IndexSubset *>(ptr),
            nullptr};
  }

  static AutoDiffConfig getTombstoneKey() {
    auto *ptr = llvm::DenseMapInfo<void *>::getTombstoneKey();
    // The `derivativeGenericSignature` component must be `nullptr` so that
    // `getHashValue` and `isEqual` do not try to call
    // `GenericSignatureImpl::getCanonicalSignature()` on an invalid pointer.
    return {static_cast<IndexSubset *>(ptr), static_cast<IndexSubset *>(ptr),
            nullptr};
  }

  static unsigned getHashValue(const AutoDiffConfig &Val) {
    auto canGenSig = Val.derivativeGenericSignature.getCanonicalSignature();
    unsigned combinedHash = hash_combine(
        ~1U, DenseMapInfo<void *>::getHashValue(Val.parameterIndices),
        DenseMapInfo<void *>::getHashValue(Val.resultIndices),
        DenseMapInfo<GenericSignature>::getHashValue(canGenSig));
    return combinedHash;
  }

  static bool isEqual(const AutoDiffConfig &LHS, const AutoDiffConfig &RHS) {
    auto lhsCanGenSig = LHS.derivativeGenericSignature.getCanonicalSignature();
    auto rhsCanGenSig = RHS.derivativeGenericSignature.getCanonicalSignature();
    return LHS.parameterIndices == RHS.parameterIndices &&
           LHS.resultIndices == RHS.resultIndices &&
           DenseMapInfo<GenericSignature>::isEqual(lhsCanGenSig, rhsCanGenSig);
  }
};

template <> struct DenseMapInfo<AutoDiffDerivativeFunctionKind> {
  static AutoDiffDerivativeFunctionKind getEmptyKey() {
    return static_cast<AutoDiffDerivativeFunctionKind::innerty>(
        DenseMapInfo<unsigned>::getEmptyKey());
  }

  static AutoDiffDerivativeFunctionKind getTombstoneKey() {
    return static_cast<AutoDiffDerivativeFunctionKind::innerty>(
        DenseMapInfo<unsigned>::getTombstoneKey());
  }

  static unsigned getHashValue(const AutoDiffDerivativeFunctionKind &Val) {
    return DenseMapInfo<unsigned>::getHashValue(Val);
  }

  static bool isEqual(const AutoDiffDerivativeFunctionKind &LHS,
                      const AutoDiffDerivativeFunctionKind &RHS) {
    return static_cast<AutoDiffDerivativeFunctionKind::innerty>(LHS) ==
        static_cast<AutoDiffDerivativeFunctionKind::innerty>(RHS);
  }
};

template <> struct DenseMapInfo<SILAutoDiffDerivativeFunctionKey> {
  static bool isEqual(const SILAutoDiffDerivativeFunctionKey &lhs,
                      const SILAutoDiffDerivativeFunctionKey &rhs) {
    return lhs.originalType == rhs.originalType &&
           lhs.parameterIndices == rhs.parameterIndices &&
           lhs.resultIndices == rhs.resultIndices &&
           lhs.kind.rawValue == rhs.kind.rawValue &&
           lhs.derivativeFnGenSig == rhs.derivativeFnGenSig &&
           lhs.isReabstractionThunk == rhs.isReabstractionThunk;
  }

  static inline SILAutoDiffDerivativeFunctionKey getEmptyKey() {
    return {DenseMapInfo<SILFunctionType *>::getEmptyKey(),
            DenseMapInfo<IndexSubset *>::getEmptyKey(),
            DenseMapInfo<IndexSubset *>::getEmptyKey(),
            AutoDiffDerivativeFunctionKind::innerty(
                DenseMapInfo<unsigned>::getEmptyKey()),
            CanGenericSignature(DenseMapInfo<GenericSignature>::getEmptyKey()),
            (bool)DenseMapInfo<unsigned>::getEmptyKey()};
  }

  static inline SILAutoDiffDerivativeFunctionKey getTombstoneKey() {
    return {
        DenseMapInfo<SILFunctionType *>::getTombstoneKey(),
        DenseMapInfo<IndexSubset *>::getTombstoneKey(),
        DenseMapInfo<IndexSubset *>::getTombstoneKey(),
        AutoDiffDerivativeFunctionKind::innerty(
            DenseMapInfo<unsigned>::getTombstoneKey()),
        CanGenericSignature(DenseMapInfo<GenericSignature>::getTombstoneKey()),
        (bool)DenseMapInfo<unsigned>::getTombstoneKey()};
  }

  static unsigned getHashValue(const SILAutoDiffDerivativeFunctionKey &Val) {
    return hash_combine(
        DenseMapInfo<SILFunctionType *>::getHashValue(Val.originalType),
        DenseMapInfo<IndexSubset *>::getHashValue(Val.parameterIndices),
        DenseMapInfo<IndexSubset *>::getHashValue(Val.resultIndices),
        DenseMapInfo<unsigned>::getHashValue((unsigned)Val.kind.rawValue),
        DenseMapInfo<GenericSignature>::getHashValue(Val.derivativeFnGenSig),
        DenseMapInfo<unsigned>::getHashValue(
            (unsigned)Val.isReabstractionThunk));
  }
};

template <> struct DenseMapInfo<SILDifferentiabilityWitnessKey> {
  static bool isEqual(const SILDifferentiabilityWitnessKey &lhs,
                      const SILDifferentiabilityWitnessKey &rhs) {
    return DenseMapInfo<StringRef>::isEqual(
               lhs.originalFunctionName, rhs.originalFunctionName) &&
           DenseMapInfo<unsigned>::isEqual(
               (unsigned)lhs.kind, (unsigned)rhs.kind) &&
           DenseMapInfo<AutoDiffConfig>::isEqual(lhs.config, rhs.config);
  }

  static inline SILDifferentiabilityWitnessKey getEmptyKey() {
    return {DenseMapInfo<StringRef>::getEmptyKey(),
            (DifferentiabilityKind)DenseMapInfo<unsigned>::getEmptyKey(),
            DenseMapInfo<AutoDiffConfig>::getEmptyKey()};
  }

  static inline SILDifferentiabilityWitnessKey getTombstoneKey() {
    return {DenseMapInfo<StringRef>::getTombstoneKey(),
            (DifferentiabilityKind)DenseMapInfo<unsigned>::getTombstoneKey(),
            DenseMapInfo<AutoDiffConfig>::getTombstoneKey()};
  }

  static unsigned getHashValue(const SILDifferentiabilityWitnessKey &val) {
    return hash_combine(
        DenseMapInfo<StringRef>::getHashValue(val.originalFunctionName),
        DenseMapInfo<unsigned>::getHashValue((unsigned)val.kind),
        DenseMapInfo<AutoDiffConfig>::getHashValue(val.config));
  }
};

} // end namespace llvm

#endif // SWIFT_AST_AUTODIFF_H

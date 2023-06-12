//===--- AbstractionPattern.h - SIL type abstraction patterns ---*- C++ -*-===//
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
// This file defines the AbstractionPattern class, which is used to
// lower formal AST types into their SIL lowerings.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_ABSTRACTIONPATTERN_H
#define SWIFT_SIL_ABSTRACTIONPATTERN_H

#include "swift/Basic/IndexedViewRange.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"

namespace llvm {
  template <class T> class function_ref;
}

namespace clang {
  class CXXMethodDecl;
  class ObjCMethodDecl;
  class Type;
  class ValueDecl;
}

namespace swift {
namespace Lowering {
class FunctionParamGenerator;
class TupleElementGenerator;

/// A pattern for the abstraction of a value.
///
/// The representation of values in Swift can vary according to how
/// their type is abstracted: which is to say, according to the pattern
/// of opaque type variables within their type.  The main motivation
/// here is performance: it would be far easier for types to adopt a
/// single representation regardless of their abstraction, but this
/// would force Swift to adopt a very inefficient representation for
/// abstractable values.
///
/// For example, consider the comparison function on Int:
///   func <(lhs : Int, rhs : Int) -> Bool
///
/// This function can be used as an opaque value of type
/// (Int,Int)->Bool.  An optimal representation of values of that type
/// (ignoring context parameters for the moment) would be a pointer to
/// a function that takes these two arguments directly in registers and
/// returns the result directly in a register.
///
/// (It's important to remember throughout this discussion that we're
/// talking about abstract values.  There's absolutely nothing that
/// requires direct uses of the function to follow the same conventions
/// as abstract uses!  A direct use of a declaration --- even one that
/// implies an indirect call, like a class's instance method ---
/// provides a concrete specification for exactly how to interact with
/// value.)
///
/// However, that representation is problematic in the presence of
/// generics.  This function could be passed off to any of the following
/// generic functions:
///   func foo<T>(f : (T, Int) -> Bool)
///   func bar<U,V>(f : (U, V) -> Bool)
///   func baz<W>(f : (Int, Int) -> W)
///
/// These generic functions all need to be able to call 'f'.  But in
/// Swift's implementation model, these functions don't have to be
/// instantiated for different parameter types, which means that (e.g.)
/// the same 'baz' implementation needs to also be able to work when
/// W=String.  But the optimal way to pass an Int to a function might
/// well be different from the optimal way to pass a String.
///
/// And this runs in both directions: a generic function might return
/// a function that the caller would like to use as an (Int,Int)->Bool:
///   func getFalseFunction<T>() -> (T,T)->Bool
///
/// There are three ways we can deal with this:
///
/// 1. Give all types in Swift a common representation.  The generic
/// implementation can work with both W=String and W=Int because
/// both of those types have the same (direct) storage representation.
/// That's pretty clearly not an acceptable sacrifice.
///
/// 2. Adopt a most-general representation of function types that is
/// used for opaque values; for example, all parameters and results
/// could be passed indirectly.  Concrete values must be coerced to
/// this representation when made abstract.  Unfortunately, there
/// are a lot of obvious situations where this is sub-optimal:
/// for example, in totally non-generic code that just passes around
/// a value of type (Int,Int)->Bool.
///
/// 3. Permit the representation of values to vary by abstraction.
/// Values require coercion when changing abstraction patterns.
/// For example, the argument to 'bar' would be expected to return
/// its Bool result directly but take the T and U parameters indirectly.
/// When '<' is passed to this, what must actually be passed is a
/// thunk that loads both indirect parameters before calling '<'.
///
/// There is one major risk with (3): naively implemented, a single
/// function value which undergoes many coercions could build up a
/// linear number of re-abstraction thunks.  However, this can be
/// solved dynamically by applying thunks with a runtime function that
/// can recognize and bypass its own previous handiwork.
///
/// In general, abstraction patterns are derived from some explicit
/// type expression, such as the written type of a variable or
/// parameter.  This works whenever the expression directly provides
/// structure for the type in question; for example, when the original
/// type is (T,Int)->Bool and we are working with an (Int,Int)->Bool
/// substitution.  However, it is inadequate when the expression does
/// not provide structure at the appropriate level, i.e. when that
/// level is substituted in: when the original type is merely T.  In
/// these cases, we must devolve to a representation which all legal
/// substitutors will agree upon.
///
/// The most general type of a function type replaces all parameters and the
/// result with fresh, unrestricted generic parameters.
///
/// That is, if we have a substituted function type:
///
///   (UnicodeScalar, (Int, Float), Double) -> (Bool, String)
///
/// then its most general form is
///
///   (A, B, C) -> D
///
/// because there is a valid substitution
///   A := UnicodeScalar
///   B := (Int, Float)
///   C := Double
///   D := (Bool, String)
///
class AbstractionPattern {
  enum class Kind {
    /// A type reference.  OrigType is valid.
    Type,
    /// An invalid pattern.
    Invalid,
    /// A completely opaque abstraction pattern.
    Opaque,
    /// An open-coded tuple pattern.  OrigTupleElements is valid.
    /// OtherData is the number of tuple elements.
    Tuple,
    /// A discarded value. OrigType is valid.
    Discard,
    /// A type reference with a Clang type.  OrigType and ClangType are valid.
    ClangType,
    /// The curried imported type of an Objective-C method (that is,
    /// 'Self -> Input -> Result').  OrigType is valid and is a function
    /// type.  ObjCMethod is valid.  OtherData is an encoded foreign
    /// error index.
    CurriedObjCMethodType,
    /// The partially-applied curried imported type of an Objective-C
    /// method (that is, 'Input -> Result').  OrigType is valid and is a
    /// function type.  ObjCMethod is valid.  OtherData is an encoded
    /// foreign error index.
    PartialCurriedObjCMethodType,
    /// The uncurried imported type of a C function imported as a method.
    /// OrigType is valid and is a function type. ClangType is valid and is
    /// a function type. OtherData is an encoded ImportAsMemberStatus.
    CFunctionAsMethodType,
    /// The curried imported type of a C function imported as a method.
    /// OrigType is valid and is a function type. ClangType is valid and is
    /// a function type. OtherData is an encoded ImportAsMemberStatus.
    CurriedCFunctionAsMethodType,
    /// The partially-applied curried imported type of a C function imported as
    /// a method.
    /// OrigType is valid and is a function type. ClangType is valid and is
    /// a function type. OtherData is an encoded ImportAsMemberStatus.
    PartialCurriedCFunctionAsMethodType,
    /// The uncurried imported type of an Objective-C method (that is,
    /// '(Input, Self) -> Result').  OrigType is valid and is a function
    /// type.  ObjCMethod is valid.  OtherData is an encoded foreign
    /// error index.
    ObjCMethodType,
    /// The type of an ObjC block used as a completion handler for
    /// an API that has been imported into Swift as async,
    /// representing the tuple of results of the async projection of the
    /// API.
    ObjCCompletionHandlerArgumentsType,
    /// The uncurried imported type of a C++ non-operator non-static member
    /// function. OrigType is valid and is a function type. CXXMethod is valid.
    CXXMethodType,
    /// The curried imported type of a C++ non-operator non-static member
    /// function. OrigType is valid and is a function type. CXXMethod is valid.
    CurriedCXXMethodType,
    /// The partially-applied curried imported type of a C++ non-operator
    /// non-static member function. OrigType is valid and is a function type.
    /// CXXMethod is valid.
    PartialCurriedCXXMethodType,
    /// A Swift function whose parameters and results are opaque. This is
    /// like `AP::Type<T>((T) -> T)`, except that the number of parameters is
    /// unspecified.
    ///
    /// This is used to construct the abstraction pattern for the
    /// derivative function of a function with opaque abstraction pattern. See
    /// `OpaqueDerivativeFunction`.
    OpaqueFunction,
    /// A Swift function whose parameters are opaque and whose result is the
    /// tuple abstraction pattern `(AP::Opaque, AP::OpaqueFunction)`.
    ///
    /// Purpose: when we reabstract `@differentiable` function-typed values
    /// using the`AP::Opaque` pattern, we use `AP::Opaque` to reabstract the
    /// original function in the bundle and `AP::OpaqueDerivativeFunction` to
    /// reabstract the derivative functions in the bundle. This preserves the
    /// `@differentiable` function invariant that the derivative type
    /// (`SILFunctionType::getAutoDiffDerivativeFunctionType()`) of the original
    /// function is equal to the type of the derivative function. For example:
    ///
    ///   differentiable_function
    ///     [parameters 0]
    ///     %0 : $@callee_guaranteed (Float) -> Float
    ///     with_derivative {
    ///       %1 : $@callee_guaranteed (Float) -> (
    ///         Float,
    ///         @owned @callee_guaranteed (Float) -> Float
    ///       ),
    ///       %2 : $@callee_guaranteed (Float) -> (
    ///         Float,
    ///         @owned @callee_guaranteed (Float) -> Float
    ///       )
    ///     }
    ///
    /// The invariant-respecting abstraction of this value to `AP::Opaque` is:
    ///
    ///   differentiable_function
    ///     [parameters 0]
    ///     %3 : $@callee_guaranteed (@in_guaranteed Float) -> @out Float
    ///     with_derivative {
    ///       %4 : $@callee_guaranteed (@in_guaranteed Float) -> (
    ///         @out Float,
    ///         @owned @callee_guaranteed (@in_guaranteed Float) -> @out Float
    ///       ),
    ///       %5 : $@callee_guaranteed (@in_guaranteed Float) -> (
    ///         @out Float,
    ///         @owned @callee_guaranteed (@in_guaranteed Float) -> @out Float
    ///       )
    ///     }
    ///
    /// In particular:
    ///
    /// - The reabstraction %0 => %3 uses pattern `AP::Opaque`.
    /// - The reabstraction %1 => %4 uses pattern
    ///   `AP::OpaqueDerivativeFunction`, which maximally abstracts all the
    ///   parameters, and abstracts the result as the tuple
    ///   `(AP::Opaque, AP::OpaqueFunction)`.
    /// - The reabstraction %2 => %5 similarly uses pattern
    ///   `AP::OpaqueDerivativeFunction`.
    OpaqueDerivativeFunction,
  };

  class EncodedForeignInfo {
    unsigned Value;
    
    enum Error_t {
      Error,
    };
    
    enum Async_t {
      Async,
    };
    
    enum {
      AsyncCompletionParameterIndexMask = 0xFFEu,
      AsyncCompletionParameterIndexShift = 1,
      
      AsyncCompletionErrorParameterIndexMask = 0x1FF000u,
      AsyncCompletionErrorParameterIndexShift = 12,
      
      AsyncCompletionErrorFlagParameterIndexMask = 0x3FE00000u,
      AsyncCompletionErrorFlagParameterIndexShift = 21,
      
      AsyncCompletionErrorFlagParameterPolarityMask = 0x40000000u,
      AsyncCompletionErrorFlagParameterPolarityShift = 30,
    };
    
  public:
    enum ForeignKind {
      IsNotForeign,
      IsError,
      IsAsync,
    };

  private:
    friend AbstractionPattern;

    EncodedForeignInfo() : Value(0) {}
    EncodedForeignInfo(Error_t,
                       unsigned errorParameterIndex,
                       bool replaceParamWithVoid,
                       bool stripsResultOptionality)
      : Value(1
              + (unsigned(IsError) - 1)
              + (unsigned(stripsResultOptionality) << 1)
              + (unsigned(replaceParamWithVoid) << 2)
              + (errorParameterIndex << 3)) {
        assert(getKind() == IsError);
        assert(getErrorParamIndex() == errorParameterIndex);
        assert(hasErrorParameterReplacedWithVoid() == replaceParamWithVoid);
        assert(errorStripsResultOptionality() == stripsResultOptionality);
      }

    EncodedForeignInfo(Async_t,
                       unsigned completionParameterIndex,
                       Optional<unsigned> completionErrorParameterIndex,
                       Optional<unsigned> completionErrorFlagParameterIndex,
                       bool completionErrorFlagIsZeroOnError)
    : Value(1
      + (unsigned(IsAsync) - 1)
      + (unsigned(completionParameterIndex) << AsyncCompletionParameterIndexShift)
      + ((completionErrorParameterIndex ? *completionErrorParameterIndex + 1
                                        : 0) << AsyncCompletionErrorParameterIndexShift)
      + ((completionErrorFlagParameterIndex ? *completionErrorFlagParameterIndex + 1
                                            : 0) << AsyncCompletionErrorFlagParameterIndexShift)
      + (unsigned(completionErrorFlagIsZeroOnError) << AsyncCompletionErrorFlagParameterPolarityShift)){

      assert(getKind() == IsAsync);
      assert(getAsyncCompletionHandlerParamIndex() == completionParameterIndex);
      assert(getAsyncCompletionHandlerErrorParamIndex() == completionErrorParameterIndex);
      assert(getAsyncCompletionHandlerErrorFlagParamIndex() == completionErrorFlagParameterIndex);
      assert(isCompletionErrorFlagZeroOnError() == completionErrorFlagIsZeroOnError);
    }

  public:
    static EncodedForeignInfo
    encode(const Optional<ForeignErrorConvention> &foreignError,
           const Optional<ForeignAsyncConvention> &foreignAsync);

    bool hasValue() const { return Value != 0; }
    ForeignKind getKind() const {
      if (!hasValue())
        return IsNotForeign;
      
      return ForeignKind((Value - 1 & 1) + 1);
    }
    
    bool errorStripsResultOptionality() const {
      if (getKind() != IsError) return false;
      return (Value - 1) & 2;
    }

    bool hasErrorParameterReplacedWithVoid() const {
      if (getKind() != IsError) return false;
      return (Value - 1) & 4;
    }

    unsigned getErrorParamIndex() const {
      assert(getKind() == IsError);
      return (Value - 1) >> 3;
    }
    
    unsigned getAsyncCompletionHandlerParamIndex() const {
      assert(getKind() == IsAsync);
      return ((Value - 1) & AsyncCompletionParameterIndexMask)
        >> AsyncCompletionParameterIndexShift;
    }
    
    Optional<unsigned> getAsyncCompletionHandlerErrorParamIndex() const {
      assert(getKind() == IsAsync);

      unsigned encodedValue = ((Value - 1) & AsyncCompletionErrorParameterIndexMask)
        >> AsyncCompletionErrorParameterIndexShift;
      if (encodedValue == 0) {
        return llvm::None;
      }
      return encodedValue - 1;
    }

    Optional<unsigned> getAsyncCompletionHandlerErrorFlagParamIndex() const {
      assert(getKind() == IsAsync);

      unsigned encodedValue = ((Value - 1) & AsyncCompletionErrorFlagParameterIndexMask)
        >> AsyncCompletionErrorFlagParameterIndexShift;
      if (encodedValue == 0) {
        return llvm::None;
      }
      return encodedValue - 1;
    }
    
    bool isCompletionErrorFlagZeroOnError() const {
      assert(getKind() == IsAsync);

      return (Value - 1) & AsyncCompletionErrorFlagParameterPolarityMask;
    }

    unsigned getForeignParamIndex() const {
      switch (getKind()) {
      case IsNotForeign:
        llvm_unreachable("no foreign param");
      
      case IsError:
        return getErrorParamIndex();
          
      case IsAsync:
        return getAsyncCompletionHandlerParamIndex();
      }
      llvm_unreachable("uncovered switch");
    }

    unsigned getOpaqueValue() const { return Value; }
    static EncodedForeignInfo fromOpaqueValue(unsigned value) {
      EncodedForeignInfo result;
      result.Value = value;
      return result;
    }
  };

  static constexpr const unsigned NumOtherDataBits = 28;
  static constexpr const unsigned MaxOtherData = (1 << NumOtherDataBits) - 1;

  unsigned TheKind : 33 - NumOtherDataBits;
  unsigned OtherData : NumOtherDataBits;
  CanType OrigType;
  union {
    const clang::Type *ClangType;
    const clang::ObjCMethodDecl *ObjCMethod;
    const clang::CXXMethodDecl *CXXMethod;
    const AbstractionPattern *OrigTupleElements;
    const void *RawTypePtr;
  };
  CanGenericSignature GenericSig;
  SubstitutionMap GenericSubs;

  Kind getKind() const { return Kind(TheKind); }

  CanGenericSignature getGenericSignatureForFunctionComponent() const {
    if (auto genericFn = dyn_cast<GenericFunctionType>(getType())) {
      return genericFn.getGenericSignature();
    } else {
      return getGenericSignature();
    }
  }

  unsigned getNumTupleElements_Stored() const {
    assert(getKind() == Kind::Tuple);
    return OtherData;
  }

  bool hasStoredClangType() const {
    switch (getKind()) {
    case Kind::ClangType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return true;

    default:
      return false;
    }
  }

  bool hasStoredCXXMethod() const {
    switch (getKind()) {
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
      return true;

    default:
      return false;
    }
  }

  bool hasStoredObjCMethod() const {
    switch (getKind()) {
    case Kind::CurriedObjCMethodType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::ObjCMethodType:
      return true;

    default:
      return false;
    }
  }

  bool hasStoredForeignInfo() const {
    switch (getKind()) {
    case Kind::CurriedObjCMethodType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return true;

    default:
      return false;
    }
  }

  bool hasImportAsMemberStatus() const {
    switch (getKind()) {
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
      return true;

    default:
      return false;
    }
  }
  
  void initSwiftType(SubstitutionMap subs,
                     CanGenericSignature signature,
                     CanType origType,
                     Kind kind = Kind::Type) {
    assert(signature || !origType->hasTypeParameter());
    TheKind = unsigned(kind);
    OrigType = origType;
    GenericSig = CanGenericSignature();
    GenericSubs = subs;
    if (OrigType->hasTypeParameter()) {
      assert(OrigType == signature.getReducedType(origType));
      GenericSig = signature;
    }
  }

  void initClangType(SubstitutionMap subs, CanGenericSignature signature,
                     CanType origType, const clang::Type *clangType,
                     Kind kind = Kind::ClangType) {
    initSwiftType(subs, signature, origType, kind);
    ClangType = clangType;
  }

  void initObjCMethod(SubstitutionMap subs, CanGenericSignature signature,
                      CanType origType, const clang::ObjCMethodDecl *method,
                      Kind kind, EncodedForeignInfo errorInfo) {
    initSwiftType(subs, signature, origType, kind);
    ObjCMethod = method;
    OtherData = errorInfo.getOpaqueValue();
  }

  void initCFunctionAsMethod(SubstitutionMap subs,
                             CanGenericSignature signature,
                             CanType origType, const clang::Type *clangType,
                             Kind kind,
                             ImportAsMemberStatus memberStatus) {
    initClangType(subs, signature, origType, clangType, kind);
    OtherData = memberStatus.getRawValue();
  }

  void initCXXMethod(SubstitutionMap subs,
                     CanGenericSignature signature, CanType origType,
                     const clang::CXXMethodDecl *method, Kind kind,
                     ImportAsMemberStatus memberStatus) {
    initSwiftType(subs, signature, origType, kind);
    CXXMethod = method;
    OtherData = memberStatus.getRawValue();
  }

  AbstractionPattern() {}
  explicit AbstractionPattern(Kind kind) : TheKind(unsigned(kind)) {}

public:
  explicit AbstractionPattern(Type origType)
    : AbstractionPattern(origType->getCanonicalType()) {}
  explicit AbstractionPattern(CanType origType)
    : AbstractionPattern(nullptr, origType) {}
  explicit AbstractionPattern(CanGenericSignature signature, CanType origType) {
    initSwiftType(SubstitutionMap(), signature, origType);
  }
  explicit AbstractionPattern(SubstitutionMap subs, CanType origType) {
    initSwiftType(subs, subs.getGenericSignature().getCanonicalSignature(),
                  origType);
  }
  explicit AbstractionPattern(SubstitutionMap subs, CanGenericSignature sig,
                              CanType origType) {
    initSwiftType(subs, sig, origType);
  }
  explicit AbstractionPattern(CanType origType, const clang::Type *clangType)
    : AbstractionPattern(nullptr, origType, clangType) {}
  explicit AbstractionPattern(CanGenericSignature signature, CanType origType,
                              const clang::Type *clangType) {
    initClangType(SubstitutionMap(), signature, origType, clangType);
  }
  explicit AbstractionPattern(SubstitutionMap subs,
                              CanGenericSignature signature,
                              CanType origType,
                              const clang::Type *clangType) {
    initClangType(subs, signature, origType, clangType);
  }

  static AbstractionPattern getOpaque() {
    return AbstractionPattern(Kind::Opaque);
  }

  static AbstractionPattern getInvalid() {
    return AbstractionPattern(Kind::Invalid);
  }

  static AbstractionPattern getOpaqueFunction() {
    return AbstractionPattern(Kind::OpaqueFunction);
  }

  static AbstractionPattern getOpaqueDerivativeFunction() {
    return AbstractionPattern(Kind::OpaqueDerivativeFunction);
  }

  bool hasGenericSignature() const {
    switch (getKind()) {
    case Kind::Type:
    case Kind::Discard:
    case Kind::ClangType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return true;
    case Kind::Invalid:
    case Kind::Opaque:
    case Kind::Tuple:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      return false;
    }
    llvm_unreachable("Unhandled AbstractionPatternKind in switch");
  }

  SubstitutionMap getGenericSubstitutions() const {
    return GenericSubs;
  }

  CanGenericSignature getGenericSignature() const {
    assert(hasGenericSignature());
    return CanGenericSignature(GenericSig);
  }

  CanGenericSignature getGenericSignatureOrNull() const {
    if (!hasGenericSignature())
      return CanGenericSignature();
    return CanGenericSignature(GenericSig);
  }

  /// Return an open-coded abstraction pattern for a tuple.  The
  /// caller is responsible for ensuring that the storage for the
  /// tuple elements is valid for as long as the abstraction pattern is.
  static AbstractionPattern getTuple(ArrayRef<AbstractionPattern> tuple) {
    AbstractionPattern pattern(Kind::Tuple);
    pattern.OtherData = tuple.size();
    pattern.OrigTupleElements = tuple.data();
    return pattern;
  }

  /// Return an abstraction pattern for a result tuple
  /// corresponding to the parameters of a completion handler
  /// block of an API that was imported as async.
  static AbstractionPattern
  getObjCCompletionHandlerArgumentsType(SubstitutionMap subs,
                                        CanGenericSignature sig,
                                        CanType origTupleType,
                                        const clang::Type *clangBlockType,
                                        EncodedForeignInfo foreignInfo) {
    AbstractionPattern pattern(Kind::ObjCCompletionHandlerArgumentsType);
    pattern.initClangType(subs, sig, origTupleType, clangBlockType,
                          Kind::ObjCCompletionHandlerArgumentsType);
    pattern.OtherData = foreignInfo.getOpaqueValue();
    
    return pattern;
  }

public:
  /// Return an abstraction pattern for the curried type of an
  /// Objective-C method.
  static AbstractionPattern
  getCurriedObjCMethod(CanType origType, const clang::ObjCMethodDecl *method,
                       const Optional<ForeignErrorConvention> &foreignError,
                       const Optional<ForeignAsyncConvention> &foreignAsync);

  
  /// Return an abstraction pattern for the uncurried type of a C function
  /// imported as a method.
  ///
  /// For example, if the original function is:
  ///   void CCRefrigeratorSetTemperature(CCRefrigeratorRef fridge,
  ///                                   CCRefrigeratorCompartment compartment,
  ///                                   CCTemperature temperature);
  /// then the uncurried type is:
  ///   ((CCRefrigeratorComponent, CCTemperature), CCRefrigerator) -> ()
  static AbstractionPattern
  getCFunctionAsMethod(CanType origType, const clang::Type *clangType,
                       ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCFunctionAsMethod(SubstitutionMap(), nullptr,
                                  origType, clangType,
                                  Kind::CFunctionAsMethodType,
                                  memberStatus);
    return pattern;
  }

  /// Return an abstraction pattern for the curried type of a
  /// C function imported as a method.
  ///
  /// For example, if the original function is:
  ///   void CCRefrigeratorSetTemperature(CCRefrigeratorRef fridge,
  ///                                   CCRefrigeratorCompartment compartment,
  ///                                   CCTemperature temperature);
  /// then the curried type is:
  ///   (CCRefrigerator) -> (CCRefrigeratorCompartment, CCTemperature) -> ()
  static AbstractionPattern
  getCurriedCFunctionAsMethod(CanType origType,
                              const AbstractFunctionDecl *function);

  /// Return an abstraction pattern for the curried type of a C++ method.
  static AbstractionPattern
  getCurriedCXXMethod(CanType origType, const AbstractFunctionDecl *function);

  /// Return an abstraction pattern for the uncurried type of a C++ method.
  ///
  /// For example, if the original function is:
  ///   void Refrigerator::SetTemperature(RefrigeratorCompartment compartment,
  ///                                     Temperature temperature);
  /// then the uncurried type is:
  ///   ((RefrigeratorCompartment, Temperature), Refrigerator) -> ()
  static AbstractionPattern getCXXMethod(CanType origType,
                                         const clang::CXXMethodDecl *method,
                                         ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCXXMethod(SubstitutionMap(), nullptr, origType, method,
                          Kind::CXXMethodType, memberStatus);
    return pattern;
  }

  /// Return an abstraction pattern for the curried type of a C++ method.
  ///
  /// For example, if the original function is:
  ///   void Refrigerator::SetTemperature(RefrigeratorCompartment compartment,
  ///                                     Temperature temperature);
  /// then the curried type:
  ///   (Refrigerator) -> (Compartment, Temperature) -> ()
  static AbstractionPattern
  getCurriedCXXMethod(CanType origType, const clang::CXXMethodDecl *method,
                      ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCXXMethod(SubstitutionMap(), nullptr, origType, method,
                          Kind::CurriedCXXMethodType, memberStatus);
    return pattern;
  }

  /// For a C-function-as-method pattern,
  /// get the index of the C function parameter that was imported as the
  /// `self` parameter of the imported method, or None if this is a static
  /// method with no `self` parameter.
  ImportAsMemberStatus getImportAsMemberStatus() const {
    assert(hasImportAsMemberStatus());
    return ImportAsMemberStatus(OtherData);
  }
  
  /// Return an abstraction pattern for a value that is discarded after being
  /// evaluated.
  static AbstractionPattern getDiscard(SubstitutionMap subs,
                                       CanGenericSignature signature,
                                       CanType origType) {
    AbstractionPattern pattern;
    pattern.initSwiftType(subs, signature, origType, Kind::Discard);
    return pattern;
  }
  
  /// Return an abstraction pattern for the type of the given struct field or enum case
  /// substituted in `this` type.
  ///
  /// Note that, for most purposes, you should lower a field's type against its
  /// *unsubstituted* interface type.
  AbstractionPattern
  unsafeGetSubstFieldType(ValueDecl *member, CanType origMemberType,
                          SubstitutionMap subMap) const;
  
private:
  /// Return an abstraction pattern for the curried type of an
  /// Objective-C method.
  static AbstractionPattern
  getCurriedObjCMethod(CanType origType, const clang::ObjCMethodDecl *method,
                       EncodedForeignInfo errorInfo) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initObjCMethod(SubstitutionMap(), nullptr, origType, method,
                           Kind::CurriedObjCMethodType, errorInfo);
    return pattern;
  }
  
  static AbstractionPattern
  getCurriedCFunctionAsMethod(CanType origType,
                              const clang::Type *clangType,
                              ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCFunctionAsMethod(SubstitutionMap(), nullptr,
                                  origType, clangType,
                                  Kind::CurriedCFunctionAsMethodType,
                                  memberStatus);
    return pattern;
  }

  /// Return an abstraction pattern for the partially-applied curried
  /// type of an Objective-C method.
  static AbstractionPattern
  getPartialCurriedObjCMethod(SubstitutionMap subs,
                              CanGenericSignature signature,
                              CanType origType,
                              const clang::ObjCMethodDecl *method,
                              EncodedForeignInfo errorInfo) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initObjCMethod(subs, signature, origType, method,
                           Kind::PartialCurriedObjCMethodType, errorInfo);
    return pattern;
  }

  /// Return an abstraction pattern for the partially-applied curried
  /// type of a C function imported as a method.
  ///
  /// For example, if the original function is:
  ///   CCRefrigeratorSetTemperature(CCRefrigeratorRef, CCTemperature)
  /// then the curried type is:
  ///   (CCRefrigerator) -> (CCTemperature) -> ()
  /// and the partially-applied curried type is:
  ///   (CCTemperature) -> ()
  static AbstractionPattern
  getPartialCurriedCFunctionAsMethod(SubstitutionMap subs,
                                     CanGenericSignature signature,
                                     CanType origType,
                                     const clang::Type *clangType,
                                     ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCFunctionAsMethod(subs, signature, origType, clangType,
                                  Kind::PartialCurriedCFunctionAsMethodType,
                                  memberStatus);
    return pattern;
  }

  /// Return an abstraction pattern for the partially-applied curried
  /// type of an C++ method.
  ///
  /// For example, if the original function is:
  ///   void Refrigerator::SetTemperature(RefrigeratorCompartment compartment,
  ///                                     Temperature temperature);
  /// then the partially-applied curried type is:
  ///   (Compartment, Temperature) -> ()
  static AbstractionPattern
  getPartialCurriedCXXMethod(SubstitutionMap subs,
                             CanGenericSignature signature, CanType origType,
                             const clang::CXXMethodDecl *method,
                             ImportAsMemberStatus memberStatus) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initCXXMethod(subs, signature, origType, method,
                          Kind::PartialCurriedCXXMethodType, memberStatus);
    return pattern;
  }

public:
  /// Return an abstraction pattern for the type of an Objective-C method.
  static AbstractionPattern
  getObjCMethod(CanType origType, const clang::ObjCMethodDecl *method,
                const Optional<ForeignErrorConvention> &foreignError,
                const Optional<ForeignAsyncConvention> &foreignAsync);

private:
  /// Return an abstraction pattern for the uncurried type of an
  /// Objective-C method.
  static AbstractionPattern
  getObjCMethod(CanType origType, const clang::ObjCMethodDecl *method,
                EncodedForeignInfo errorInfo) {
    assert(isa<AnyFunctionType>(origType));
    AbstractionPattern pattern;
    pattern.initObjCMethod(SubstitutionMap(), nullptr,
                           origType, method, Kind::ObjCMethodType,
                           errorInfo);
    return pattern;
  }

  /// Return a pattern corresponding to the 'self' parameter of the
  /// current Objective-C method.
  AbstractionPattern getObjCMethodSelfPattern(CanType paramType) const;

  /// Return a pattern corresponding to the 'self' parameter of the
  /// current C function imported as a method.
  AbstractionPattern getCFunctionAsMethodSelfPattern(CanType paramType) const;

  /// Return a pattern corresponding to the 'self' parameter of the
  /// current C++ method.
  AbstractionPattern getCXXMethodSelfPattern(CanType paramType) const;

public:
  /// Return an abstraction pattern with an added level of optionality.
  ///
  /// The based abstraction pattern must be either opaque or based on
  /// a Clang or Swift type.  That is, it cannot be a tuple or an ObjC
  /// method type.
  static AbstractionPattern getOptional(AbstractionPattern objectPattern);

  /// Does this abstraction pattern have something that can be used as a key?
  bool hasCachingKey() const {
    // Only the simplest Kind::Type pattern has a caching key; we
    // don't want to try to unique by Clang node.
    //
    // Even if we support Clang nodes someday, we *cannot* cache
    // by the open-coded patterns like Tuple.
    return getKind() == Kind::Type || getKind() == Kind::Opaque
        || getKind() == Kind::Discard;
  }
  using CachingKey = CanType;
  CachingKey getCachingKey() const {
    assert(hasCachingKey());
    return OrigType;
  }

  bool isValid() const {
    return getKind() != Kind::Invalid;
  }

  bool isTypeParameterOrOpaqueArchetype() const {
    switch (getKind()) {
    case Kind::Opaque:
      return true;
    case Kind::Type:
    case Kind::ClangType:
    case Kind::Discard: {
      auto type = getType();
      if (isa<DependentMemberType>(type) ||
          isa<GenericTypeParamType>(type) ||
          isa<PackElementType>(type) ||
          isa<ArchetypeType>(type)) {
        return true;
      }
      return false;
    }
    default:
      return false;
    }
  }

  bool isTypeParameter() const {
    switch (getKind()) {
    case Kind::Opaque:
      return true;
    case Kind::Type:
    case Kind::ClangType:
    case Kind::Discard: {
      auto type = getType();
      if (isa<DependentMemberType>(type) ||
          isa<GenericTypeParamType>(type) ||
          isa<PackElementType>(type)) {
        return true;
      }
      if (auto archetype = dyn_cast<ArchetypeType>(type)) {
        return !isa<OpaqueTypeArchetypeType>(archetype);
      }
      return false;
    }
    default:
      return false;
    }
  }

  bool isTypeParameterPack() const {
    switch (getKind()) {
    case Kind::Opaque:
      return false;
    case Kind::Type:
    case Kind::ClangType:
    case Kind::Discard: {
      auto ty = getType();
      return isa<PackArchetypeType>(ty) || ty->isParameterPack();
    }
    default:
      return false;
    }
  }

  bool isPackExpansion() const {
    switch (getKind()) {
    case Kind::Type:
    case Kind::Discard:
      return isa<PackExpansionType>(getType());
    default:
      return false;
    }
  }

  /// Is this an interface type that is subject to a concrete
  /// same-type constraint?
  bool isConcreteType() const;

  bool requiresClass() const;
  LayoutConstraint getLayoutConstraint() const;

  /// Return the Swift type which provides structure for this
  /// abstraction pattern.
  ///
  /// This is always valid unless the pattern is opaque or an
  /// open-coded tuple.  However, it does not always fully describe
  /// the abstraction pattern.
  CanType getType() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
      llvm_unreachable("opaque pattern has no type");
    case Kind::Tuple:
      llvm_unreachable("open-coded tuple pattern has no type");
    case Kind::OpaqueFunction:
      llvm_unreachable("opaque function pattern has no type");
    case Kind::OpaqueDerivativeFunction:
      llvm_unreachable("opaque derivative function pattern has no type");
    case Kind::ClangType:
    case Kind::ObjCCompletionHandlerArgumentsType:
    case Kind::CurriedObjCMethodType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::Type:
    case Kind::Discard:
      return OrigType;
    }
    llvm_unreachable("bad kind");
  }

  /// Do the two given types have the same basic type structure as
  /// far as abstraction patterns are concerned?
  ///
  /// Type structure means tuples, functions, and optionals should
  /// appear in the same positions.
  static bool hasSameBasicTypeStructure(CanType l, CanType r);

  /// Rewrite the type of this abstraction pattern without otherwise
  /// changing its structure.  It is only valid to do this on a pattern
  /// that already stores a type, and the new type must have the same
  /// basic type structure as the old one.
  void rewriteType(CanGenericSignature signature, CanType type) {
    switch (getKind()) {
    case Kind::Invalid:
    case Kind::Opaque:
    case Kind::Tuple:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      llvm_unreachable("type cannot be replaced on pattern without type");
    case Kind::ClangType:
    case Kind::CurriedObjCMethodType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::Type:
    case Kind::Discard:
    case Kind::ObjCCompletionHandlerArgumentsType:
      assert(signature || !type->hasTypeParameter());
      assert(hasSameBasicTypeStructure(OrigType, type));
      GenericSig = (type->hasTypeParameter() ? signature : nullptr);
      OrigType = type;
      return;
    }
    llvm_unreachable("bad kind");
  }

  /// Add substitutions to this pattern.
  AbstractionPattern withSubstitutions(SubstitutionMap subs) const {
    AbstractionPattern result = *this;
    if (subs) {
      // If we have a generic signature, it should match the substitutions.
      // But in corner cases, "match" can mean that it applies to an inner
      // local generic context, which is not something we can easily assert.
      result.GenericSubs = subs;
    }
    return result;
  }

  /// Return whether this abstraction pattern contains foreign type
  /// information.
  ///
  /// In general, after eliminating tuples, a foreign abstraction
  /// pattern will satisfy either isClangType() or isObjCMethod().
  bool isForeign() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::Tuple:
    case Kind::Type:
    case Kind::Discard:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      return false;
    case Kind::ClangType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return true;
    }
    llvm_unreachable("bad kind");
  }

  /// True if the value is discarded.
  bool isDiscarded() const {
    return getKind() == Kind::Discard;
  }

  /// Return whether this abstraction pattern represents a Clang type.
  /// If so, it is legal to return getClangType().
  bool isClangType() const {
    return (getKind() == Kind::ClangType);
  }

  const clang::Type *getClangType() const {
    assert(hasStoredClangType());
    return ClangType;
  }

  /// Return whether this abstraction pattern represents an
  /// Objective-C method.  If so, it is legal to call getObjCMethod().
  bool isObjCMethod() const {
    return (getKind() == Kind::ObjCMethodType ||
            getKind() == Kind::CurriedObjCMethodType);
  }

  const clang::ObjCMethodDecl *getObjCMethod() const {
    assert(hasStoredObjCMethod());
    return ObjCMethod;
  }

  /// Return whether this abstraction pattern represents a C++ method.
  /// If so, it is legal to call getCXXMethod().
  bool isCXXMethod() const {
    return (getKind() == Kind::CXXMethodType ||
            getKind() == Kind::CurriedCXXMethodType);
  }

  const clang::CXXMethodDecl *getCXXMethod() const {
    assert(hasStoredCXXMethod());
    return CXXMethod;
  }

  bool isOpaqueTuple() const {
    return getKind() == Kind::Tuple;
  }

  bool isOpaqueFunctionOrOpaqueDerivativeFunction() const {
    return (getKind() == Kind::OpaqueFunction ||
            getKind() == Kind::OpaqueDerivativeFunction);
  }

  EncodedForeignInfo getEncodedForeignInfo() const {
    assert(hasStoredForeignInfo());
    return EncodedForeignInfo::fromOpaqueValue(OtherData);
  }
  
  bool hasForeignErrorStrippingResultOptionality() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Tuple:
      llvm_unreachable("querying foreign-error bits on non-function pattern");

    case Kind::Opaque:
    case Kind::ClangType:
    case Kind::Type:
    case Kind::Discard:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return false;
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::ObjCMethodType: {
      auto errorInfo = getEncodedForeignInfo();
      return errorInfo.errorStripsResultOptionality();
    }
    }
    llvm_unreachable("bad kind");
  }

  template<typename TYPE>
  typename CanTypeWrapperTraits<TYPE>::type
  getAs() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::Tuple:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      return typename CanTypeWrapperTraits<TYPE>::type();
    case Kind::ClangType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::Type:
    case Kind::Discard:
    case Kind::ObjCCompletionHandlerArgumentsType:
      return dyn_cast<TYPE>(getType());
    }
    llvm_unreachable("bad kind");
  }

  /// Is this pattern the exact given type?
  ///
  /// This is only useful for avoiding redundant work at compile time;
  /// code should be prepared to do the right thing in the face of a slight
  /// mismatch.  This may happen for any number of reasons.
  bool isExactType(CanType type) const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::Tuple:
    case Kind::ClangType:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::ObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
    case Kind::ObjCCompletionHandlerArgumentsType:
      // We assume that the Clang type might provide additional structure.
      return false;
    case Kind::Type:
    case Kind::Discard:
      return getType() == type;
    }
    llvm_unreachable("bad kind");
  }

  /// Is the given tuple type a valid substitution of this abstraction
  /// pattern?  Note that the type doesn't have to be a tuple type in the
  /// case of a vanishing tuple.
  bool matchesTuple(CanType substType) const;

  bool isTuple() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::ObjCMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      return false;
    case Kind::ObjCCompletionHandlerArgumentsType:
    case Kind::Tuple:
      return true;
    case Kind::Type:
    case Kind::Discard:
    case Kind::ClangType:
      return isa<TupleType>(getType());
    }
    llvm_unreachable("bad kind");
  }

  size_t getNumTupleElements() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::ObjCMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
      llvm_unreachable("pattern is not a tuple");      
    case Kind::Tuple:
      return getNumTupleElements_Stored();
    case Kind::ObjCCompletionHandlerArgumentsType:
    case Kind::Type:
    case Kind::Discard:
    case Kind::ClangType:
      return cast<TupleType>(getType())->getNumElements();
    }
    llvm_unreachable("bad kind");
  }

  bool doesTupleContainPackExpansionType() const;

  /// If this type is a tuple type that vanishes (is flattened to its
  /// singleton non-expansion element) under the stored substitutions,
  /// return the abstraction pattern of the surviving element.
  ///
  /// If the surviving element came from an expansion element, the
  /// returned element is the pattern type of the expansion.
  Optional<AbstractionPattern> getVanishingTupleElementPatternType() const;

  static AbstractionPattern
  projectTupleElementType(const AbstractionPattern *base, size_t index) {
    return base->getTupleElementType(index);
  }

  IndexedViewRange<const AbstractionPattern *, AbstractionPattern,
                   projectTupleElementType> getTupleElementTypes() const {
    assert(isTuple());
    return { { this, 0 }, { this, getNumTupleElements() } };
  }

  /// Perform a parallel visitation of the elements of a tuple type,
  /// preserving structure about where pack expansions appear in the
  /// original type and how many elements of the substituted type they
  /// expand to.
  ///
  /// This pattern must be a tuple pattern.  The substituted type may be
  /// a non-tuple only if this is a vanshing tuple pattern.
  void forEachTupleElement(CanType substType,
         llvm::function_ref<void(TupleElementGenerator &element)> fn) const;

  /// Perform a parallel visitation of the elements of a tuple type,
  /// expanding the elements of the type.  This preserves the structure
  /// of the *substituted* tuple type: it will be called once per element
  /// of the substituted type, in order.  The original element trappings
  /// are also provided for convenience.
  ///
  /// This pattern must match the substituted type, but it may be an
  /// opaque pattern.
  void forEachExpandedTupleElement(CanType substType,
      llvm::function_ref<void(AbstractionPattern origEltType,
                              CanType substEltType,
                              const TupleTypeElt &elt)> handleElement) const;

  /// Is the given pack type a valid substitution of this abstraction
  /// pattern?
  bool matchesPack(CanPackType substType);

  bool isPack() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::ObjCMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
    case Kind::ObjCCompletionHandlerArgumentsType:
    case Kind::Tuple:
    case Kind::ClangType:
      return false;
    case Kind::Type:
    case Kind::Discard:
      return isa<PackType>(getType());
    }
    llvm_unreachable("bad kind");
  }

  size_t getNumPackElements() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("querying invalid abstraction pattern!");
    case Kind::Opaque:
    case Kind::PartialCurriedObjCMethodType:
    case Kind::CurriedObjCMethodType:
    case Kind::CFunctionAsMethodType:
    case Kind::CurriedCFunctionAsMethodType:
    case Kind::PartialCurriedCFunctionAsMethodType:
    case Kind::ObjCMethodType:
    case Kind::CXXMethodType:
    case Kind::CurriedCXXMethodType:
    case Kind::PartialCurriedCXXMethodType:
    case Kind::OpaqueFunction:
    case Kind::OpaqueDerivativeFunction:
    case Kind::ObjCCompletionHandlerArgumentsType:
    case Kind::Tuple:
    case Kind::ClangType:
      llvm_unreachable("pattern is not a pack");
    case Kind::Type:
    case Kind::Discard:
      return cast<PackType>(getType())->getNumElements();
    }
    llvm_unreachable("bad kind");
  }

  /// Given that the value being abstracted is a move only type, return the
  /// abstraction pattern with the move only bit removed.
  AbstractionPattern removingMoveOnlyWrapper() const;

  /// Given that the value being abstracted is not a move only type, return the
  /// abstraction pattern with the move only bit added.
  AbstractionPattern addingMoveOnlyWrapper() const;

  /// Given that the value being abstracted is a tuple type, return
  /// the abstraction pattern for an element type.
  AbstractionPattern getTupleElementType(unsigned index) const;

  /// Given that the value being abstracted is a pack element type, return
  /// the abstraction pattern for its pack type.
  AbstractionPattern getPackElementPackType() const;

  /// Given that the value being abstracted is a pack type, return
  /// the abstraction pattern for an element type.
  AbstractionPattern getPackElementType(unsigned index) const;

  /// Given that the value being abstracted is a pack expansion type,
  /// return the underlying pattern type.
  ///
  /// If you're looking for getPackExpansionCountType(), it deliberately
  /// does not exist.  Count types are not lowered types, and the original
  /// count types are not relevant to lowering.  Only the substituted
  /// components and expansion counts are significant.
  AbstractionPattern getPackExpansionPatternType() const;

  /// Given that the value being abstracted is a pack expansion type,
  /// return the appropriate pattern type for the given expansion
  /// component.
  AbstractionPattern getPackExpansionComponentType(CanType substType) const;
  AbstractionPattern getPackExpansionComponentType(bool isExpansion) const;

  /// Given that the value being abstracted is a metatype type, return
  /// the abstraction pattern for its instance type.
  AbstractionPattern getMetatypeInstanceType() const;

  /// Given that the value being abstracted is a dynamic self type, return
  /// the abstraction pattern for its self type.
  AbstractionPattern getDynamicSelfSelfType() const;

  /// Given that the value being abstracted is a parameterized protocol
  /// type, return the abstraction pattern for one of its argument types.
  AbstractionPattern getParameterizedProtocolArgType(unsigned i) const;

  /// Given that the value being abstracted is a function, return the
  /// abstraction pattern for its result type.
  AbstractionPattern getFunctionResultType() const;

  /// Given that the value being abstracted is a function type, return
  /// the abstraction pattern for one of its parameter types.
  AbstractionPattern getFunctionParamType(unsigned index) const;

  /// Given that the value being abstracted is a function type, and that
  /// this is not an opaque abstraction pattern, return the parameter flags
  /// for one of its parameters.
  ParameterTypeFlags getFunctionParamFlags(unsigned index) const;

  /// Given that the value being abstracted is a function type, and that
  /// this is not an opaque abstraction pattern, return the number of
  /// parameters in the pattern.
  unsigned getNumFunctionParams() const;

  /// Traverses the parameters of a function, where this is the
  /// abstraction pattern for the function (its "original type")
  /// and the given parameters are the substituted formal parameters.
  /// Calls the callback once for each parameter in the abstraction
  /// pattern.
  ///
  /// If this is not a function pattern, calls handleScalar for each
  /// parameter of the substituted function type.  Note that functions
  /// with pack expansions cannot be legally abstracted this way; it
  /// is not possible in Swift's ABI to support this without some sort
  /// of dynamic argument-forwarding thunk.
  void forEachFunctionParam(AnyFunctionType::CanParamArrayRef substParams,
                            bool ignoreFinalParam,
    llvm::function_ref<void(FunctionParamGenerator &param)> function) const;

  /// Given that the value being abstracted is optional, return the
  /// abstraction pattern for its object type.
  AbstractionPattern getOptionalObjectType() const;

  /// If this pattern refers to a reference storage type, look through
  /// it.
  AbstractionPattern getReferenceStorageReferentType() const;

  /// Give that the value being abstracted is an existential, return the
  /// underlying constraint type.
  AbstractionPattern getExistentialConstraintType() const;

  /// Given that the value being abstracted is a function type, return the
  /// abstraction pattern for the derivative function.
  ///
  /// The arguments are the same as the arguments to
  /// `AnyFunctionType::getAutoDiffDerivativeFunctionType()`.
  AbstractionPattern getAutoDiffDerivativeFunctionType(
      IndexSubset *parameterIndices, AutoDiffDerivativeFunctionKind kind,
      LookupConformanceFn lookupConformance,
      GenericSignature derivativeGenericSignature = GenericSignature(),
      bool makeSelfParamFirst = false);

  /// If this pattern refers to a foreign ObjC method that was imported as async, this returns
  /// the abstraction pattern for the completion callback with the original ObjC block type.
  ///
  /// Otherwise, this produces the default fully-concrete abstraction pattern for the given
  /// Swift type.
  AbstractionPattern getObjCMethodAsyncCompletionHandlerType(
                                     CanType swiftCompletionHandlerType) const;

  /// Given that this is a pack expansion, return the number of components
  /// that it should expand to.  This, and the general correctness of
  /// traversing variadically generic tuple and function types under
  /// substitution, relies on substitutions having been  set properly
  /// on the abstraction pattern; without that, AbstractionPattern assumes
  /// that every component expands to a single pack expansion component,
  /// which will generally only work in specific situations.
  size_t getNumPackExpandedComponents() const;

  /// If this pattern refers to a foreign ObjC method that was imported as 
  /// async, return the bridged-back-to-ObjC completion handler type.
  CanType getObjCMethodAsyncCompletionHandlerForeignType(
      ForeignAsyncConvention convention,
      Lowering::TypeConverter &TC
  ) const;

  /// How values are passed or returned according to this abstraction pattern.
  enum CallingConventionKind {
    // Value is passed or returned directly as a unit.
    Direct,
    // Value is passed or returned indirectly through memory.
    Indirect,
    // Value is a tuple that is destructured, and each element is considered
    // independently.
    Destructured,
  };

  /// Given that this is a pack expansion, do the pack elements need to be
  /// passed indirectly?
  bool arePackElementsPassedIndirectly(TypeConverter &TC) const;
  
  /// If this abstraction pattern appears in function return position, how is
  /// the corresponding value returned?
  CallingConventionKind getResultConvention(TypeConverter &TC) const;
  
  /// If this abstraction pattern appears in function parameter position, how
  /// is the corresponding value passed?
  CallingConventionKind getParameterConvention(TypeConverter &TC) const;
  
  /// Generate the abstraction pattern for lowering the substituted SIL
  /// function type for a function type matching this abstraction pattern.
  ///
  /// This abstraction pattern must be a function abstraction pattern, matching
  /// \c substType .
  ///
  /// Where the abstraction pattern involves substitutable types, in order
  /// to minimize function conversions, we extract those positions out into
  /// fresh generic arguments, with the minimum set of constraints necessary
  /// to maintain the calling convention (such as passed-directly or
  /// passed-indirectly) as well as satisfy requirements of where the generic
  /// argument structurally appears in the type.
  /// The goal is for similar-shaped generic function types to remain
  /// canonically equivalent, like `(T, U) -> ()`, `(T, T) -> ()`,
  /// `(U, T) -> ()` or `(T, T.A) -> ()` when given substitutions that produce
  /// the same function types.
  ///
  /// Returns a new AbstractionPattern to use for type lowering, as well as
  /// the SubstitutionMap used to map `substType` into the new abstraction
  /// pattern's generic environment, and the coroutine yield type mapped into
  /// the generic environment of the new abstraction pattern.
  std::tuple<AbstractionPattern, SubstitutionMap, AbstractionPattern>
  getSubstFunctionTypePattern(CanAnyFunctionType substType,
                              TypeConverter &TC,
                              AbstractionPattern coroutineYieldOrigType,
                              CanType coroutineYieldSubstType) const;
  
  void dump() const LLVM_ATTRIBUTE_USED;
  void print(raw_ostream &OS) const;
  
  bool operator==(const AbstractionPattern &other) const;
  bool operator!=(const AbstractionPattern &other) const {
    return !(*this == other);
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const AbstractionPattern &pattern) {
  pattern.print(out);
  return out;
}

}
}

#endif

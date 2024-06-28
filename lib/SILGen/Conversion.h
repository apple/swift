//===--- Conversion.h - Types for value conversion --------------*- C++ -*-===//
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
// Defines the Conversion class as well as ConvertingInitialization.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_CONVERSION_H
#define SWIFT_LOWERING_CONVERSION_H

#include "swift/Basic/Assertions.h"
#include "swift/Basic/ExternalUnion.h"
#include "Initialization.h"
#include "SGFContext.h"

namespace swift {
namespace Lowering {

class OptionalInjectionConversion;

/// An abstraction representing certain kinds of conversion that SILGen can
/// do automatically in various situations.  These are used primarily with
/// ConvertingInitialization in order to guide the emission of an expression.
/// Some of these conversions are semantically required, such as propagating
/// @isolated(any) down to the emission of the function reference, or some of
/// the bridging conversions.
class Conversion {
public:
  enum KindTy {
    /// A bridging conversion to a foreign type.
    BridgeToObjC,

    /// A bridging conversion to a foreign type following a force.
    /// Although it's not reflected in the name, this is always an
    /// implicit force cast.
    ForceAndBridgeToObjC,

    /// Force an optional value.
    ForceOptional,

    /// A bridging conversion from a foreign type.
    BridgeFromObjC,

    /// A bridging conversion for a function result.
    BridgeResultFromObjC,

    /// An erasure to Any (possibly wrapped in optional conversions).
    /// This is sortof a bridging conversion?  Really it's more of a
    /// subtype conversion, but we're calling it out separately here,
    /// and that's easier.
    AnyErasure,

    /// A subtype conversion, except it's allowed to do optional injections
    /// and existential erasures.  This comes up with bridging peepholes
    /// and is annoying to not have a way to represent.  The conversion
    /// should always involve class references and so will be harmless
    /// in terms of representations.
    BridgingSubtype,

    /// A subtype conversion.
    Subtype,

    /// A reabstraction conversion.  There can also be a subtype difference
    /// between the substituted types.
    Reabstract,
  };

  static bool isBridgingKind(KindTy kind) {
    switch (kind) {
    case BridgeToObjC:
    case ForceAndBridgeToObjC:
    case ForceOptional:
    case BridgeFromObjC:
    case BridgeResultFromObjC:
    case AnyErasure:
    case BridgingSubtype:
    case Subtype:
      return true;

    case Reabstract:
      return false;
    }
    llvm_unreachable("bad kind");
  }
  
  static bool isReabstractionKind(KindTy kind) {
    switch (kind) {
    case Reabstract:
      return true;

    case BridgeToObjC:
    case ForceAndBridgeToObjC:
    case ForceOptional:
    case BridgeFromObjC:
    case BridgeResultFromObjC:
    case AnyErasure:
    case BridgingSubtype:
    case Subtype:
      return false;
    }
    llvm_unreachable("bad kind");
  }

private:
  KindTy Kind;

  struct BridgingStorage {
    bool IsExplicit;
  };

  /// The types we store for reabstracting contexts.  In general, when
  /// we're just emitting an expression, it's expected that the input
  /// abstraction type and lowered type will match the input formal type,
  /// which will be the type of the expression we're emitting.  They can
  /// therefore simply be replaced if we're e.g. prepending a subtype
  /// conversion to the reabstraction.  But it's very useful to be able to
  /// represent both sides of the conversion uniformly so that e.g. we can
  /// elegantly perform a single (perhaps identity) reabstraction when
  /// receiving a function result or loading a value from abstracted
  /// storage.
  struct ReabstractionStorage {
    AbstractionPattern InputOrigType;
    AbstractionPattern OutputOrigType;
    SILType InputLoweredTy;
  };

  CanType SourceType;
  CanType ResultType;
  SILType LoweredResultType;

  using Members = ExternalUnionMembers<BridgingStorage, ReabstractionStorage>;

  static Members::Index getStorageIndexForKind(KindTy kind) {
    switch (kind) {
    case BridgeToObjC:
    case ForceAndBridgeToObjC:
    case ForceOptional:
    case BridgeFromObjC:
    case BridgeResultFromObjC:
    case AnyErasure:
    case BridgingSubtype:
    case Subtype:
      return Members::indexOf<BridgingStorage>();

    case Reabstract:
      return Members::indexOf<ReabstractionStorage>();
    }
    llvm_unreachable("bad kind");
  }

  ExternalUnion<KindTy, Members, getStorageIndexForKind> Types;
  static_assert(decltype(Types)::union_is_trivially_copyable,
                "define the special members if this changes");

  Conversion(KindTy kind, CanType sourceType, CanType resultType,
             SILType loweredResultTy, bool isExplicit)
      : Kind(kind), SourceType(sourceType), ResultType(resultType),
        LoweredResultType(loweredResultTy) {
    Types.emplaceAggregate<BridgingStorage>(kind, isExplicit);
  }

  Conversion(AbstractionPattern inputOrigType, CanType inputSubstType,
             SILType inputLoweredTy,
             AbstractionPattern outputOrigType, CanType outputSubstType,
             SILType outputLoweredTy)
      : Kind(Reabstract), SourceType(inputSubstType),
        ResultType(outputSubstType),
        LoweredResultType(outputLoweredTy) {
    Types.emplaceAggregate<ReabstractionStorage>(Kind, inputOrigType,
                                                 outputOrigType,
                                                 inputLoweredTy);
  }

  static bool isAllowedConversion(CanType inputType, CanType outputType) {
    // Allow all identity conversions.  (This should only happen with
    // reabstraction.)
    if (inputType == outputType) return true;

    // Allow optional-to-optional conversions, but not injections
    // into optional.  Emitters can be expected to just strip optionality
    // from the result type when peepholing through an optional injection,
    // and doing so avoids the need to handle injections specially in
    // emitters, like those for function references and closures.
    while (auto outputObjectType = outputType.getOptionalObjectType()) {
      auto inputObjectType = inputType.getOptionalObjectType();
      if (!inputObjectType) return false;
      outputType = outputObjectType;
      inputType = inputObjectType;
    }

    // Disallow existential erasures from being directly represented here
    // because it may involve a representation change for the value.  Emitters
    // shouldn't have to specially recognize those.
    if (outputType.isExistentialType())
      return inputType.isExistentialType();

    assert(!inputType.getOptionalObjectType());
    return true;
  }

public:
  static Conversion getOrigToSubst(AbstractionPattern origType,
                                   CanType substType,
                                   SILType inputLoweredTy,
                                   SILType outputLoweredTy) {
    return getReabstract(origType, substType, inputLoweredTy,
                         AbstractionPattern(substType), substType, outputLoweredTy);
  }

  static Conversion getSubstToOrig(AbstractionPattern origType,
                                   CanType substType,
                                   SILType inputLoweredTy,
                                   SILType outputLoweredTy) {
    return getReabstract(AbstractionPattern(substType), substType, inputLoweredTy,
                         origType, substType, outputLoweredTy);
  }

  static Conversion getReabstract(AbstractionPattern inputOrigType,
                                  CanType inputSubstType,
                                  SILType inputLoweredTy,
                                  AbstractionPattern outputOrigType,
                                  CanType outputSubstType,
                                  SILType outputLoweredTy) {
    assert(isAllowedConversion(inputSubstType, outputSubstType) &&
           "don't build subtype conversions that do existential erasures");
    return Conversion(inputOrigType, inputSubstType, inputLoweredTy,
                      outputOrigType, outputSubstType, outputLoweredTy);
  }

  static Conversion getBridging(KindTy kind, CanType origType,
                                CanType resultType, SILType loweredResultTy,
                                bool isExplicit = false) {
    assert(isBridgingKind(kind));
    assert((kind != Subtype || isAllowedConversion(origType, resultType)) &&
           "disallowed conversion for subtype relationship");
    return Conversion(kind, origType, resultType, loweredResultTy, isExplicit);
  }

  static Conversion getSubtype(CanType origType, CanType substType,
                               SILType loweredResultTy) {
    return getBridging(Subtype, origType, substType, loweredResultTy);
  }

  KindTy getKind() const {
    return Kind;
  }

  bool isBridging() const {
    return isBridgingKind(getKind());
  }
  
  bool isReabstraction() const {
    return isReabstractionKind(getKind());
  }

  AbstractionPattern getReabstractionInputOrigType() const {
    return Types.get<ReabstractionStorage>(Kind).InputOrigType;
  }

  CanType getReabstractionInputSubstType() const {
    return getSourceType();
  }

  SILType getReabstractionInputLoweredType() const {
    return Types.get<ReabstractionStorage>(Kind).InputLoweredTy;
  }

  AbstractionPattern getReabstractionOutputOrigType() const {
    return Types.get<ReabstractionStorage>(Kind).OutputOrigType;
  }

  CanType getReabstractionOutputSubstType() const {
    return getResultType();
  }

  SILType getReabstractionOutputLoweredType() const {
    return getLoweredResultType();
  }

  bool isBridgingExplicit() const {
    return Types.get<BridgingStorage>(Kind).IsExplicit;
  }

  CanType getSourceType() const {
    return SourceType;
  }

  CanType getResultType() const {
    return ResultType;
  }

  SILType getLoweredResultType() const {
    return LoweredResultType;
  }

  /// Given that this conversion is not one of the specialized bridging
  /// conversion (i.e. it is either a reabstraction or a subtype conversion),
  /// rebuild it with the given source type.
  Conversion withSourceType(AbstractionPattern origSourceType,
                            CanType sourceType,
                            SILType loweredSourceTy) const;
  Conversion withSourceType(SILGenFunction &SGF, CanType sourceType) const;

  /// Given that this conversion is not one of the specialized bridging
  /// conversion (i.e. it is either a reabstraction or a subtype conversion),
  /// rebuild it with the given result type.
  Conversion withResultType(AbstractionPattern origResultType,
                            CanType sourceType,
                            SILType loweredSourceTy) const;

  ManagedValue emit(SILGenFunction &SGF, SILLocation loc,
                    ManagedValue source, SGFContext ctxt) const;

  /// Try to form a conversion that does an optional injection
  /// or optional-to-optional conversion followed by this conversion.
  std::optional<Conversion>
  adjustForInitialOptionalConversions(CanType newSourceType) const;

  /// Try to form a conversion that does a force-value followed by
  /// this conversion.
  std::optional<Conversion> adjustForInitialForceValue() const;

  OptionalInjectionConversion adjustForInitialOptionalInjection() const;

  void dump() const LLVM_ATTRIBUTE_USED;
  void print(llvm::raw_ostream &out) const;
};

/// Information about how to peephole two conversions.
///
/// This is really the private state of SILGenConvert.
class ConversionPeepholeHint {
public:
  enum Kind : uint8_t {
    /// The value will be exactly the right type.
    Identity,

    /// The value needs to be bridged to AnyObject (possibly optionally).
    BridgeToAnyObject,

    /// The value just needs to undergo a subtype conversion.
    Subtype,

    /// The inner conversion is a subtype conversion and can be done implicitly
    /// as part of the outer conversion.
    SubtypeIntoReabstract,

    /// Both conversions are reabstractions and can be combined.
    Reabstract,
  };

private:
  Kind TheKind;
  bool Forced;

public:
  ConversionPeepholeHint(Kind kind, bool forced)
    : TheKind(kind), Forced(forced) {
  }

  Kind getKind() const { return TheKind; }

  /// Does the value need to be forced before the conversion?
  /// This comes up with result conversions where the result was imported
  /// as non-optional, as well as with implicitly unwrapped optionals.
  bool isForced() const { return Forced; }
};

struct CombinedConversions {
  std::optional<Conversion> first;
  std::optional<Conversion> second;

  explicit CombinedConversions() {}
  explicit CombinedConversions(const Conversion &first)
    : first(first) {}
  explicit CombinedConversions(const Conversion &first,
                               const Conversion &second)
    : first(first), second(second) {}
};

bool canPeepholeConversions(SILGenFunction &SGF,
                            const Conversion &outer,
                            const Conversion &inner);

/// The result of trying to combine an optional injection with an existing
/// conversion.
class OptionalInjectionConversion {
  enum Kind {
    None,
    Injection,
    Value
  };

  std::optional<Conversion> conversion;
  Kind kind;

  OptionalInjectionConversion(Kind kind, const Conversion &conv)
    : conversion(conv), kind(kind) {}

public:
  OptionalInjectionConversion() : kind(None) {}
  static OptionalInjectionConversion forInjection(const Conversion &conv) {
    return { Injection, conv };
  }
  static OptionalInjectionConversion forValue(const Conversion &conv) {
    return { Value, conv };
  }

  /// Is the result of this combination a conversion that produces a
  /// value of the original optional type?
  bool isInjection() const {
    return kind == Injection;
  }
  const Conversion &getInjectionConversion() const {
    assert(isInjection());
    return *conversion;
  }

  /// Is the result of this combination a conversion that produces a
  /// value of the element of the original optional type?
  bool isValue() const {
    return kind == Value;
  }
  const Conversion &getValueConversion() const {
    assert(isValue());
    return *conversion;
  }
};

/// An initialization where we ultimately want to apply a conversion to
/// the value before completing the initialization.
///
/// Value generators may call getAsConversion() to check whether an
/// Initialization is one of these.  This adds initWithConvertedValue
/// to the normal set of ways to receive an initializing value.
class ConvertingInitialization final : public Initialization {
private:
  enum StateTy {
    /// Nothing has happened.
    Uninitialized,

    /// The converted value has been set.
    Initialized,

    /// finishInitialization has been called.
    Finished,

    /// The converted value has been extracted.
    Extracted,

    /// We're doing pack initialization instead of the normal state
    /// transition, and we haven't been finished yet.
    PackExpanding,

    /// We're doing pack initialization instead of the normal state
    /// transition, and finishInitialization has been called.
    FinishedPackExpanding,
  };

  StateTy State;

  /// The conversion that needs to be applied to the formal value.
  Conversion TheConversion;

  /// The converted value, set if the initializing code calls tryPeephole,
  /// setReabstractedValue, or copyOrInitValueInto.
  ManagedValue Value;
  SGFContext FinalContext;

  StateTy getState() const {
    return State;
  }
  
  InitializationPtr OwnedSubInitialization;

public:
  ConvertingInitialization(Conversion conversion, SGFContext finalContext)
    : State(Uninitialized), TheConversion(conversion),
      FinalContext(finalContext) {}

  ConvertingInitialization(Conversion conversion,
                           InitializationPtr subInitialization)
    : State(Uninitialized), TheConversion(conversion),
      FinalContext(SGFContext(subInitialization.get())) {
    OwnedSubInitialization = std::move(subInitialization);
  }

  
  /// Return the conversion to apply to the unconverted value.
  const Conversion &getConversion() const {
    return TheConversion;
  }

  /// Return the context into which to emit the converted value.
  SGFContext getFinalContext() const {
    return FinalContext;
  }

  // The three ways to perform this initialization:

  /// Set the converted value for this initialization.
  ///
  /// If the converted value has been emitted into the final context, you
  /// can pass ManagedValue::forInContext() to this function.  In this
  /// case, you must call finishInitialization on the final initialization
  /// yourself prior to calling this.  finishEmission will return
  /// ManagedValue::forInContext().
  ///
  /// Otherwise, if the final context exists, this will forward the value
  /// into it and finish it.  finishEmission will return
  /// ManagedValue::forInContext().
  ///
  /// Otherwise, this will store the value internally, and finishEmission
  /// will return it.
  ///
  /// You must call finishInitialization after calling this.
  void initWithConvertedValue(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue value);

  /// Set the unconverted value for this initialization.  The value will
  /// first be converted.  If the final context has an initialization,
  /// the converted value will be forwarded into it, and finishEmission
  /// will return ManagedValue::forInContext().  Otherwise, finishEmission
  /// will return the converted value.
  ///
  /// You must call finishInitialization after calling this.
  void copyOrInitValueInto(SILGenFunction &SGF, SILLocation loc,
                           ManagedValue value, bool isInit) override;

  /// Given that the result of the given expression needs to sequentially
  /// undergo the given conversion and then this conversion, attempt to
  /// peephole the result.
  ///
  /// If this returns true, this initialization will have been initialized
  /// as if initWithConvertedValue has been called.  You must call
  /// finishInitialization in this path.
  ///
  /// Otherwise, there is no state change for the conversion.
  bool tryPeephole(SILGenFunction &SGF, Expr *E, Conversion innerConversion);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc,
                   Conversion innerConversion, ValueProducerRef producer);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc, ManagedValue value,
                   Conversion innerConversion);

  /// Given that an emitter was able to adjust the conversion when
  /// emitting into this initialization, continue emission into the
  /// new conversion.
  ManagedValue emitWithAdjustedConversion(SILGenFunction &SGF, SILLocation loc,
                                          Conversion adjustedConversion,
                                          ValueProducerRef producer);

  /// Given the unconverted result, i.e. the result of emitting a
  /// value formally of the unconverted type with this initialization
  /// as the SGFContext, produce the converted result.
  ///
  /// If this initialization was initialized, the unconverted result
  /// must be ManagedValue::forInContext(), and vice-versa.
  ///
  /// The result of this function may itself be
  /// ManagedValue::forInContext() if this Initialization was created
  /// with an SGFContext which contains another Initialization.
  ManagedValue finishEmission(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue formalResult);

  // Implement to make the cast work.
  ConvertingInitialization *getAsConversion() override {
    return this;
  }
  
  // Bookkeeping.
  void finishInitialization(SILGenFunction &SGF) override {
    if (getState() == PackExpanding) {
      FinalContext.getEmitInto()->finishInitialization(SGF);
      State = FinishedPackExpanding;
    } else {
      assert(getState() == Initialized);
      State = Finished;
    }
  }

  // Support pack-expansion initialization.
  bool canPerformPackExpansionInitialization() const override {
    if (auto finalInit = FinalContext.getEmitInto())
      return finalInit->canPerformPackExpansionInitialization();
    return false;
  }

  void performPackExpansionInitialization(SILGenFunction &SGF,
                                          SILLocation loc,
                                          SILValue indexWithinComponent,
                llvm::function_ref<void(Initialization *into)> fn) override;
};

} // end namespace Lowering
} // end namespace swift

#endif

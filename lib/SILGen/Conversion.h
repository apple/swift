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

#include "swift/Basic/ExternalUnion.h"
#include "Initialization.h"
#include "SGFContext.h"

namespace swift {
namespace Lowering {

/// An abstraction representing certain kinds of conversion that SILGen can
/// do automatically in various situations.
class Conversion {
public:
  enum KindTy {
    /// A bridging conversion to a foreign type.
    BridgeToObjC,

    /// A bridging conversion to a foreign type following a force.
    /// Although it's not reflected in the name, this is always an
    /// implicit force cast.
    ForceAndBridgeToObjC,

    /// A bridging conversion from a foreign type.
    BridgeFromObjC,

    /// A bridging conversion for a function result.
    BridgeResultFromObjC,

    /// An erasure to Any (possibly wrapped in optional conversions).
    /// This is sortof a bridging conversion.
    AnyErasure,

    LastBridgingKind = AnyErasure,

    /// An orig-to-subst conversion.
    OrigToSubst,

    /// A subst-to-orig conversion.  These can always be annihilated.
    SubstToOrig,
  };

  static bool isBridgingKind(KindTy kind) {
    return kind <= LastBridgingKind;
  }
  
  static bool isReabstractionKind(KindTy kind) {
    // Update if we end up with more kinds!
    return !isBridgingKind(kind);
  }

private:
  KindTy Kind;

  struct BridgingTypes {
    CanType OrigType;
    CanType ResultType;
    SILType LoweredResultType;
    bool IsExplicit;
  };

  struct ReabstractionTypes {
    AbstractionPattern OrigType;
    CanType SubstType;
    SILType LoweredResultType;
  };

  using Members = ExternalUnionMembers<BridgingTypes, ReabstractionTypes>;

  static Members::Index getStorageIndexForKind(KindTy kind) {
    switch (kind) {
    case BridgeToObjC:
    case ForceAndBridgeToObjC:
    case BridgeFromObjC:
    case BridgeResultFromObjC:
    case AnyErasure:
      return Members::indexOf<BridgingTypes>();

    case OrigToSubst:
    case SubstToOrig:
      return Members::indexOf<ReabstractionTypes>();
    }
    llvm_unreachable("bad kind");
  }

  ExternalUnion<KindTy, Members, getStorageIndexForKind> Types;
  static_assert(decltype(Types)::union_is_trivially_copyable,
                "define the special members if this changes");

  Conversion(KindTy kind, CanType origType, CanType resultType,
             SILType loweredResultTy, bool isExplicit)
      : Kind(kind) {
    Types.emplaceAggregate<BridgingTypes>(kind, origType, resultType,
                                          loweredResultTy, isExplicit);
  }

  Conversion(KindTy kind, AbstractionPattern origType, CanType substType,
             SILType loweredResultTy)
      : Kind(kind) {
    Types.emplaceAggregate<ReabstractionTypes>(kind, origType, substType,
                                               loweredResultTy);
  }

public:
  static Conversion getOrigToSubst(AbstractionPattern origType,
                                   CanType substType,
                                   SILType loweredResultTy) {
    return Conversion(OrigToSubst, origType, substType, loweredResultTy);
  }

  static Conversion getSubstToOrig(AbstractionPattern origType,
                                   CanType substType,
                                   SILType loweredResultTy) {
    return Conversion(SubstToOrig, origType, substType, loweredResultTy);
  }

  static Conversion getBridging(KindTy kind, CanType origType,
                                CanType resultType, SILType loweredResultTy,
                                bool isExplicit = false) {
    assert(isBridgingKind(kind));
    return Conversion(kind, origType, resultType, loweredResultTy, isExplicit);
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

  AbstractionPattern getReabstractionOrigType() const {
    return Types.get<ReabstractionTypes>(Kind).OrigType;
  }

  CanType getReabstractionSubstType() const {
    return Types.get<ReabstractionTypes>(Kind).SubstType;
  }

  SILType getReabstractionLoweredResultType() const {
    return Types.get<ReabstractionTypes>(Kind).LoweredResultType;
  }

  bool isBridgingExplicit() const {
    return Types.get<BridgingTypes>(Kind).IsExplicit;
  }

  CanType getBridgingSourceType() const {
    return Types.get<BridgingTypes>(Kind).OrigType;
  }

  CanType getBridgingResultType() const {
    return Types.get<BridgingTypes>(Kind).ResultType;
  }

  SILType getBridgingLoweredResultType() const {
    return Types.get<BridgingTypes>(Kind).LoweredResultType;
  }

  ManagedValue emit(SILGenFunction &SGF, SILLocation loc,
                    ManagedValue source, SGFContext ctxt) const;

  /// Try to form a conversion that does an optional injection
  /// or optional-to-optional conversion followed by this conversion.
  llvm::Optional<Conversion>
  adjustForInitialOptionalConversions(CanType newSourceType) const;

  /// Try to form a conversion that does a force-value followed by
  /// this conversion.
  llvm::Optional<Conversion> adjustForInitialForceValue() const;

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
    Subtype
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

llvm::Optional<ConversionPeepholeHint>
canPeepholeConversions(SILGenFunction &SGF, const Conversion &outerConversion,
                       const Conversion &innerConversion);

/// An initialization where we ultimately want to apply a conversion to
/// the value before completing the initialization.
///
/// Value generators may call getAsConversion() to check whether an
/// Initialization is one of these.  If so, they may call either
/// tryPeephole or setConvertedValue.
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

  /// Set the unconverted value for this initialization.
  void copyOrInitValueInto(SILGenFunction &SGF, SILLocation loc,
                           ManagedValue value, bool isInit) override;

  /// Given that the result of the given expression needs to sequentially
  /// undergo the given conversion and then this conversion, attempt to
  /// peephole the result.  If successful, the value will be set in this
  /// initialization.  The initialization will not yet be finished.
  bool tryPeephole(SILGenFunction &SGF, Expr *E, Conversion innerConversion);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc,
                   Conversion innerConversion, ValueProducerRef producer);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc, ManagedValue value,
                   Conversion innerConversion);

  /// Set the converted value for this initialization.
  void setConvertedValue(ManagedValue value) {
    assert(getState() == Uninitialized);
    assert(!value.isInContext() || FinalContext.getEmitInto());
    Value = value;
    State = Initialized;
  }

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
  
  // Get the abstraction pattern, if any, the value is converted to.
  llvm::Optional<AbstractionPattern> getAbstractionPattern() const override;

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

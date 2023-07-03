//===--- SGFContext.h - Expression emission context -------------*- C++ -*-===//
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

#ifndef SWIFT_SILGEN_SGFCONTEXT_H
#define SWIFT_SILGEN_SGFCONTEXT_H

#include "Initialization.h"

namespace swift {
namespace Lowering {

/// Internal context information for the SILGenFunction visitor.
///
/// In general, emission methods which take an SGFContext indicate
/// that they've initialized the emit-into buffer (if they have) by
/// returning a "isInContext()" ManagedValue of whatever type.  Callers who
/// propagate down an SGFContext that might have an emit-into buffer must be
/// aware of this.
///
/// Clients of emission routines that take an SGFContext can also specify that
/// they are ok getting back an RValue at +0 instead of requiring it to be at
/// +1.  The client is then responsible for checking the ManagedValue to see if
/// it got back a ManagedValue at +0 or +1.
class SGFContext {
  enum DesiredTransfer {
    PlusOne,
    ImmediatePlusZero,
    GuaranteedPlusZero,
  };
  llvm::PointerIntPair<Initialization *, 2, DesiredTransfer> state;
public:
  SGFContext() = default;

  enum AllowImmediatePlusZero_t {
    /// The client is okay with getting a +0 value and plans to use it
    /// immediately.
    ///
    /// For example, in this context, it would be okay to return +0
    /// even for a load from a mutable variable, because the only way
    /// the value could be invalidated before it's used is a race
    /// condition.
    AllowImmediatePlusZero
  };

  enum AllowGuaranteedPlusZero_t {
    /// The client is okay with getting a +0 value as long as it's
    /// guaranteed to last at least as long as the current evaluation.
    /// (For expression evaluation, this generally means at least
    /// until the end of the current statement.)
    ///
    /// For example, in this context, it would be okay to return +0
    /// for a reference to a local 'let' because that will last until
    /// the 'let' goes out of scope.  However, it would not be okay to
    /// return +0 for a load from a mutable 'var', because that could
    /// be mutated before the end of the statement.
    AllowGuaranteedPlusZero
  };

  /// Creates an emitInto context that will store the result of the visited expr
  /// into the given Initialization.
  explicit SGFContext(Initialization *emitInto) : state(emitInto, PlusOne) {
  }

  /*implicit*/
  SGFContext(AllowImmediatePlusZero_t) : state(nullptr, ImmediatePlusZero) {
  }

  /*implicit*/
  SGFContext(AllowGuaranteedPlusZero_t) : state(nullptr, GuaranteedPlusZero) {
  }

  /// Returns a pointer to the Initialization that the current expression should
  /// store its result to, or null if the expression should allocate temporary
  /// storage for its result.
  Initialization *getEmitInto() const {
    return state.getPointer();
  }

  /// Try to get the address of the emit-into initialization if we can.
  /// Otherwise, return an empty SILValue.
  ///
  /// Note that, if this returns a non-empty address, the caller must
  /// finish the emit-into initialization.
  SILValue getAddressForInPlaceInitialization(SILGenFunction &SGF,
                                              SILLocation loc) const {
    if (auto *init = getEmitInto()) {
      if (init->canPerformInPlaceInitialization())
        return init->getAddressForInPlaceInitialization(SGF, loc);
    }
    return SILValue();
  }

  /// If getAddressForInPlaceInitialization did (or would have)
  /// returned a non-null address, finish the initialization and
  /// return true.  Otherwise, return false.
  bool finishInPlaceInitialization(SILGenFunction &SGF) const {
    if (auto *init = getEmitInto()) {
      if (init->canPerformInPlaceInitialization()) {
        init->finishInitialization(SGF);
        return true;
      }
    }

    return false;
  }

  /// Try to get this context as a conversion initialization.
  ///
  /// This does not commit the caller to anything, whether it succeeds
  /// or fails.
  ConvertingInitialization *getAsConversion() const {
    if (auto init = getEmitInto())
      return init->getAsConversion();
    return nullptr;
  }

  /// Return true if a ManagedValue producer is allowed to return at
  /// +0, given that it cannot guarantee that the value will be valid
  /// until the end of the current evaluation.
  bool isImmediatePlusZeroOk() const {
    return state.getInt() == ImmediatePlusZero;
  }

  /// Return true if a ManagedValue producer is allowed to return at
  /// +0 if it can guarantee that the value will be valid until the
  /// end of the current evaluation.
  bool isGuaranteedPlusZeroOk() const {
    // Either ImmediatePlusZero or GuaranteedPlusZero is fine.
    return state.getInt() >= ImmediatePlusZero;
  }

  /// Get a context for a sub-expression given that arbitrary side
  /// effects may follow the subevaluation.
  SGFContext withFollowingSideEffects() const {
    SGFContext copy = *this;
    if (copy.state.getInt() == ImmediatePlusZero) {
      copy.state.setInt(GuaranteedPlusZero);
    }
    return copy;
  }

  /// Get a context for a sub-expression where we plan to project out
  /// a value.  The Initialization is not okay to propagate down, but
  /// the +0/+1-ness is.
  SGFContext withFollowingProjection() const {
    SGFContext result;
    result.state.setInt(state.getInt());
    return result;
  }

  /// Get a context for a sub-expression where we plan to evaluate arbitrary
  /// side-effects. This means we propagate down the initialization, but
  /// eliminates the +0/+1-ness.
  SGFContext withSubExprSideEffects() const {
    if (auto *init = getEmitInto()) {
      return SGFContext(init);
    }

    return SGFContext();
  }
  
  /// Return the abstraction pattern of the context we're emitting into.
  llvm::Optional<AbstractionPattern> getAbstractionPattern() const {
    if (auto *init = getEmitInto()) {
      return init->getAbstractionPattern();
    }
    return llvm::None;
  }
};

using ValueProducerRef =
  llvm::function_ref<ManagedValue(SILGenFunction &SGF, SILLocation loc,
                                  SGFContext context)>;

} // end namespace Lowering
} // end namespace swift

#endif

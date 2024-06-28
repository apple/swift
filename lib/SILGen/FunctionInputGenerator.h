//===--- FunctionInputGenerator.h - Generator for formal params -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines FunctionInputGenerator, which generates the sequence
// of formal parameters for a SIL function given its abstraction pattern,
// informing the caller of important information such as the orig and subst
// types and whether the parameter is passed as part of a pack.  (It's this
// latter possibility that makes this relatively complicated.)
//
// The assumption is that this will be used as part of binding the parameters
// for a function and that the client of the traversal will claim those
// parameters one by one as the traversal goes on.
//
// In order to properly handle packs of parameters, and especially empty
// packs of parameters, this generator must claim the pack parameters
// as it goes.  As a result, it's actually quite important that clients
// use the pattern above of claiming non-pack parameter values immediately
// while traversing a parameter and before advancing to the next parameter.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_FUNCTIONINPUTGENERATOR_H
#define SWIFT_SILGEN_FUNCTIONINPUTGENERATOR_H

#include "ManagedValue.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Generators.h"

namespace swift {
namespace Lowering {

/// A class for destructuring a list of formal input parameters given
/// an abstraction pattern for it.
class FunctionInputGenerator {
  const ASTContext &ctx;
  SimpleGeneratorRef<ManagedValue> inputs;
  FunctionParamGenerator origParam;

  /// If origParam.isPackExpansion(), this is the pack currently
  /// being destructured.  The cleanup on it is only for the components
  /// starting at substParamIndex.
  ManagedValue packValue;

  /// If origParam.isPackExpansion(), this is the formal type
  /// of the orig parameter pack.
  CanPackType formalPackType;

  /// The current index within origParam.getSubstParams().
  unsigned substParamIndex;

  /// Precondition: we aren't finished visiting orig parameters,
  /// and we've already readied the current orig parameter.
  ///
  /// Ready the next subst parameter (the one at inputSubstParamIndex)
  /// from the current orig parameter.  If we've exhausted the supply
  /// of subst parameters from this orig parameter, advance to the
  /// next orig parameter and repeat.
  ///
  /// Postcondition: we're either finished or properly configured
  /// with a subst parameter that hasn't been presented before.
  void readyNextSubstParameter() {
    while (true) {
      assert(!origParam.isFinished());
      assert(substParamIndex <= origParam.getSubstParams().size());

      // If we haven't reached the limit of the current parameter yet,
      // continue.
      if (substParamIndex != origParam.getSubstParams().size())
        return;

      // Otherwise, advance, and ready the next orig parameter if we
      // didn't finish.
      origParam.advance();
      if (origParam.isFinished()) return;
      readyOrigParameter();
    }
  }

  /// Ready the current orig parameter.
  void readyOrigParameter() {
    substParamIndex = 0;
    if (origParam.isOrigPackExpansion()) {
      // The pack value exists in the lowered parameters and must be
      // claimed whether it contains formal parameters or not.
      packValue = inputs.claimNext();

      // We don't need to do any other set up if we're going to
      // immediately move past it, though.
      if (origParam.getSubstParams().empty())
        return;

      // Compute a formal pack type for the pack.
      formalPackType = CanPackType::get(ctx, origParam.getSubstParams());
    }
  }

public:
  FunctionInputGenerator(const ASTContext &ctx,
                         SimpleGeneratorRef<ManagedValue> inputs,
                         AbstractionPattern origFunctionType,
                         AnyFunctionType::CanParamArrayRef substParams,
                         bool ignoreFinalParam)
    : ctx(ctx), inputs(inputs),
      origParam(origFunctionType, substParams, ignoreFinalParam) {

    if (!origParam.isFinished()) {
      readyOrigParameter();
      readyNextSubstParameter();
    }
  }

  /// Is this generator finished?  If so, the getters below may not be used.
  bool isFinished() const {
    return origParam.isFinished();
  }

  bool isOrigPackExpansion() const {
    return origParam.isOrigPackExpansion();
  }

  AbstractionPattern getOrigType() const {
    return origParam.getOrigType();
  }

  AnyFunctionType::CanParam getSubstParam() const {
    return origParam.getSubstParams()[substParamIndex];
  }

  ManagedValue getPackValue() const {
    assert(isOrigPackExpansion());
    return packValue;
  }

  void updatePackValue(ManagedValue newPackValue) {
    assert(isOrigPackExpansion());
    packValue = newPackValue;
  }

  unsigned getPackComponentIndex() const {
    assert(isOrigPackExpansion());
    return substParamIndex;
  }

  CanPackType getFormalPackType() const {
    assert(isOrigPackExpansion());
    return formalPackType;
  }

  /// Given that we just processed an input, advance to the next,
  /// if there is on.
  ///
  /// Postcondition: either isFinished() or all of the invariants
  /// for the mutable state have been established.
  void advance() {
    assert(!isFinished());
    assert(substParamIndex < origParam.getSubstParams().size());
    substParamIndex++;

    readyNextSubstParameter();
  }

  void finish() {
    origParam.finish();
  }

  /// Project out the current pack component.  Do not call this multiple
  /// times for the same component.
  ManagedValue projectPackComponent(SILGenFunction &SGF, SILLocation loc);
};

} // end namespace Lowering
} // end namespace swift

#endif

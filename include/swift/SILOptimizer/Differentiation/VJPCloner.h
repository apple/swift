//===--- VJPCloner.h - VJP function generation ----------------*- C++ -*---===//
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
// This file defines a helper class for generating VJP functions for automatic
// differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_VJPCLONER_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_VJPCLONER_H

#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"
#include "swift/SILOptimizer/Differentiation/LinearMapInfo.h"
#include "swift/SIL/LoopInfo.h"

namespace swift {
namespace autodiff {

class ADContext;
class PullbackCloner;

/// Consider a function f with type (Ti) -> U, where Ti = {T0, ..., Ti, ... Tn}
/// and U all conform to the Differentiable protocol.
///
/// In that case, VJP(f) or the Vector Jacobian Product of f is defined as:
///
///  VJP(f)(t) : Ti -> ((U.TangentVector) -> (U, Ti.TangentVector))
///  VJP(f)(t)(u) = (f(Ti), f*(Ti)(t2))
///         ^  ^---\ ^      ^--------v
///  original args | normal result   pullback (jacobian) of f wrt args
///                |
///            free parameter passed applied to full derivative
///
/// In words, the VJP(f)(t) is a function that maps the list of vectors Ti to a
/// 2nd function that maps u in U to the tuple (f(t), f*(t)(u)). The first
/// element of this result tuple, is just the constant original result of f at
/// the point t, but the 2nd result is a backpropagation function called the
/// "pullback", which takes the derivatives with respect to the result and
/// returns the derivative with respect to arguments.
///
/// Operationally, this cloner takes in a SILFunction f and maps it during
/// cloning to a SILFunction that returns the above mentioned tuple without
/// evaluating f*(Ti) so that our caller can evaluate the pullback at Ti when
/// they need.
class VJPCloner final {
  class Implementation;
  Implementation &impl;

public:
  /// Creates a VJP cloner.
  ///
  /// The parent VJP cloner stores the original function and an empty
  /// to-be-generated pullback function.
  explicit VJPCloner(ADContext &context, SILDifferentiabilityWitness *witness,
                     SILFunction *vjp, DifferentiationInvoker invoker);
  ~VJPCloner();

  ADContext &getContext() const;
  SILModule &getModule() const;
  SILFunction &getOriginal() const;
  SILFunction &getVJP() const;
  SILFunction &getPullback() const;
  SILDifferentiabilityWitness *getWitness() const;
  const AutoDiffConfig &getConfig() const;
  DifferentiationInvoker getInvoker() const;
  LinearMapInfo &getPullbackInfo() const;
  SILLoopInfo *getLoopInfo() const;
  const DifferentiableActivityInfo &getActivityInfo() const;

  /// Performs VJP generation on the empty VJP function. Returns true if any
  /// error occurs.
  bool run();
};

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_VJPCLONER_H

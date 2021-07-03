//===--- JVPCloner.h - JVP function generation ----------------*- C++ -*---===//
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
// This file defines a helper class for generating JVP functions for automatic
// differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_JVPCLONER_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_JVPCLONER_H

#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"

namespace swift {

class SILFunction;
class SILDifferentiabilityWitness;

namespace autodiff {

class ADContext;

/// Consider a function f with type (Ti) -> U, where Ti = {T0, ..., Ti, ... Tn}
/// and U all conform to the Differentiable protocol.
///
/// In that case, JVP(f) or the Jacobian Vector Product of f is defined as:
///
///  JVP(f)(t1) : Ti -> ((Ti) -> (U, U.TangentVector))
///  JVP(f)(t1)(t2) = (f(Ti), Df(Ti)(t2))
///         ^   ^--\   ^      ^--------v
///  original args |   normal result   full derivative (jacobian) of f wrt args
///                |
///            free parameter passed applied to full derivative
///
/// In words, the JVP(f)(t1) is a function that maps the vector Ti to a 2nd
/// function that maps t2 in T to (f(t1), Df(t1)(t2)). The first element of this
/// result tuple, is just the constant original result of f at the point t1, but
/// the 2nd result is a linear approximation of f at t1 that varies linearly
/// over the input value t2.
///
/// Operationally, this cloner takes in a function f and produces said tuple
/// function, producing the parts of the jacobian needed to evaluate the full
/// derivative.
class JVPCloner final {
  class Implementation;
  Implementation &impl;

public:
  /// Creates a JVP cloner.
  ///
  /// The parent JVP cloner stores the original function and an empty
  /// to-be-generated pullback function.
  explicit JVPCloner(ADContext &context, SILDifferentiabilityWitness *witness,
                     SILFunction *jvp, DifferentiationInvoker invoker);
  ~JVPCloner();

  /// Performs JVP generation on the empty JVP function. Returns true if any
  /// error occurs.
  bool run();

  SILFunction &getJVP() const;
};

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_JVPCLONER_H

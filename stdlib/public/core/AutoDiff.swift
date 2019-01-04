//===--- AutoDiff.swift ---------------------------------------*- swift -*-===//
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
// SWIFT_ENABLE_TENSORFLOW
//
// This file defines support for automatic differentiation.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Compiler Protocols
//===----------------------------------------------------------------------===//

/// A type that represents an unranked vector space. Values of this type are
/// elements in this vector space and have either no shape or a static shape.
public protocol VectorNumeric : AdditiveArithmetic {
  /// The type of scalars in the vector space.
  associatedtype Scalar : AdditiveArithmetic

  static func * (lhs: Scalar, rhs: Self) -> Self
  static func *= (lhs: inout Self, rhs: Scalar)
}

public extension VectorNumeric {
  static func * (lhs: Self, rhs: Scalar) -> Self {
    return rhs * lhs
  }

  static func *= (lhs: inout Self, rhs: Scalar) {
    lhs = rhs * lhs
  }
}

/// A type that represents an unranked vector space. Values of this type are
/// elements in this vector space and have a dynamic shape.
public protocol ShapedVectorNumeric : VectorNumeric {
  /// The type whose values specifies the dimensionality of an object in the
  /// vector space.
  associatedtype Shape

  /// Create an object in the vector space with the specified shape by filling
  /// the object with the specified scalar value.
  ///
  /// - Parameters:
  ///   - shape: the shape
  ///   - repeatedValue: the value repeat for the specified shape
  init(repeating repeatedValue: Scalar, shape: Shape)
}

/// A type that mathematically represents a differentiable manifold whose
/// tangent spaces are finite-dimensional.
public protocol Differentiable {
  /// The tangent bundle of this differentiable manifold.
  associatedtype TangentVector : Differentiable & AdditiveArithmetic
    // FIXME(SR-9595): Unexpected error when type checking constrained
    // associated types.
    where TangentVector.TangentVector == TangentVector,
          TangentVector.CotangentVector == CotangentVector

  /// The cotangent bundle of this differentiable manifold.
  associatedtype CotangentVector : Differentiable & AdditiveArithmetic
    // FIXME(SR-9595): Unexpected error when type checking constrained
    // associated types.
    where CotangentVector.TangentVector == CotangentVector,
          CotangentVector.CotangentVector == TangentVector

  /// Returns `self` moved along the value space towards the given tangent
  /// vector. In Riemannian geometry (mathematics), this represents an
  /// exponential map.
  func moved(along direction: TangentVector) -> Self

  /// Convert a cotangent vector to its corresponding tangent vector.
  func tangentVector(from cotangent: CotangentVector) -> TangentVector
}

// FIXME: The `Self : AdditiveArithmetic` constraint should be implied by
// `TangentVector == Self`, but the type checker errors out when it does not
// exist.
public extension Differentiable
  where TangentVector == Self, Self : AdditiveArithmetic {
  func moved(along direction: TangentVector) -> Self {
    return self + direction
  }
}

// FIXME: This is currently commented because the where clause leads to
// associated type inference which conflicts with `Differentiable` derived
// conformances.
/*
public extension Differentiable where TangentVector == CotangentVector {
  func tangentVector(from cotangent: CotangentVector) -> TangentVector {
    return cotangent
  }
}
*/

//===----------------------------------------------------------------------===//
// Differential Operators
//===----------------------------------------------------------------------===//

/* TODO: This will be available when we support forward-mode differentiation.
@inlinable
public func valueWithDifferential<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> (value: R, differential: (T.TangentVector) -> R.TangentVector)
  where T : Differentiable, R : Differentiable {
  return Builtin.autodiffApply_jvp(f, x)
}
 */

// Value with pullback

@inlinable
public func valueWithPullback<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> (value: R, pullback: (R.CotangentVector) -> T.CotangentVector)
  where T : Differentiable, R : Differentiable {
  return Builtin.autodiffApply_vjp(f, x)
}

@inlinable
public func valueWithPullback<T, U, R>(
  at x: T, _ y: U, in f: @autodiff (T, U) -> R
) -> (value: R,
      pullback: (R.CotangentVector) -> (T.CotangentVector, U.CotangentVector))
  where T : Differentiable, U : Differentiable, R : Differentiable {
  return Builtin.autodiffApply_vjp_arity2(f, x, y)
}

@inlinable
public func valueWithPullback<T, U, V, R>(
  at x: T, _ y: U, _ z: V, in f: @autodiff (T, U, V) -> R
) -> (value: R,
      pullback: (R.CotangentVector)
        -> (T.CotangentVector, U.CotangentVector, V.CotangentVector))
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : Differentiable {
  return Builtin.autodiffApply_vjp_arity3(f, x, y, z)
}

// Pullback

@inlinable
public func pullback<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> (R.CotangentVector) -> T.CotangentVector
  where T : Differentiable, R : Differentiable {
  return Builtin.autodiffApply_vjp(f, x).1
}

@inlinable
public func pullback<T, U, R>(
  at x: T, _ y: U, in f: @autodiff (T, U) -> R
) -> (R.CotangentVector) -> (T.CotangentVector, U.CotangentVector)
  where T : Differentiable, U : Differentiable, R : Differentiable {
  return Builtin.autodiffApply_vjp_arity2(f, x, y).1
}

@inlinable
public func pullback<T, U, V, R>(
  at x: T, _ y: U, _ z: V, in f: @autodiff (T, U, V) -> R
) -> (R.CotangentVector)
    -> (T.CotangentVector, U.CotangentVector, V.CotangentVector)
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : Differentiable {
  return Builtin.autodiffApply_vjp_arity3(f, x, y, z).1
}

/* TODO: These will be available when we support forward-mode differentiation.
@inlinable
public func derivative<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> R.TangentVector
  where T : FloatingPoint, R : Differentiable,
        T.TangentVector == T {
  let (y, differential) = valueWithDifferential(at: x, in: f)
  return differential(R(1))
}

@inlinable
public func derivative<T, R>(
  of f: @escaping @autodiff (T) -> R
) -> (T) -> R.TangentVector
  where T : FloatingPoint, R : Differentiable,
        T.TangentVector == T {
  return { x in derivative(at: x, in: f) }
}
 */

// Value with gradient

@inlinable
public func valueWithGradient<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> (value: R, gradient: T.CotangentVector)
  where T : Differentiable, R : FloatingPoint,
        R.CotangentVector == R {
  let (y, pullback) = valueWithPullback(at: x, in: f)
  return (y, pullback(R(1)))
}

@inlinable
public func valueWithGradient<T, U, R>(
  at x: T, _ y: U, in f: @autodiff (T, U) -> R
) -> (value: R, gradient: (T.CotangentVector, U.CotangentVector))
  where T : Differentiable, U : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  let (y, pullback) = valueWithPullback(at: x, y, in: f)
  return (y, pullback(R(1)))
}

@inlinable
public func valueWithGradient<T, U, V, R>(
  at x: T, _ y: U, _ z: V, in f: @autodiff (T, U, V) -> R
) -> (value: R,
      gradient: (T.CotangentVector, U.CotangentVector, V.CotangentVector))
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  let (y, pullback) = valueWithPullback(at: x, y, z, in: f)
  return (y, pullback(R(1)))
}

// Value with gradient (curried)

@inlinable
public func valueWithGradient<T, R>(
  of f: @escaping @autodiff (T) -> R
) -> (T) -> (value: R, gradient: T.CotangentVector)
  where T : Differentiable, R : FloatingPoint,
        R.CotangentVector == R {
  return { x in valueWithGradient(at: x, in: f) }
}

@inlinable
public func valueWithGradient<T, U, R>(
  of f: @escaping @autodiff (T, U) -> R
) -> (T, U) -> (value: R, gradient: (T.CotangentVector, U.CotangentVector))
  where T : Differentiable, U : Differentiable,
        R : FloatingPoint,
        R.CotangentVector == R {
  return { x, y in valueWithGradient(at: x, y, in: f) }
}

@inlinable
public func valueWithGradient<T, U, V, R>(
  of f: @escaping @autodiff (T, U, V) -> R
) -> (T, U, V)
    -> (value: R,
        gradient: (T.CotangentVector, U.CotangentVector, V.CotangentVector))
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : FloatingPoint & Differentiable,
        R.CotangentVector == R {
  return { x, y, z in valueWithGradient(at: x, y, z, in: f) }
}

// Gradient

@inlinable
public func gradient<T, R>(
  at x: T, in f: @autodiff (T) -> R
) -> T.CotangentVector
  where T : Differentiable, R : FloatingPoint,
        R.CotangentVector == R {
  return pullback(at: x, in: f)(R(1))
}

@inlinable
public func gradient<T, U, R>(
  at x: T, _ y: U, in f: @autodiff (T, U) -> R
) -> (T.CotangentVector, U.CotangentVector)
  where T : Differentiable, U : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  return pullback(at: x, y, in: f)(R(1))
}

@inlinable
public func gradient<T, U, V, R>(
  at x: T, _ y: U, _ z: V, in f: @autodiff (T, U, V) -> R
) -> (T.CotangentVector, U.CotangentVector, V.CotangentVector)
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  return pullback(at: x, y, z, in: f)(R(1))
}

// Gradient (curried)

@inlinable
public func gradient<T, R>(
  of f: @escaping @autodiff (T) -> R
) -> (T) -> T.CotangentVector
  where T : Differentiable, R : FloatingPoint,
        R.CotangentVector == R {
  return { x in gradient(at: x, in: f) }
}

@inlinable
public func gradient<T, U, R>(
  of f: @escaping @autodiff (T, U) -> R
) -> (T, U) -> (T.CotangentVector, U.CotangentVector)
  where T : Differentiable, U : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  return { x, y in gradient(at: x, y, in: f) }
}

@inlinable
public func gradient<T, U, V, R>(
  of f: @escaping @autodiff (T, U, V) -> R
) -> (T, U, V) -> (T.CotangentVector, U.CotangentVector, V.CotangentVector)
  where T : Differentiable, U : Differentiable, V : Differentiable,
        R : FloatingPoint, R.CotangentVector == R {
  return { x, y, z in gradient(at: x, y, z, in: f) }
}

//===----------------------------------------------------------------------===//
// Builtins
//===----------------------------------------------------------------------===//

@usableFromInline @_fixed_layout
class _AutoDiffTape<Element> {}

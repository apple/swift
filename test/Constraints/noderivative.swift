// RUN: %target-typecheck-verify-swift

import _Differentiation

// Allow Type -> @noDerivative Type
//
func test1(_ foo: @escaping @differentiable(reverse) (Float, Float) -> Float) {
    let fn: @differentiable(reverse) (Float, @noDerivative Float) -> Float = foo
    _ = fn(0, 0)
}

// Allow @noDerivative Type -> Type when LHS function is not differentiable
//
func test2(_ foo: @escaping @differentiable(reverse) (Float, @noDerivative Float) -> Float) {
    let fn: (Float, Float) -> Float = foo
    _ = fn(0, 0)
}

// Disallow @noDerivative Type -> Type when LHS function is also differentiable
//
func test3(_ foo: @escaping @differentiable(reverse) (Float, @noDerivative Float) -> Float) {
    // expected-error @+1 {{cannot convert value of type '@differentiable(reverse) (Float, @noDerivative Float) -> Float' to specified type '@differentiable(reverse) (Float, Float) -> Float'}}
    let fn: @differentiable(reverse) (Float, Float) -> Float = foo
    _ = fn(0, 0)
}

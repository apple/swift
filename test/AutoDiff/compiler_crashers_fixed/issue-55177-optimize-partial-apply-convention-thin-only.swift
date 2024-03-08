// RUN: %target-build-swift -Osize %s

// https://github.com/apple/swift/issues/55177
// Fix `partial_apply` optimization

// Do not rewrite `partial_apply` to `thin_to_thick_function` if the specialized
// callee is not `@convention(thin)`.

// FIXME(https://github.com/apple/swift/issues/55466): Disabled due to flakiness on Linux, likely related to TF-1197.


import DifferentiationUnittest

func callback(_ x: inout Tracked<Float>.TangentVector) {}

@differentiable(reverse)
func caller(_ x: Tracked<Float>) -> Tracked<Float> {
  return x.withDerivative(callback)
}

// SIL verification failed: operand of thin_to_thick_function must be thin: opFTy->getRepresentation() == SILFunctionType::Representation::Thin
// Verifying instruction:
//      // function_ref specialized Differentiable._vjpWithDerivative(_:)
//   %10 = function_ref @$s16_Differentiation14DifferentiablePAAE18_vjpWithDerivativeyx5value_13TangentVectorQzAGc8pullbacktyAGzcF0A8Unittest7TrackedVySfG_Tg5 : $@convention(method) (@guaranteed @callee_guaranteed @substituted <τ_0_0> (@inout τ_0_0) -> () for <Tracked<Float>>, @in_guaranteed Tracked<Float>) -> (@out Tracked<Float>, @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Tracked<Float>, Tracked<Float>>) // user: %11
// ->   %11 = thin_to_thick_function %10 : $@convention(method) (@guaranteed @callee_guaranteed @substituted <τ_0_0> (@inout τ_0_0) -> () for <Tracked<Float>>, @in_guaranteed Tracked<Float>) -> (@out Tracked<Float>, @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Tracked<Float>, Tracked<Float>>) to $@callee_guaranteed (@guaranteed @callee_guaranteed @substituted <τ_0_0> (@inout τ_0_0) -> () for <Tracked<Float>>, @in_guaranteed Tracked<Float>) -> (@out Tracked<Float>, @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Tracked<Float>, Tracked<Float>>) // user: %12

// RUN: %target-build-swift %s
// Every so often, this test causes the linker to crash!


// https://github.com/apple/swift/issues/56330
// Fix "multiple consuming users" ownership error caused by
// `VJPCloner::visitApply` related to `@differentiable`-function-typed callees

import _Differentiation

protocol P: Differentiable {
  associatedtype Assoc: Differentiable
}

struct S<T: P> {
  var fn: @differentiable(reverse) (T.Assoc, T.Assoc) -> Float

  func method(y: T.Assoc) {
    _ = gradient(at: y) { y in return self.fn(y, y) }
  }
}

// Original error:
// Begin Error in Function: 'AD__$s4main1SV6method1yy5AssocQz_tFSfAGcfU___vjp_src_0_wrt_0_4main1PRzl'
// Found over consume?!
// Value:   %5 = copy_value %4 : $@differentiable(reverse) @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Float for <τ_0_0.Assoc, τ_0_0.Assoc> // users: %19, %6
// User:   %6 = convert_function %5 : $@differentiable(reverse) @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Float for <τ_0_0.Assoc, τ_0_0.Assoc> to $@differentiable(reverse) @callee_guaranteed (@in_guaranteed τ_0_0.Assoc, @in_guaranteed τ_0_0.Assoc) -> Float // user: %7
// Block: bb0
// Consuming Users:
//   destroy_value %5 : $@differentiable(reverse) @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Float for <τ_0_0.Assoc, τ_0_0.Assoc> // id: %19
//   %6 = convert_function %5 : $@differentiable(reverse) @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Float for <τ_0_0.Assoc, τ_0_0.Assoc> to $@differentiable(reverse) @callee_guaranteed (@in_guaranteed τ_0_0.Assoc, @in_guaranteed τ_0_0.Assoc) -> Float // user: %7

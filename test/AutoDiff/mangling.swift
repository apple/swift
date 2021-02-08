// RUN: %target-swift-frontend -emit-sil -enable-experimental-forward-mode-differentiation -module-name=mangling -verify %s | %FileCheck %s

import _Differentiation

@differentiable
func foo(_ x: Float) -> Float { x }

// CHECK-LABEL: sil_differentiability_witness hidden [parameters 0] [results 0] @$s8mangling3fooyS2fF : $@convention(thin) (Float) -> Float {
// CHECK:  jvp: @$s8mangling3fooyS2fFTJfSpSr : $@convention(thin) (Float) -> (Float, @owned @callee_owned (Float) -> Float)
// CHECK:  vjp: @$s8mangling3fooyS2fFTJrSpSr : $@convention(thin) (Float) -> (Float, @owned @callee_owned (Float) -> Float)

@differentiable
func generic<T: Differentiable>(_ x: T) -> T { x }

// CHECK-LABEL: sil_differentiability_witness hidden [parameters 0] [results 0] <T where T : Differentiable> @$s8mangling7genericyxx16_Differentiation14DifferentiableRzlF : $@convention(thin) <T where T : Differentiable> (@in_guaranteed T) -> @out T {
// CHECK:  jvp: @$s8mangling7genericyxx16_Differentiation14DifferentiableRzlFAcDRzlTJfSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Differentiable> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0.TangentVector, τ_0_0.TangentVector>)
// CHECK:  vjp: @$s8mangling7genericyxx16_Differentiation14DifferentiableRzlFAcDRzlTJrSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Differentiable> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0.TangentVector, τ_0_0.TangentVector>)

@differentiable(where T: Differentiable)
func genericConstrained<T>(_ x: T) -> T { x }

// CHECK-LABEL: sil_differentiability_witness hidden [parameters 0] [results 0] <T where T : Differentiable> @$s8mangling18genericConstrainedyxxlF : $@convention(thin) <T> (@in_guaranteed T) -> @out T {
// CHECK:  jvp: @$s8mangling18genericConstrainedyxxlF16_Differentiation14DifferentiableRzlTJfSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Differentiable> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0.TangentVector, τ_0_0.TangentVector>)
// CHECK:  vjp: @$s8mangling18genericConstrainedyxxlF16_Differentiation14DifferentiableRzlTJrSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Differentiable> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0.TangentVector, τ_0_0.TangentVector>)

class Class: Differentiable {
  @differentiable(wrt: (self, x))
  @differentiable(wrt: x)
  func f(_ x: Float) -> Float { x }
}

// CHECK-LABEL: sil_differentiability_witness hidden [parameters 0] [results 0] @$s8mangling5ClassC1fyS2fF : $@convention(method) (Float, @guaranteed Class) -> Float {
// CHECK:  jvp: @$s8mangling5ClassC1fyS2fFTJfSUpSr : $@convention(method) (Float, @guaranteed Class) -> (Float, @owned @callee_owned (Float) -> Float)
// CHECK:  vjp: @$s8mangling5ClassC1fyS2fFTJrSUpSr : $@convention(method) (Float, @guaranteed Class) -> (Float, @owned @callee_owned (Float) -> Float)

func test<C: Class>(_ c: C, _ x: Float) {
  _ = gradient(at: c, x) { c, x in c.f(x) }
  _ = gradient(at: x) { x in c.f(x) }
}

// CHECK-LABEL: sil_differentiability_witness private [parameters 0] [results 0] <τ_0_0 where τ_0_0 : Class> @$s8mangling4testyyx_SftAA5ClassCRbzlFS2fcfU0_ : $@convention(thin) <C where C : Class> (Float, @guaranteed C) -> Float {
// CHECK:  jvp: @$s8mangling4testyyx_SftAA5ClassCRbzlFS2fcfU0_ADRbzlTJfSUpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Class> (Float, @guaranteed τ_0_0) -> (Float, @owned @callee_owned (Float) -> Float)
// CHECK:  vjp: @$s8mangling4testyyx_SftAA5ClassCRbzlFS2fcfU0_ADRbzlTJrSUpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Class> (Float, @guaranteed τ_0_0) -> (Float, @owned @callee_owned (Float) -> Float)

// CHECK-LABEL: sil_differentiability_witness private [parameters 0 1] [results 0] <τ_0_0 where τ_0_0 : Class> @$s8mangling4testyyx_SftAA5ClassCRbzlFSfx_SftcfU_ : $@convention(thin) <C where C : Class> (@guaranteed C, Float) -> Float {
// CHECK:  jvp: @$s8mangling4testyyx_SftAA5ClassCRbzlFSfx_SftcfU_ADRbzlTJfSSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Class> (@guaranteed τ_0_0, Float) -> (Float, @owned @callee_owned @substituted <τ_0_0> (@guaranteed τ_0_0, Float) -> Float for <τ_0_0.TangentVector>)
// CHECK:  vjp: @$s8mangling4testyyx_SftAA5ClassCRbzlFSfx_SftcfU_ADRbzlTJrSSpSr : $@convention(thin) <τ_0_0 where τ_0_0 : Class> (@guaranteed τ_0_0, Float) -> (Float, @owned @callee_owned @substituted <τ_0_0> (Float) -> (τ_0_0, Float) for <τ_0_0.TangentVector>)

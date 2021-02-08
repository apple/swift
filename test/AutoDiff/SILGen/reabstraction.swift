// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

import _Differentiation

@_silgen_name("triggerReabstraction1")
func triggerReabstraction1<T: Differentiable>(_ f: @escaping @differentiable (T) -> T) {}

@_silgen_name("triggerReabstraction2")
func triggerReabstraction2<T>(_ t: T) {}

@_silgen_name("differentiable1")
func differentiable1(_ x: Float) -> Float { x }

@_silgen_name("differentiable2")
func differentiable2<T: Differentiable>(_ t: T) -> T { t }

@_silgen_name("makeSignatureAbstract")
func makeSignatureAbstract() {
  triggerReabstraction1(differentiable1)
}

// CHECK-LABEL: sil{{.*}}@makeSignatureAbstract
// CHECK:   [[BEFORE:%.*]] = differentiable_function [parameters 0] [results 0]
// CHECK:   [[ORIG_0:%.*]] = differentiable_function_extract [original] [[BEFORE]]
// CHECK:   [[ORIG_1:%.*]] = copy_value [[ORIG_0]]
// CHECK:   [[ORIG_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> Float) -> @out Float
// CHECK:   [[ORIG_2:%.*]] = partial_apply [callee_guaranteed] [[ORIG_THUNK]]([[ORIG_1]])
// CHECK:   [[ORIG_3:%.*]] = convert_function [[ORIG_2]]
// CHECK:   [[JVP_0:%.*]] = differentiable_function_extract [jvp] [[BEFORE]]
// CHECK:   [[JVP_1:%.*]] = copy_value [[JVP_0]]
// CHECK:   [[JVP_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> (Float, @owned @callee_owned (Float) -> Float)) -> (@out Float, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Float, Float>)
// CHECK:   [[JVP_2:%.*]] = partial_apply [callee_guaranteed] [[JVP_THUNK]]([[JVP_1]])
// CHECK:   [[JVP_3:%.*]] = convert_function [[JVP_2]]
// CHECK:   [[VJP_0:%.*]] = differentiable_function_extract [vjp] [[BEFORE]]
// CHECK:   [[VJP_1:%.*]] = copy_value [[VJP_0]]
// CHECK:   [[VJP_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> (Float, @owned @callee_owned (Float) -> Float)) -> (@out Float, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Float, Float>)
// CHECK:   [[VJP_2:%.*]] = partial_apply [callee_guaranteed] [[VJP_THUNK]]([[VJP_1]])
// CHECK:   [[VJP_3:%.*]] = convert_function [[VJP_2]]
// CHECK:   [[AFTER:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_3]] {{.*}} with_derivative {[[JVP_3]] {{.*}}, [[VJP_3]] {{.*}}}
// CHECK:   [[TRIGGER:%.*]] = function_ref @triggerReabstraction1
// CHECK:   apply [[TRIGGER]]<Float>([[AFTER]])

@_silgen_name("makeOpaque")
func makeOpaque() {
  let f: @differentiable (Float) -> Float = differentiable1
  triggerReabstraction2(f)
}

// CHECK-LABEL: sil{{.*}}@makeOpaque
// CHECK:   [[STACK_ADDR:%.*]] = alloc_stack $@differentiable @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Float, Float>
// CHECK:   [[ORIG_0:%.*]] = differentiable_function_extract [original] [[BEFORE:%.*]] : $@differentiable @callee_guaranteed (Float) -> Float
// CHECK:   [[ORIG_1:%.*]] = copy_value [[ORIG_0]]
// CHECK:   [[ORIG_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> Float) -> @out Float
// CHECK:   [[ORIG_2:%.*]] = partial_apply [callee_guaranteed] [[ORIG_THUNK]]([[ORIG_1]])
// CHECK:   [[ORIG_3:%.*]] = convert_function [[ORIG_2]]
// CHECK:   [[JVP_0:%.*]] = differentiable_function_extract [jvp] [[BEFORE]]
// CHECK:   [[JVP_1:%.*]] = copy_value [[JVP_0]]
// CHECK:   [[JVP_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> (Float, @owned @callee_owned (Float) -> Float)) -> (@out Float, @out @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Float, Float>)
// CHECK:   [[JVP_2:%.*]] = partial_apply [callee_guaranteed] [[JVP_THUNK]]([[JVP_1]])
// CHECK:   [[JVP_3:%.*]] = convert_function [[JVP_2]]
// CHECK:   [[VJP_0:%.*]] = differentiable_function_extract [vjp] [[BEFORE]]
// CHECK:   [[VJP_1:%.*]] = copy_value [[VJP_0]]
// CHECK:   [[VJP_THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (@in_guaranteed Float, @guaranteed @callee_guaranteed (Float) -> (Float, @owned @callee_owned (Float) -> Float)) -> (@out Float, @out @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <Float, Float>)
// CHECK:   [[VJP_2:%.*]] = partial_apply [callee_guaranteed] [[VJP_THUNK]]([[VJP_1]])
// CHECK:   [[VJP_3:%.*]] = convert_function [[VJP_2]]
// CHECK:   [[AFTER:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_3]] {{.*}} with_derivative {[[JVP_3]] {{.*}}, [[VJP_3]] {{.*}}}
// CHECK:   store [[AFTER]] to [init] [[STACK_ADDR]]
// CHECK:   [[TRIGGER:%.*]] = function_ref @triggerReabstraction2
// CHECK:   apply [[TRIGGER]]<@differentiable (Float) -> Float>([[STACK_ADDR]])

@_silgen_name("makeSignatureDirect")
func makeSignatureDirect() {
  let _: @differentiable (Float) -> Float = differentiable2
}

// CHECK-LABEL: sil{{.*}}@makeSignatureDirect
// CHECK:  [[ORIG_0:%.*]] = function_ref @differentiable2
// CHECK:  [[ORIG_1:%.*]] = partial_apply [callee_guaranteed] [[ORIG_0]]<Float>()
// CHECK:  [[THUNK:%.*]] = function_ref {{.*}} : $@convention(thin) (Float, @guaranteed @callee_guaranteed (@in_guaranteed Float) -> @out Float) -> Float
// CHECK:  [[ORIG_2:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[ORIG_1]])
// CHECK:  differentiable_function [parameters 0] [results 0] [[ORIG_2]]

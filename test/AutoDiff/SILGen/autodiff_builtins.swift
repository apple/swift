// RUN: %target-swift-frontend -parse-stdlib -emit-silgen %s | %FileCheck %s

import _Differentiation
import Swift

@_silgen_name("f_direct_arity1")
func f_direct_arity1(_ x: Float) -> Float {
  x
}

@_silgen_name("f_direct_arity1_jvp")
func f_direct_arity1_jvp(_ x: Float) -> (Float, (Float) -> Float) {
  (x, { $0 })
}

@_silgen_name("f_direct_arity1_vjp")
func f_direct_arity1_vjp(_ x: Float) -> (Float, (Float) -> Float) {
  (x, { $0 })
}

@_silgen_name("f_direct_arity2")
func f_direct_arity2(_ x: Float, _ y: Float) -> Float {
  x
}

@_silgen_name("f_indirect_arity1")
func f_indirect_arity1<T: AdditiveArithmetic & Differentiable>(_ x: T) -> T {
  x
}

// MARK: - applyDerivative

@_silgen_name("applyDerivative_f_direct_arity1_jvp")
func applyDerivative_f1_jvp(_ x: Float) -> (Float, (Float) -> Float) {
  return Builtin.applyDerivative_jvp(f_direct_arity1, x)
}
// CHECK-LABEL: sil{{.*}}@applyDerivative_f_direct_arity1_jvp
// CHECK: bb0([[X:%.*]] : $Float):
// CHECK: [[D:%.*]] = differentiable_function_extract [jvp]
// CHECK: [[D_RESULT:%.*]] = apply [[D]]([[X]])
// CHECK: ([[D_RESULT_0:%.*]], [[DF:%.*]]) = destructure_tuple [[D_RESULT]]
// CHECK: [[THUNK:%.*]] = function_ref @{{.*}} : $@convention(thin) (Float, @owned @callee_owned (Float) -> Float) -> Float
// CHECK: [[REABSTRACTED_DF:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[DF]])
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : {{.*}}, [[REABSTRACTED_DF]] : {{.*}})
// CHECK: ([[D_RESULT_0:%.*]], [[REABSTRACTED_DF:%.*]]) = destructure_tuple [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> Float)
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : $Float, [[REABSTRACTED_DF]] : $@callee_guaranteed (Float) -> Float)
// CHECK:  return [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> Float)

@_silgen_name("applyDerivative_f_direct_arity1_vjp")
func applyDerivative_f1_vjp(_ x: Float) -> (Float, (Float) -> Float) {
  return Builtin.applyDerivative_vjp(f_direct_arity1, x)
}
// CHECK-LABEL: sil{{.*}}@applyDerivative_f_direct_arity1_vjp
// CHECK: bb0([[X:%.*]] : $Float):
// CHECK: [[D:%.*]] = differentiable_function_extract [vjp]
// CHECK: [[D_RESULT:%.*]] = apply [[D]]([[X]])
// CHECK: ([[D_RESULT_0:%.*]], [[PB:%.*]]) = destructure_tuple [[D_RESULT]]
// CHECK: [[THUNK:%.*]] = function_ref @{{.*}} : $@convention(thin) (Float, @owned @callee_owned (Float) -> Float) -> Float
// CHECK: [[REABSTRACTED_PB:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[PB]])
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : {{.*}}, [[REABSTRACTED_PB]] : {{.*}})
// CHECK: ([[D_RESULT_0:%.*]], [[REABSTRACTED_PB:%.*]]) = destructure_tuple [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> Float)
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : $Float, [[REABSTRACTED_PB]] : $@callee_guaranteed (Float) -> Float)
// CHECK:  return [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> Float)

@_silgen_name("applyDerivative_f_direct_arity2_vjp")
func applyDerivative_f1_vjp(_ x: Float, _ y: Float) -> (Float, (Float) -> (Float, Float)) {
  return Builtin.applyDerivative_vjp_arity2(f_direct_arity2, x, y)
}
// CHECK-LABEL: sil{{.*}}@applyDerivative_f_direct_arity2_vjp
// CHECK: bb0([[X:%.*]] : $Float, [[Y:%.*]] : $Float):
// CHECK: [[D:%.*]] = differentiable_function_extract [vjp]
// CHECK: [[D_RESULT:%.*]] = apply [[D]]([[X]], [[Y]])
// CHECK: ([[D_RESULT_0:%.*]], [[PB:%.*]]) = destructure_tuple [[D_RESULT]]
// CHECK: [[THUNK:%.*]] = function_ref @{{.*}} : $@convention(thin) (Float, @owned @callee_owned (Float) -> (Float, Float)) -> (Float, Float)
// CHECK: [[REABSTRACTED_PB:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[PB]])
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : {{.*}}, [[REABSTRACTED_PB]] : {{.*}})
// CHECK: ([[D_RESULT_0:%.*]], [[REABSTRACTED_PB:%.*]]) = destructure_tuple [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> (Float, Float))
// CHECK: [[D_RESULT_RETUPLED:%.*]] = tuple ([[D_RESULT_0]] : $Float, [[REABSTRACTED_PB]] : $@callee_guaranteed (Float) -> (Float, Float))
// CHECK:  return [[D_RESULT_RETUPLED]] : $(Float, @callee_guaranteed (Float) -> (Float, Float))

@_silgen_name("applyDerivative_f_indirect_arity1_vjp")
func applyDerivative_f1_vjp<T: AdditiveArithmetic & Differentiable>(t0: T) -> (T, (T.TangentVector) -> T.TangentVector) {
  return Builtin.applyDerivative_vjp(f_indirect_arity1, t0)
}
// CHECK-LABEL: sil{{.*}}@applyDerivative_f_indirect_arity1_vjp
// CHECK: bb0([[ORIG_RESULT_OUT_PARAM:%.*]] : $*T, [[X:%.]] : $*T):
// CHECK: [[D:%.*]] = differentiable_function_extract [vjp]
// CHECK: [[D_RESULT_BUFFER:%.*]] = alloc_stack $(T, @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>)
// CHECK: [[D_RESULT_BUFFER_0_FOR_STORE:%.*]] = tuple_element_addr [[D_RESULT_BUFFER]] : ${{.*}}, 0
// CHECK: [[PB:%.*]] = apply [[D]]([[D_RESULT_BUFFER_0_FOR_STORE]], [[X]])
// CHECK: [[PB_SUBST:%.*]] = convert_function [[PB]]
// CHECK: [[THUNK:%.*]] = function_ref @{{.*}} : $@convention(thin) <τ_0_0 where τ_0_0 : AdditiveArithmetic, τ_0_0 : Differentiable> (@in_guaranteed τ_0_0.TangentVector, @owned @callee_owned (@in_guaranteed τ_0_0.TangentVector) -> @out τ_0_0.TangentVector) -> @out τ_0_0.TangentVector
// CHECK: [[PB:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]<T>([[PB_SUBST]]) : $@convention(thin) <τ_0_0 where τ_0_0 : AdditiveArithmetic, τ_0_0 : Differentiable> (@in_guaranteed τ_0_0.TangentVector, @owned @callee_owned (@in_guaranteed τ_0_0.TangentVector) -> @out τ_0_0.TangentVector) -> @out τ_0_0.TangentVector
// CHECK: [[PB_SUBST:%.*]] = convert_function [[PB]] : $@callee_guaranteed (@in_guaranteed T.TangentVector) -> @out T.TangentVector to $@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>
// CHECK: [[PB_BUFFER:%.*]] = tuple_element_addr [[D_RESULT_BUFFER]] : $*(T, @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>), 1
// CHECK: store [[PB_SUBST]] to [init] [[PB_BUFFER]] : $*@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>
// CHECK: [[ORIG_RESULT_BUFFER:%.*]] = tuple_element_addr [[D_RESULT_BUFFER]] : $*(T, @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>), 0
// CHECK: [[PB_BUFFER:%.*]] = tuple_element_addr [[D_RESULT_BUFFER]] : $*(T, @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>), 1
// CHECK: [[PB:%.*]] = load [take] [[PB_BUFFER]] : $*@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector> // user: %25
// CHECK: copy_addr [take] [[ORIG_RESULT_BUFFER]] to [initialization] [[ORIG_RESULT_OUT_PARAM]] : $*T
// CHECK: dealloc_stack [[D_RESULT_BUFFER]] : $*(T, @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>)
// CHECK: return [[PB]] : $@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T.TangentVector, T.TangentVector>

// MARK: - applyTranspose

@_silgen_name("applyTranspose_f_direct_arity1")
func applyTranspose_f_direct_arity1(_ x: Float) -> Float {
  return Builtin.applyTranspose_arity1(f_direct_arity1, x)
}
// CHECK-LABEL: sil{{.*}}@applyTranspose_f_direct_arity1
// CHECK: bb0([[X:%.*]] : $Float):
// CHECK:   [[ORIG:%.*]] = function_ref @f_direct_arity1 : $@convention(thin) (Float) -> Float
// CHECK:   [[THICK_ORIG:%.*]] = thin_to_thick_function [[ORIG]] : $@convention(thin) (Float) -> Float to $@callee_guaranteed (Float) -> Float
// CHECK:   [[LINEAR:%.*]] = linear_function [parameters 0] [[THICK_ORIG]] : $@callee_guaranteed (Float) -> Float
// CHECK:   [[NOESC_LINEAR:%.*]] = convert_escape_to_noescape [not_guaranteed] [[LINEAR]] : $@differentiable(linear) @callee_guaranteed (Float) -> Float to $@differentiable(linear) @noescape @callee_guaranteed (Float) -> Float
// CHECK:   [[TRANS:%.*]] = linear_function_extract [transpose] [[NOESC_LINEAR]] : $@differentiable(linear) @noescape @callee_guaranteed (Float) -> Float
// CHECK:   [[RESULT:%.*]] = apply [[TRANS]]([[X]]) : $@noescape @callee_guaranteed (Float) -> Float
// CHECK:   return [[RESULT]] : $Float
// CHECK: } // end sil function 'applyTranspose_f_direct_arity1'

@_silgen_name("applyTranspose_f_direct_arity2")
func applyTranspose_f_direct_arity2(_ x: Float) -> (Float, Float) {
  return Builtin.applyTranspose_arity2(f_direct_arity2, x)
}

// CHECK-LABEL: sil{{.*}}@applyTranspose_f_direct_arity2 :
// CHECK: bb0([[X:%.*]] : $Float)
// CHECK:   [[ORIG:%.*]] = function_ref @f_direct_arity2 : $@convention(thin) (Float, Float) -> Float
// CHECK:   [[THICK_ORIG:%.*]] = thin_to_thick_function [[ORIG]] : $@convention(thin) (Float, Float) -> Float to $@callee_guaranteed (Float, Float) -> Float
// CHECK:   [[LINEAR:%.*]] = linear_function [parameters 0 1] [[THICK_ORIG]] : $@callee_guaranteed (Float, Float) -> Float
// CHECK:   [[NOESC_LINEAR:%.*]] = convert_escape_to_noescape [not_guaranteed] [[LINEAR]] : $@differentiable(linear) @callee_guaranteed (Float, Float) -> Float to $@differentiable(linear) @noescape @callee_guaranteed (Float, Float) -> Float
// CHECK:   [[TRANS:%.*]] = linear_function_extract [transpose] [[NOESC_LINEAR]] : $@differentiable(linear) @noescape @callee_guaranteed (Float, Float) -> Float
// CHECK:   [[RESULT:%.*]] = apply [[TRANS]]([[X]]) : $@noescape @callee_guaranteed (Float) -> (Float, Float)
// CHECK:   ([[RES1:%.*]], [[RES2:%.*]]) = destructure_tuple [[RESULT]] : $(Float, Float)
// CHECK:   [[RESULT:%.*]] = tuple ([[RES1]] : $Float, [[RES2]] : $Float)
// CHECK:   return [[RESULT]] : $(Float, Float)
// CHECK: } // end sil function 'applyTranspose_f_direct_arity2'

@_silgen_name("applyTranspose_f_indirect_arity1")
func applyTranspose_f_indirect_arity1<T: AdditiveArithmetic & Differentiable>(_ x: T) -> T {
  return Builtin.applyTranspose_arity1(f_indirect_arity1, x)
}
// CHECK-LABEL: sil{{.*}}@applyTranspose_f_indirect_arity1
// CHECK: bb0([[OUT_PARAM:%.*]] : $*T, [[X:%.*]] : $*T):
// CHECK: [[RESULT:%.*]] = apply [[TRANSPOSE:%.*]]([[OUT_PARAM]], [[X]])

struct ExamplePullbackStruct<T: Differentiable> {
  var pb0: (T.TangentVector) -> T.TangentVector
}

@_silgen_name("test_context_builtins")
func test_context_builtins() {
  let pbStruct = ExamplePullbackStruct<Float>(pb0: { $0 })
  let context = Builtin.autoDiffCreateLinearMapContext(Builtin.sizeof(type(of: pbStruct)))
  let topLevelSubctxAddr = Builtin.autoDiffProjectTopLevelSubcontext(context)
  UnsafeMutableRawPointer(topLevelSubctxAddr).storeBytes(of: pbStruct, as: type(of: pbStruct))
  let newBuffer = Builtin.autoDiffAllocateSubcontext(context, Builtin.sizeof(type(of: pbStruct)))
  UnsafeMutableRawPointer(newBuffer).storeBytes(of: pbStruct, as: type(of: pbStruct))
}

// CHECK-LABEL: sil{{.*}}@test_context_builtins
// CHECK: bb0:
// CHECK:   [[CTX:%.*]] = builtin "autoDiffCreateLinearMapContext"({{%.*}} : $Builtin.Word) : $Builtin.NativeObject
// CHECK:   [[BORROWED_CTX:%.*]] = begin_borrow [[CTX]] : $Builtin.NativeObject
// CHECK:   [[BUF:%.*]] = builtin "autoDiffProjectTopLevelSubcontext"([[BORROWED_CTX]] : $Builtin.NativeObject) : $Builtin.RawPointer
// CHECK:   [[BORROWED_CTX:%.*]] = begin_borrow [[CTX]] : $Builtin.NativeObject
// CHECK:   [[BUF:%.*]] = builtin "autoDiffAllocateSubcontext"([[BORROWED_CTX]] : $Builtin.NativeObject, {{.*}} : $Builtin.Word) : $Builtin.RawPointer
// CHECK:   destroy_value [[CTX]]

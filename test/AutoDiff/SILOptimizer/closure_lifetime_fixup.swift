// RUN: %target-swift-frontend -emit-sil -verify -Xllvm --sil-print-after=closure-lifetime-fixup %s 2>&1 | %FileCheck %s

import _Differentiation

@differentiable(reverse)
func oneOperation(a: Float) -> Float {
    return a * 2
}

// Lifetimes of differentiable functions derived from `thin_to_thick_function` 
// closures are not fixed up by the closure lifetime fixup pass.

@_silgen_name("mustNotBeFixedUpWithLoops")
func f(x: Float) -> Float {
  var obj: Float = x

  for i in 1...1000 {
    obj += gradient(at: Float(i), of: oneOperation)
  }

  return obj
}
// CHECK-LABEL: sil hidden [ossa] @mustNotBeFixedUpWithLoops : $@convention(thin) (Float) -> Float {
// CHECK: bb0
// CHECK: [[FR:%.*]] = function_ref @$s22closure_lifetime_fixup12oneOperation1aS2f_tF
// CHECK: [[TTTF:%.*]] = thin_to_thick_function [[FR]]
// CHECK: [[DF:%.*]] = differentiable_function [parameters 0] [results 0] [[TTTF]]
// CHECK: [[NEDF:%.*]] = convert_escape_to_noescape [[DF]]
// CHECK-NOT: enum $Optional
// CHECK-NOT: mark_dependence
// CHECK: [[BNEDF:%.*]] = begin_borrow [[NEDF]]

// CHECK: [[DFEO:%.*]] = differentiable_function_extract [original] [[BNEDF]]
// CHECK: [[CDFEO:%.*]] = copy_value [[DFEO]]
// CHECK: [[THUNK1:%.*]] = function_ref @$sS2fIgyd_S2fIegnr_TR
// CHECK: [[THUNKED1:%.*]] = partial_apply [callee_guaranteed] [[THUNK1]]([[CDFEO]])
// CHECK: [[CVT1:%.*]] = convert_function [[THUNKED1]]
// CHECK: [[CVT2:%.*]] = convert_escape_to_noescape [[CVT1]]
// CHECK-NOT: enum $Optional
// CHECK-NOT: mark_dependence

// CHECK: [[DFEJ:%.*]] = differentiable_function_extract [jvp] [[BNEDF]]
// CHECK: [[CDFEJ:%.*]] = copy_value [[DFEJ]]
// CHECK: [[THUNK2:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED2:%.*]] = partial_apply [callee_guaranteed] [[THUNK2]]([[CDFEJ]])
// CHECK: [[CVT3:%.*]] = convert_function [[THUNKED2]]
// CHECK: [[CVT4:%.*]] = convert_escape_to_noescape [[CVT3]]
// CHECK-NOT: enum $Optional
// CHECK-NOT: mark_dependence

// CHECK: [[DFEV:%.*]] = differentiable_function_extract [vjp] [[BNEDF]]
// CHECK: [[CDFEV:%.*]] = copy_value [[DFEV]]
// CHECK: [[THUNK3:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED3:%.*]] = partial_apply [callee_guaranteed] [[THUNK3]]([[CDFEV]])
// CHECK: [[CVT5:%.*]] = convert_function [[THUNKED3]]
// CHECK: [[CVT6:%.*]] = convert_escape_to_noescape [[CVT5]]
// CHECK-NOT: enum $Optional
// CHECK-NOT: mark_dependence

  
@_silgen_name("mustNotBeFixedUp")
func g(x: Float) -> Float {
  return gradient(at: Float(x), of: oneOperation)
}
// CHECK-LABEL: sil hidden [ossa] @mustNotBeFixedUp : $@convention(thin) (Float) -> Float {
// CHECK: bb0
// CHECK: [[FR:%.*]] = function_ref @$s22closure_lifetime_fixup12oneOperation1aS2f_tF
// CHECK: [[TTTF:%.*]] = thin_to_thick_function [[FR]]
// CHECK: [[DF:%.*]] = differentiable_function [parameters 0] [results 0] [[TTTF]]
// CHECK: [[NEDF:%.*]] = convert_escape_to_noescape [[DF]]
// CHECK: [[BNEDF:%.*]] = begin_borrow [[NEDF]]

// CHECK: [[DFEO:%.*]] = differentiable_function_extract [original] [[BNEDF]]
// CHECK: [[CDFEO:%.*]] = copy_value [[DFEO]]
// CHECK: [[THUNK1:%.*]] = function_ref @$sS2fIgyd_S2fIegnr_TR
// CHECK: [[THUNKED1:%.*]] = partial_apply [callee_guaranteed] [[THUNK1]]([[CDFEO]])
// CHECK: [[CVT1:%.*]] = convert_function [[THUNKED1]]
// CHECK-NOT: copy_value [[CVT1]]
// CHECK: [[CVT2:%.*]] = convert_escape_to_noescape [[CVT1]]

// CHECK: [[DFEJ:%.*]] = differentiable_function_extract [jvp] [[BNEDF]]
// CHECK: [[CDFEJ:%.*]] = copy_value [[DFEJ]]
// CHECK: [[THUNK2:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED2:%.*]] = partial_apply [callee_guaranteed] [[THUNK2]]([[CDFEJ]])
// CHECK: [[CVT3:%.*]] = convert_function [[THUNKED2]]
// CHECK-NOT: copy_value [[CVT3]]
// CHECK: [[CVT4:%.*]] = convert_escape_to_noescape [[CVT3]]

// CHECK: [[DFEV:%.*]] = differentiable_function_extract [vjp] [[BNEDF]]
// CHECK: [[CDFEV:%.*]] = copy_value [[DFEV]]
// CHECK: [[THUNK3:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED3:%.*]] = partial_apply [callee_guaranteed] [[THUNK3]]([[CDFEV]])
// CHECK: [[CVT5:%.*]] = convert_function [[THUNKED3]]
// CHECK-NOT: copy_value [[CVT5]]
// CHECK: [[CVT6:%.*]] = convert_escape_to_noescape [[CVT5]]


// Lifetimes of differentiable functions NOT derived from `thin_to_thick_function` 
// closures are fixed up by the closure lifetime fixup pass.
@_silgen_name("mustBeFixedUp")
func h(x: Float) -> Float {
  return gradient(at: Float(x), of: { y in x*y })
}

// CHECK-LABEL: sil hidden [ossa] @mustBeFixedUp : $@convention(thin) (Float) -> Float {
// CHECK: bb0
// CHECK: [[FR:%.*]] = function_ref @$s22closure_lifetime_fixup1h1xS2f_tFS2fcfU_
// CHECK: [[PAI:%.*]] = partial_apply [callee_guaranteed] [[FR]]
// CHECK: [[CPAI:%.*]] = copy_value [[PAI]]
// CHECK: [[DF:%.*]] = differentiable_function [parameters 0] [results 0] [[CPAI]]
// CHECK: [[CDF:%.*]] = copy_value [[DF]]
// CHECK: [[NEDF:%.*]] = convert_escape_to_noescape [[CDF]]
// CHECK: [[BNEDF:%.*]] = begin_borrow [[NEDF]]

// CHECK: [[DFEO:%.*]] = differentiable_function_extract [original] [[BNEDF]]
// CHECK: [[CDFEO:%.*]] = copy_value [[DFEO]]
// CHECK: [[THUNK1:%.*]] = function_ref @$sS2fIgyd_S2fIegnr_TR
// CHECK: [[THUNKED1:%.*]] = partial_apply [callee_guaranteed] [[THUNK1]]([[CDFEO]])
// CHECK: [[CVT1:%.*]] = convert_function [[THUNKED1]]
// CHECK: [[CCVT1:%.*]] = copy_value [[CVT1]]
// CHECK: [[CVT2:%.*]] = convert_escape_to_noescape [[CCVT1]]

// CHECK: [[DFEJ:%.*]] = differentiable_function_extract [jvp] [[BNEDF]]
// CHECK: [[CDFEJ:%.*]] = copy_value [[DFEJ]]
// CHECK: [[THUNK2:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED2:%.*]] = partial_apply [callee_guaranteed] [[THUNK2]]([[CDFEJ]])
// CHECK: [[CVT3:%.*]] = convert_function [[THUNKED2]]
// CHECK: [[CCVT3:%.*]] = copy_value [[CVT3]]
// CHECK: [[CVT4:%.*]] = convert_escape_to_noescape [[CCVT3]]

// CHECK: [[DFEV:%.*]] = differentiable_function_extract [vjp] [[BNEDF]]
// CHECK: [[CDFEV:%.*]] = copy_value [[DFEV]]
// CHECK: [[THUNK3:%.*]] = function_ref @$sS4fIegyd_Igydo_S2fxq_Ri_zRi0_zRi__Ri0__r0_lyS2fIsegnr_Iegnro_TR
// CHECK: [[THUNKED3:%.*]] = partial_apply [callee_guaranteed] [[THUNK3]]([[CDFEV]])
// CHECK: [[CVT5:%.*]] = convert_function [[THUNKED3]]
// CHECK: [[CCVT5:%.*]] = copy_value [[CVT5]]
// CHECK: [[CVT6:%.*]] = convert_escape_to_noescape [[CCVT5]]

// CHECK: destroy_value [[CDF]]
// CHECK: destroy_value [[CCVT1]]
// CHECK: destroy_value [[CCVT3]]
// CHECK: destroy_value [[CCVT5]]

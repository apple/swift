// RUN: %target-swift-frontend -emit-sil -O %s | %FileCheck %s
// REQUIRES: swift_in_compiler

// Checks for inlining depends on code-size but cow check adds some
// amount of extra code
// UNSUPPORTED: array_cow_checks

import _Differentiation

@_silgen_name("blackHole")
@inline(never)
@discardableResult
func blackHole<T>(_ x: T) -> T { x }

func float(_ x0: Float) -> Float {
  let x1 = x0 * x0
  let x2 = x1 + x1
  let x3 = x2 - x1
  let x4 = x3 / x2
  return x4
}

@_silgen_name("test_gradient_float")
func test_gradient_float() {
  blackHole(gradient(at: 10, of: float))
}

// Check that `apply`s are fully inlined.
// CHECK-LABEL: sil hidden @test_gradient_float : $@convention(thin) () -> ()
// CHECK-NOT: apply
// CHECK: [[GRADIENT_RESULT:%.*]] = struct $Float ({{.*}} : $Builtin.FPIEEE32)
// CHECK: [[FN_REF:%.*]] = function_ref @$s9blackHoleSf_Tg5 : $@convention(thin) (Float) -> Float
// CHECK-NEXT: apply [[FN_REF:%.*]]([[GRADIENT_RESULT]])
// CHECK-NOT: apply
// CHECK-LABEL: } // end sil function 'test_gradient_float'
func float_mutation(_ x: Float) -> Float {
  var result = x * x
  result = result + result
  result = result - x
  result = result / x
  return result
}

@_silgen_name("test_gradient_float_mutation")
func test_gradient_float_mutation() {
  blackHole(gradient(at: 10, of: float_mutation))
}

// Check that `apply`s are fully inlined.
// CHECK-LABEL: sil hidden @test_gradient_float_mutation : $@convention(thin) () -> ()
// CHECK-NOT: apply
// CHECK: [[GRADIENT_RESULT:%.*]] = struct $Float ({{.*}} : $Builtin.FPIEEE32)
// CHECK: [[FN_REF:%.*]] = function_ref @$s9blackHoleSf_Tg5 : $@convention(thin) (Float) -> Float
// CHECK-NEXT: apply [[FN_REF:%.*]]([[GRADIENT_RESULT]])
// CHECK-NOT: apply
// CHECK-LABEL: } // end sil function 'test_gradient_float_mutation'
func float_conditional(_ x: Float, _ bool: Bool) -> Float {
  var result = x * x
  if bool {
    result = result + result
    result = result - x
  } else {
    result = result / x
  }
  return result
}

@_silgen_name("test_gradient_float_conditional")
func test_gradient_float_conditional() {
  blackHole(gradient(at: 10, of: { float_conditional($0, true) }))
}

// Check that `apply`s are fully inlined.
// CHECK-LABEL: sil hidden @test_gradient_float_conditional : $@convention(thin) () -> ()
// CHECK-NOT: apply
// CHECK: [[GRADIENT_RESULT:%.*]] = struct $Float ({{.*}} : $Builtin.FPIEEE32)
// CHECK: [[FN_REF:%.*]] = function_ref @$s9blackHoleSf_Tg5 : $@convention(thin) (Float) -> Float
// CHECK-NEXT: apply [[FN_REF:%.*]]([[GRADIENT_RESULT]])
// CHECK-NOT: apply
// CHECK-LABEL: } // end sil function 'test_gradient_float_conditional'
func float_loop(_ x: Float, count: Int) -> Float {
  var result: Float = 0
  for _ in 0..<count {
    result = result * x
  }
  return result
}

@_silgen_name("test_gradient_float_loop")
func test_gradient_float_loop() {
  blackHole(gradient(at: 10, of: { float_loop($0, count: 10) }))
}

// Check whether `apply`s are inlined.
// CHECK-LABEL: sil hidden @test_gradient_float_loop : $@convention(thin) () -> ()
// CHECK:  = function_ref @${{.*24test_gradient_float_loopyyFS2fcfU_TJrSpSr|sSf16_DifferentiationE12_vjpMultiply3lhs3rhsSf5value_Sf_SftSfc8pullbacktSf_SftFZSf_SftSfcfU_}}
// CHECK: [[FN_REF:%.*]] = function_ref @$s9blackHoleSf_Tg5 : $@convention(thin) (Float) -> Float
// CHECK-NEXT: apply [[FN_REF:%.*]]
// CHECK-NOT: apply
// CHECK-LABEL: } // end sil function 'test_gradient_float_loop'
func array_loop(_ array: [Float]) -> Float {
  var result: Float = 0
  for i in withoutDerivative(at: array.indices) {
    result += array[i]
  }
  return result
}

@_silgen_name("test_gradient_array_loop")
func test_gradient_array_loop() {
  blackHole(gradient(at: [3, 4, 5], of: array_loop))
}

// Check whether `apply`s are inlined.
// Currently, the VJP is not inlined.
// CHECK-LABEL: sil hidden @test_gradient_array_loop : $@convention(thin) () -> ()
// CHECK: [[VJP_FN_REF:%.*]] = function_ref @{{.*}}10array_loopySfSaySfGFTJrSpSr : $@convention(thin) (@guaranteed Array<Float>) -> (Float, @owned @callee_guaranteed (Float) -> @owned Array<Float>.DifferentiableView)
// CHECK: [[VJP_RESULT:%.*]] = apply [[VJP_FN_REF]]
// CHECK-LABEL: } // end sil function 'test_gradient_array_loop' 


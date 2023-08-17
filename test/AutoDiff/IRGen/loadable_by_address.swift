// RUN: %target-swift-frontend -c -Xllvm -sil-verify-after-pass=loadable-address %s
// RUN: %target-swift-frontend -emit-sil %s | %FileCheck %s -check-prefix=CHECK-SIL
// RUN: %target-swift-frontend -c -Xllvm -sil-print-after=loadable-address %s 2>&1 | %FileCheck %s -check-prefix=CHECK-LBA-SIL
// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// `isLargeLoadableType` depends on the ABI and differs between architectures.
// REQUIRES: CPU=x86_64

// TF-11: Verify that LoadableByAddress works with differentiation-related instructions:
// - `differentiable_function`
// - `differentiable_function_extract`

// TODO: Add tests for `@differentiable(_linear)` functions.

import _Differentiation
import StdlibUnittest

var LBATests = TestSuite("LoadableByAddress")

// `Large` is a large loadable type.
// `Large.TangentVector` is not a large loadable type.
struct Large : Differentiable {
  var a: Float
  var b: Float
  var c: Float
  var d: Float
  @noDerivative let e: Float
}

@_silgen_name("large2large")
@differentiable(reverse)
func large2large(_ foo: Large) -> Large {
  foo
}

// `large2large` old verification error:
// SIL verification failed: JVP type does not match expected JVP type
// $@callee_guaranteed (@in_guaranteed Large) -> (Large, @owned @callee_guaranteed (Large.TangentVector) -> Large.TangentVector)
// $@callee_guaranteed (@in_guaranteed Large) -> (@out Large, @owned @callee_guaranteed (@in_guaranteed Large.TangentVector) -> @out Large.TangentVector)

@_silgen_name("large2small")
@differentiable(reverse)
func large2small(_ foo: Large) -> Float {
  foo.a
}

// `large2small` old verification error:
// SIL verification failed: JVP type does not match expected JVP type
//   $@callee_guaranteed (@in_guaranteed Large) -> (Float, @owned @callee_guaranteed (Large.TangentVector) -> Float)
//   $@callee_guaranteed (@in_guaranteed Large) -> (Float, @owned @callee_guaranteed (@in_guaranteed Large.TangentVector) -> Float)

// CHECK-SIL: sil {{.*}}@large2large : $@convention(thin) (Large) -> Large {
// CHECK-LBA-SIL: sil {{.*}}@large2large : $@convention(thin) (@in_guaranteed Large) -> @out Large {

// CHECK-SIL-LABEL: sil {{.*}}@large2small : $@convention(thin) (Large) -> Float {
// CHECK-LBA-SIL: sil {{.*}}@large2small : $@convention(thin) (@in_guaranteed Large) -> Float {

// CHECK-SIL: sil {{.*}}@large2largeTJfSpSr : $@convention(thin) (Large) -> (Large, @owned @callee_guaranteed (Large.TangentVector) -> Large.TangentVector) {
// CHECK-LBA-SIL: sil {{.*}}@large2largeTJfSpSr : $@convention(thin) (@in_guaranteed Large) -> (Large, @owned @callee_guaranteed (Large.TangentVector) -> Large.TangentVector) {

// CHECK-SIL: sil {{.*}}@large2largeTJrSpSr : $@convention(thin) (Large) -> (Large, @owned @callee_guaranteed (Large.TangentVector) -> Large.TangentVector) {
// CHECK-LBA-SIL: sil {{.*}}@large2largeTJrSpSr : $@convention(thin) (@in_guaranteed Large) -> (Large, @owned @callee_guaranteed (Large.TangentVector) -> Large.TangentVector) {

// CHECK-SIL: sil {{.*}}@large2smallTJfSpSr : $@convention(thin) (Large) -> (Float, @owned @callee_guaranteed (Large.TangentVector) -> Float) {
// CHECK-LBA-SIL: sil {{.*}}@large2smallTJfSpSr : $@convention(thin) (@in_guaranteed Large) -> (Float, @owned @callee_guaranteed (Large.TangentVector) -> Float) {

// CHECK-SIL: sil {{.*}}@large2smallTJrSpSr : $@convention(thin) (Large) -> (Float, @owned @callee_guaranteed (Float) -> Large.TangentVector) {
// CHECK-LBA-SIL: sil {{.*}}@large2smallTJrSpSr : $@convention(thin) (@in_guaranteed Large) -> (Float, @owned @callee_guaranteed (Float) -> Large.TangentVector) {

LBATests.test("Correctness") {
  let one = Large.TangentVector(a: 1, b: 1, c: 1, d: 1)
  expectEqual(one,
              pullback(at: Large(a: 0, b: 0, c: 0, d: 0, e: 0), of: large2large)(one))
  expectEqual(Large.TangentVector(a: 1, b: 0, c: 0, d: 0),
              gradient(at: Large(a: 0, b: 0, c: 0, d: 0, e: 0), of: large2small))
}

runAllTests()

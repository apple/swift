// REQUIRES: PTRSIZE=64
//
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-silgen -primary-file %s -o %t/constant_evaluable_subset_test_arch64_silgen.sil
//
// Run the (mandatory) passes on which constant evaluator depends, and test the
// constant evaluator on the SIL produced after the dependent passes are run.
//
// RUN: not %target-sil-opt -silgen-cleanup -raw-sil-inst-lowering -allocbox-to-stack -mandatory-inlining -constexpr-limit 3000 -test-constant-evaluable-subset %t/constant_evaluable_subset_test_arch64_silgen.sil > /dev/null 2> %t/error-output
//
// RUN: %FileCheck %s < %t/error-output
//
// Test the constant evaluator on the output of the mandatory pipeline. This is
// to test that constant evaluability is not affected by mandatory
// optimizations. Note that it can be affected by non-mandatory optimizations,
// especially performance inlining as it inlines functions such as String.+=
// that the evaluator has special knowledge about.
//
// RUN: not %target-sil-opt -silgen-cleanup -diagnose-invalid-escaping-captures -diagnose-static-exclusivity -capture-promotion -access-enforcement-selection -allocbox-to-stack -noreturn-folding -definite-init -raw-sil-inst-lowering -closure-lifetime-fixup -semantic-arc-opts -destroy-hoisting -ownership-model-eliminator -mandatory-inlining -predictable-memaccess-opts -os-log-optimization -diagnostic-constant-propagation -predictable-deadalloc-elim -mandatory-arc-opts -diagnose-unreachable -diagnose-infinite-recursion -yield-once-check -dataflow-diagnostics -split-non-cond_br-critical-edges -constexpr-limit 3000 -test-constant-evaluable-subset %t/constant_evaluable_subset_test_arch64_silgen.sil > /dev/null 2> %t/error-output-mandatory
//
// RUN: %FileCheck %s < %t/error-output-mandatory

// Test Swift code snippets that are expected to be constant evaluable and those
// that are not. If any of the test here fails, it indicates a change in the
// output of SILGen or the mandatory passes that affects the constant
// evaluability of the corresponding Swift code. The tests here are specific
// to 64bit architecture.

// CHECK-LABEL: @testIntExtensions
// CHECK-NOT: error:
@_semantics("constant_evaluable")
internal func testIntExtensions(a: Int8, b: Int16, c: Int32) -> Int64 {
  return Int64(a) + Int64(b) + Int64(c)
}

@_semantics("test_driver")
internal func interpretIntExtensions() -> Int64 {
  return testIntExtensions(a: 100, b: -130, c: -50000)
}

// CHECK-LABEL: @testUIntExtensions
// CHECK-NOT: error:
@_semantics("constant_evaluable")
internal func testUIntExtensions(a: UInt8, b: UInt16, c: UInt32) -> UInt64 {
  return UInt64(a) + UInt64(b) + UInt64(c)
}

@_semantics("test_driver")
internal func interpretUIntExtensions() -> UInt64 {
  return testUIntExtensions(a: 100, b: 130, c: 0xffffffff)
}

// CHECK-LABEL: @testIntTruncations
// CHECK-NOT: error:
// This is an expensive function to evaluate requiring evaluating approximately
// 2048 instructions with optimized stdlib and 3000 instructions with
// unoptimized stdlib.
@_semantics("constant_evaluable")
internal func testIntTruncations(a: Int64) -> Int16 {
  return Int16(a)
}

@_semantics("test_driver")
internal func interpretIntTruncations() -> Int16 {
  return testIntTruncations(a: 100)
}

// CHECK-LABEL: @testInvalidIntTruncations
// CHECK: error: not constant evaluable
@_semantics("constant_evaluable")
internal func testInvalidIntTruncations(a: Int64) -> Int8 {
  return Int8(a)
    // CHECK: note: {{.*}} Not enough bits to represent the passed value
    // CHECK: note: operation performed during this call traps
    // CHECK: function_ref @$sSZss17FixedWidthIntegerRzrlEyxqd__cSzRd__lufC
}

@_semantics("test_driver")
internal func interpretInvalidIntTruncations() -> Int8 {
  return testInvalidIntTruncations(a: 130)
}

// CHECK-LABEL: @testUIntTruncations
// CHECK-NOT: error:
// This is an expensive function to evaluate requiring evaluating approximately
// 2048 instructions.
@_semantics("constant_evaluable")
internal func testUIntTruncations(a: UInt64) -> UInt16 {
  return UInt16(a)
}

@_semantics("test_driver")
internal func interpretUIntTruncations() -> UInt16 {
  return testUIntTruncations(a: 100)
}

// CHECK-LABEL: @testSingedUnsignedConversions
// CHECK-NOT: error:
// This is an expensive function to evaluate requiring evaluating approximately
// 2048 instructions.
@_semantics("constant_evaluable")
internal func testSingedUnsignedConversions(a: Int64, b: UInt8) -> UInt64 {
  return UInt64(a) + UInt64(Int8(b))
}

@_semantics("test_driver")
internal func interpretSingedUnsignedConversions() -> UInt64 {
  return testSingedUnsignedConversions(a: 100, b: 120)
}

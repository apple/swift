// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// First, check that LBA actually modifies the function, so that this test is useful.

// RUN: %target-swift-frontend -emit-sil %S/Inputs/loadable_by_address_cross_module.swift | %FileCheck %s -check-prefix=CHECK-MODULE-PRE-LBA
// RUN: %target-swift-frontend -c -Xllvm -sil-print-after=loadable-address %S/Inputs/loadable_by_address_cross_module.swift 2>&1 | %FileCheck %s -check-prefix=CHECK-MODULE-POST-LBA

// CHECK-MODULE-PRE-LBA: sil {{.*}}LBAModifiedFunction{{.*}} $@convention(method) <T> (Float, LargeLoadableType<T>) -> Float
// CHECK-MODULE-POST-LBA: sil {{.*}}LBAModifiedFunction{{.*}} $@convention(method) <T> (Float, @in_guaranteed LargeLoadableType<T>) -> Float

// Compile the module.

// RUN: %empty-directory(%t)
// RUN: %target-build-swift-dylib(%t/%target-library-name(external)) %S/Inputs/loadable_by_address_cross_module.swift -emit-module -emit-module-path %t/external.swiftmodule -module-name external

// Next, check that differentiability_witness_functions in the client get
// correctly modified by LBA.

// RUN: %target-swift-frontend -emit-sil -I%t %s
// RUN: %target-swift-frontend -emit-sil -I%t %s | %FileCheck %s -check-prefix=CHECK-CLIENT-PRE-LBA
// RUN: %target-swift-frontend -c -I%t %s -Xllvm -sil-print-after=loadable-address 2>&1 | %FileCheck %s -check-prefix=CHECK-CLIENT-POST-LBA

// CHECK-CLIENT-PRE-LBA: differentiability_witness_function [jvp] [reverse] [parameters 0 1] [results 0] <T> @${{.*}}LBAModifiedFunction{{.*}} : $@convention(method) <τ_0_0> (Float, LargeLoadableType<τ_0_0>) -> Float
// CHECK-CLIENT-PRE-LBA: differentiability_witness_function [vjp] [reverse] [parameters 0 1] [results 0] <T> @${{.*}}LBAModifiedFunction{{.*}} : $@convention(method) <τ_0_0> (Float, LargeLoadableType<τ_0_0>) -> Float

// CHECK-CLIENT-POST-LBA: differentiability_witness_function [jvp] [reverse] [parameters 0 1] [results 0] <T> @$s8external17LargeLoadableTypeV0A19LBAModifiedFunctionyS2fF : $@convention(method) <τ_0_0> (Float, @in_guaranteed LargeLoadableType<τ_0_0>) -> Float as $@convention(method) <τ_0_0> (Float, @in_guaranteed LargeLoadableType<τ_0_0>) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float, τ_0_0) -> Float for <LargeLoadableType<τ_0_0>>)
// CHECK-CLIENT-POST-LBA: differentiability_witness_function [vjp] [reverse] [parameters 0 1] [results 0] <T> @$s8external17LargeLoadableTypeV0A19LBAModifiedFunctionyS2fF : $@convention(method) <τ_0_0> (Float, @in_guaranteed LargeLoadableType<τ_0_0>) -> Float as $@convention(method) <τ_0_0> (Float, @in_guaranteed LargeLoadableType<τ_0_0>) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float) -> (Float, τ_0_0) for <LargeLoadableType<τ_0_0>>)

// Finally, execute the test.

// RUN: %target-build-swift -I%t -L%t %s -o %t/a.out %target-rpath(%t) -L%t -lexternal
// RUN: %target-codesign %t/a.out
// RUN: %target-codesign %t/%target-library-name(external)
// RUN: %target-run %t/a.out %t/%target-library-name(external)

// REQUIRES: executable_test

import _Differentiation
import external
import StdlibUnittest

var LBATests = TestSuite("LoadableByAddressCrossModule")

LBATests.test("Correctness") {
  let g = gradient(at: LargeLoadableType<Int>(a: 5), 10) { $0.externalLBAModifiedFunction($1) }
  expectEqual((LargeLoadableType<Int>(a: 10), 5), g)
}

runAllTests()

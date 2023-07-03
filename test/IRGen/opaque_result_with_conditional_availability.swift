// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.15 -emit-module -emit-module-path=%t/opaque_result_with_conditional_availability_types.swiftmodule %S/Inputs/opaque_result_with_conditional_availability_types.swift
// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 -c -parse-as-library -o %t/opaque_result_with_conditional_availability_types.o %S/Inputs/opaque_result_with_conditional_availability_types.swift
// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.15 -I%t -emit-ir %s -swift-version 5 | %IRGenFileCheck %s
// REQUIRES: OS=macosx

import opaque_result_with_conditional_availability_types

protocol P {
  func hello()
}

@available(macOS 100, *)
struct A : P {
  func hello() { print("Hello from A") }
}

@available(macOS 100.0.2, *)
struct B : P {
  func hello() { print("Hello from B") }
}

struct C : P {
  func hello() { print("Hello from C") }
}

func test_multiple_single() -> some P {
  if #available(macOS 100.0.1, *) {
    return A()
  }

  return C()
}

// CHECK: define private ptr @"get_underlying_type_ref 43opaque_result_with_conditional_availabilityAA20test_multiple_singleQryFQOQr"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:   br label %conditional-0
// CHECK: conditional-0:                               ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  [[COND_1:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 1)
// CHECK-NEXT:  [[IS_AT_LEAST:%.*]] = icmp ne i32 [[COND_1]], 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST]], label %result-0, label %universal
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT: ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1AVMf"
// CHECK: universal:                                   ; preds = %cond-0-0
// CHECK-NEXT:   ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1CVMf"
// CHECK-NEXT: }
// CHECK: define private ptr @"get_underlying_witness 43opaque_result_with_conditional_availabilityAA20test_multiple_singleQryFQOxAA1PHC"(ptr %0)
// CHECK-NEXT:  entry:
// CHECK-NEXT:  br label %conditional-0
// CHECK:  conditional-0:                               ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK:  cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  [[COND_1:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 1)
// CHECK-NEXT:  [[IS_AT_LEAST:%.*]] = icmp ne i32 [[COND_1]], 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST]], label %result-0, label %universal
// CHECK:  result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr @"$s43opaque_result_with_conditional_availability1AVAA1PAAWP"
// CHECK:  universal:                                   ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr @"$s43opaque_result_with_conditional_availability1CVAA1PAAWP"
// CHECK-NEXT: }

func test_multiple_conds() -> some P {
  if #available(macOS 100.0.1, *) {
    return A()
  }

  if #available(macOS 100.0.2, *), #available(macOS 100.1, *) {
    return B()
  }

  return C()
}

// CHECK: define private ptr @"get_underlying_type_ref 43opaque_result_with_conditional_availabilityAA19test_multiple_condsQryFQOQr"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:  br label %conditional-0
// CHECK: conditional-0:                               ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  [[COND_1:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 1)
// CHECK-NEXT:  [[IS_AT_LEAST_100_0_1:%.*]] = icmp ne i32 [[COND_1]], 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_0_1]], label %result-0, label %conditional-1
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT: ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1AVMf"
// CHECK: conditional-1:                               ; preds = %cond-0-0
// CHECK-NEXT:  br label %cond-1-0
// CHECK: cond-1-0:                                         ; preds = %conditional-1
// CHECK-NEXT:  [[COND_2:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 2)
// CHECK-NEXT:  [[IS_AT_LEAST_100_0_2:%.*]] = icmp ne i32 %3, 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_0_2]], label %cond-1-1, label %universal
// CHECK: cond-1-1:                                         ; preds = %cond-1-0
// CHECK-NEXT:  [[COND_3:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 1, i32 0)
// CHECK-NEXT:  [[IS_AT_LEAST_100_1:%.*]] = icmp ne i32 %5, 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_1]], label %result-1, label %universal
// CHECK: result-1:                                         ; preds = %cond-1-1
// CHECK-NEXT:  ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1BVMf"
// CHECK: universal:                                   ; preds = %cond-1-1, %cond-1-0
// CHECK-NEXT:  ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1CVMf"
// CHECK-NEXT: }

// CHECK: define private ptr @"get_underlying_witness 43opaque_result_with_conditional_availabilityAA19test_multiple_condsQryFQOxAA1PHC"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:  br label %conditional-0
// CHECK: conditional-0:                               ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  [[COND_1:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 1)
// CHECK-NEXT:  [[IS_AT_LEAST_100_0_1:%.*]] = icmp ne i32 [[COND_1]], 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_0_1]], label %result-0, label %conditional-1
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT: ret ptr @"$s43opaque_result_with_conditional_availability1AVAA1PAAWP"
// CHECK: conditional-1:                               ; preds = %cond-0-0
// CHECK-NEXT:  br label %cond-1-0
// CHECK: cond-1-0:                                         ; preds = %conditional-1
// CHECK-NEXT:  [[COND_2:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 2)
// CHECK-NEXT:  [[IS_AT_LEAST_100_0_2:%.*]] = icmp ne i32 %3, 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_0_2]], label %cond-1-1, label %universal
// CHECK: cond-1-1:                                         ; preds = %cond-1-0
// CHECK-NEXT:  [[COND_3:%.*]] = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 1, i32 0)
// CHECK-NEXT:  [[IS_AT_LEAST_100_1:%.*]] = icmp ne i32 %5, 0
// CHECK-NEXT:  br i1 [[IS_AT_LEAST_100_1]], label %result-1, label %universal
// CHECK: result-1:                                         ; preds = %cond-1-1
// CHECK-NEXT:  ret ptr @"$s43opaque_result_with_conditional_availability1BVAA1PAAWP"
// CHECK: universal:                                   ; preds = %cond-1-1, %cond-1-0
// CHECK-NEXT:  ret ptr @"$s43opaque_result_with_conditional_availability1CVAA1PAAWP"
// CHECK-NEXT: }

func test_multiple_generic<T: P>(_ t: T) -> some P {
  if #available(macOS 100, *) {
    return t
  }

  return C()
}

// CHECK: define private ptr @"get_underlying_type_ref 43opaque_result_with_conditional_availabilityAA21test_multiple_genericyQrxAA1PRzlFQOQr"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:  %"\CF\84_0_01" = alloca ptr
// CHECK-NEXT:  %"\CF\84_0_0" = load ptr, ptr %0
// CHECK-NEXT:  store ptr %"\CF\84_0_0", ptr %"\CF\84_0_01"
// CHECK-NEXT:  %1 = getelementptr inbounds ptr, ptr %0, i32 1
// CHECK-NEXT:  %"\CF\84_0_0.P" = load ptr, ptr %1
// CHECK-NEXT:  br label %conditional-0
// CHECK: conditional-0:                                    ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  %2 = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 0)
// CHECK-NEXT:  %3 = icmp ne i32 %2, 0
// CHECK-NEXT:  br i1 %3, label %result-0, label %universal
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr %"\CF\84_0_0"
// CHECK: universal:                                        ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr {{.*}} @"$s43opaque_result_with_conditional_availability1CVMf"
// CHECK-NEXT: }

// CHECK: define private ptr @"get_underlying_witness 43opaque_result_with_conditional_availabilityAA21test_multiple_genericyQrxAA1PRzlFQOqd__AaCHC"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:  %"\CF\84_0_01" = alloca ptr, align 8
// CHECK-NEXT:  %"\CF\84_0_0" = load ptr, ptr %0, align 8
// CHECK-NEXT:  store ptr %"\CF\84_0_0", ptr %"\CF\84_0_01", align 8
// CHECK-NEXT:  %1 = getelementptr inbounds ptr, ptr %0, i32 1
// CHECK-NEXT:  %"\CF\84_0_0.P" = load ptr, ptr %1, align 8
// CHECK-NEXT:  br label %conditional-0
// CHECK: conditional-0:                                    ; preds = %entry
// CHECK-NEXT:  br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK-NEXT:  %2 = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 0)
// CHECK-NEXT:  %3 = icmp ne i32 %2, 0
// CHECK-NEXT:  br i1 %3, label %result-0, label %universal
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr %"\CF\84_0_0.P"
// CHECK: universal:                                        ; preds = %cond-0-0
// CHECK-NEXT:  ret ptr @"$s43opaque_result_with_conditional_availability1CVAA1PAAWP"
// CHECK-NEXT: }


// rdar://103179745
struct LocalStruct: SomeProtocol {
  func foo() -> some SomeProtocol {
    return self
  }
}

func test_cross_module() -> some SomeProtocol {
  if #available(macOS 100, *) {
    return PublicStruct(LocalStruct()).modify()
  } else {
    return PublicStruct(LocalStruct())
  }
}

// CHECK: define private ptr @"get_underlying_witness 43opaque_result_with_conditional_availabilityAA17test_cross_moduleQryFQOx0a1_b1_c1_d1_E6_types12SomeProtocolHC"(ptr %0)
// CHECK-NEXT: entry:
// CHECK-NEXT:   %1 = alloca { ptr, ptr }, align 8
// CHECK-NEXT:   br label %conditional-0
// CHECK: conditional-0:                                    ; preds = %entry
// CHECK-NEXT:   br label %cond-0-0
// CHECK: cond-0-0:                                         ; preds = %conditional-0
// CHECK:   %{{.*}} = call i32 @__isPlatformVersionAtLeast(i32 1, i32 100, i32 0, i32 0)
// CHECK:   %{{.*}} = icmp ne i32 %{{.*}}, 0
// CHECK:   br i1 %{{.*}}, label %result-0, label %universal
// CHECK: result-0:                                         ; preds = %cond-0-0
// CHECK-NEXT:   %{{.*}} = call ptr @"$s49opaque_result_with_conditional_availability_types12PublicStructVAcA12SomeProtocolAAWl"()
// CHECK:   ret ptr %{{.*}}
// CHECK: universal:                                        ; preds = %cond-0-0
// CHECK-NEXT:   [[R0:%.*]] = call ptr @"$s49opaque_result_with_conditional_availability_types12PublicStructVAcA12SomeProtocolAAWl"()
// CHECK-NEXT:   ret ptr [[R0]]
// CHECK: }

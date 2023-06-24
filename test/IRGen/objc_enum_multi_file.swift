// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -enable-objc-interop -module-name main -primary-file %s %S/Inputs/objc_enum_multi_file_helper.swift -emit-ir | %FileCheck %s

// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -enable-objc-interop -emit-module %S/Inputs/objc_enum_multi_file_helper.swift -o %t
// RUN: %target-swift-frontend -module-name main -primary-file %s -I %t -DIMPORT -emit-ir | %FileCheck %s

#if IMPORT
import objc_enum_multi_file_helper
#endif

// CHECK-LABEL: define hidden swiftcc i32 @"$s4main6useFoo{{.*}}F"(i32 %0) {{.*}} {
func useFoo(_ x: Foo) -> Int32 {
  // CHECK: switch i32 %0, label %[[DEFAULT:.+]] [
  // CHECK-DAG: i32 1, label %[[CASE_B:.+]]
  // CHECK-DAG: i32 2, label %[[CASE_C:.+]]
  // CHECK-DAG: i32 0, label %[[CASE_A:.+]]
  // CHECK: ]

  switch x {
    // CHECK: [[CASE_B]]:
    // CHECK-NEXT: br label %[[FINAL:.+]]
  case .B:
    return 11

    // CHECK: [[CASE_C]]:
    // CHECK-NEXT: br label %[[FINAL]]
  case .C:
    return 15

    // CHECK: [[CASE_A]]:
    // CHECK-NEXT: br label %[[FINAL]]
  case .A:
    return 10
  }

  // CHECK: [[DEFAULT]]:
  // CHECK: call swiftcc void @"$ss32_diagnoseUnexpectedEnumCaseValue{{.+}}"(ptr @"$s{{.+}}3FooON", ptr noalias nocapture %{{.+}}, ptr @"$ss5Int32VN")
  // CHECK-NEXT: unreachable

  // CHECK: [[FINAL]]:
  // CHECK: %[[RETVAL:.+]] = phi i32 [ 10, %[[CASE_A]] ], [ 15, %[[CASE_C]] ], [ 11, %[[CASE_B]] ]
  // CHECK: ret i32 %[[RETVAL]]
}

// CHECK-LABEL: define hidden swiftcc i32 @"$s4main6useBar{{.*}}F"(i32 %0) {{.*}} {
func useBar(_ x: Bar) -> Int32 {
  // CHECK: switch i32 %0, label %[[DEFAULT:.+]] [
  // CHECK-DAG: i32 6, label %[[CASE_B:.+]]
  // CHECK-DAG: i32 7, label %[[CASE_C:.+]]
  // CHECK-DAG: i32 5, label %[[CASE_A:.+]]
  // CHECK: ]

  switch x {
  // CHECK: [[CASE_B]]:
  // CHECK-NEXT: br label %[[FINAL:.+]]
  case .B:
    return 11

  // CHECK: [[CASE_C]]:
  // CHECK-NEXT: br label %[[FINAL]]
  case .C:
    return 15

  // CHECK: [[CASE_A]]:
  // CHECK-NEXT: br label %[[FINAL]]
  case .A:
    return 10
  }

  // CHECK: [[DEFAULT]]:
  // CHECK: call swiftcc void @"$ss32_diagnoseUnexpectedEnumCaseValue{{.+}}"(ptr @"$s{{.+}}3BarON", ptr noalias nocapture %{{.+}}, ptr @"$ss5Int32VN")
  // CHECK-NEXT: unreachable

  // CHECK: [[FINAL]]:
  // CHECK: %[[RETVAL:.+]] = phi i32 [ 10, %[[CASE_A]] ], [ 15, %[[CASE_C]] ], [ 11, %[[CASE_B]] ]
  // CHECK: ret i32 %[[RETVAL]]
}

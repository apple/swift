// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/weak_import_native_hoist_helper.swiftmodule -parse-as-library %S/Inputs/weak_import_native_hoist_helper.swift -enable-library-evolution
//
// RUN: %target-swift-frontend %use_no_opaque_pointers -disable-type-layout -primary-file %s -I %t -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -disable-type-layout -primary-file %s -I %t -emit-ir

// UNSUPPORTED: OS=windows-msvc

import weak_import_native_hoist_helper

// We should not hoist the metadata accessor across the version check.

// CHECK-LABEL: define {{.*}} void @"$s24weak_import_native_hoist09test_not_D14_weakly_linkedyyF"()
// CHECK-NOT: 15ResilientStructVMa
// CHECK: getVersion
// CHECK: br
// CHECK: 15ResilientStructVMa
// CHECK: ret

public func test_not_hoist_weakly_linked() {
  if getVersion() == 1 {
     var _ = ResilientStruct()
  }
}

// CHECK-LABEL: define {{.*}} void @"$s24weak_import_native_hoist09test_not_D15_weakly_linked2yyF"()
// CHECK-NOT: 15ResilientStructVMa
// CHECK: getVersion
// CHECK: br
// CHECK: 15ResilientStructVMa
// CHECK: ret
public func test_not_hoist_weakly_linked2() {
  if getVersion() == 1 {
     var _ = (ResilientStruct(), 1)
  }
}

struct One<T> {
  var elt : T?
}

// CHECK-LABEL: define {{.*}} void @"$s24weak_import_native_hoist09test_not_D15_weakly_linked3yyF"()
// CHECK-NOT: 15ResilientStructVMa
// CHECK: getVersion
// CHECK: br
// CHECK: 15ResilientStructVMa
// CHECK: ret
public func test_not_hoist_weakly_linked3() {
  if getVersion() == 1 {
     var _ = One(elt:ResilientStruct())
  }
}

// CHECK-LABEL: define {{.*}} void @"$s24weak_import_native_hoist09test_not_D15_weakly_linked4yyF"()
// CHECK-NOT: 15ResilientStructVMa
// CHECK: getVersion
// CHECK: br
// CHECK: 15ResilientStructVMa
// CHECK: ret
public func test_not_hoist_weakly_linked4() {
  if getVersion() == 1 {
     var _ = One(elt:(ResilientStruct(), 1))
  }
}

// CHECK-LABEL: define {{.*}} @"$s24weak_import_native_hoist29test_weakly_linked_enum_cases1eSi0a1_b1_c1_D7_helper1EO_tF
// CHECK:  [[TAG:%.*]] = call i32 %getEnumTag(
// CHECK:  [[STRONG_CASE:%.*]] = load i32, i32* @"$s31weak_import_native_hoist_helper1EO6strongyA2CmFWC"
// CHECK:  [[IS_STRONG:%.*]] = icmp eq i32 [[TAG]], [[STRONG_CASE]]
// CHECK:  br i1 [[IS_STRONG]], label %[[BB0:[0-9]+]], label %[[BB1:[0-9]+]]
//
// CHECK:  [[BB1]]:
// CHECK:  br i1 icmp eq ({{.*}} ptrtoint (i32* @"$s31weak_import_native_hoist_helper1EO0A0yA2CmFWC" to {{.*}}), {{.*}} 0), label %[[BB2:[0-9]+]], label %[[BB3:[0-9]+]]
//
// CHECK:  [[BB3]]:
// CHECK:  [[WEAK_CASE:%.*]] = load i32, i32* @"$s31weak_import_native_hoist_helper1EO0A0yA2CmFWC"
// CHECK:  [[IS_WEAK:%.*]] = icmp eq i32 [[TAG]], [[WEAK_CASE]]
// CHECK:  br label %[[BB2]]
//
// CHECK:  [[BB2]]:
// CHECK:  = phi i1 [ false, %[[BB1]] ], [ [[IS_WEAK]], %[[BB3]] ]
public func test_weakly_linked_enum_cases(e: E) -> Int {
  switch e {
    case .strong:
      return 1
    case .weak:
      return 2
    default:
      return 3
  }
}

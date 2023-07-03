// RUN: %target-swift-frontend %s -emit-ir -disable-objc-attr-requires-foundation-module -enable-swift3-objc-inference -swift-version 4 | %FileCheck %s

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// CHECK: [[FILENAME_STR:@.*]] = private unnamed_addr constant {{.*}}.swift\00"

import Foundation

class ObjCSubclass : NSObject {
  func foo() { }
}

// CHECK-LABEL: define internal void @"$s016objc_deprecated_A7_thunks12ObjCSubclassC3fooyyFTo"(ptr %0, ptr %1)
// CHECK: entry:
// CHECK: call void @swift_objc_swift3ImplicitObjCEntrypoint(ptr %0, ptr %1, ptr [[FILENAME_STR]], i64 [[FILENAME_LENGTH:[0-9]+]], i64 11, i64 3, ptr {{.*}})

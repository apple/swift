// RUN: %target-swift-frontend %s %use_no_opaque_pointers -emit-ir -disable-objc-attr-requires-foundation-module -enable-swift3-objc-inference -swift-version 4 | %FileCheck %s
// RUN: %target-swift-frontend %s -emit-ir -disable-objc-attr-requires-foundation-module -enable-swift3-objc-inference -swift-version 4

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// CHECK: [[FILENAME_STR:@.*]] = private unnamed_addr constant {{.*}}.swift\00"

import Foundation

class ObjCSubclass : NSObject {
  func foo() { }
}

// CHECK-LABEL: define internal void @"$s016objc_deprecated_A7_thunks12ObjCSubclassC3fooyyFTo"(%0* %0, i8* %1)
// CHECK: entry:
// CHECK: [[SELF:%[0-9]+]] = bitcast %0* %0 to %objc_object*
// CHECK-NEXT: call void @swift_objc_swift3ImplicitObjCEntrypoint(%objc_object* [[SELF]], i8* %1, i8* getelementptr inbounds ({{.*}}[[FILENAME_STR]]{{.*}}), i64 [[FILENAME_LENGTH:[0-9]+]], i64 12, i64 3, i8* {{.*}})

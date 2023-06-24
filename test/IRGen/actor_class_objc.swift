// RUN: %target-swift-frontend -emit-ir %s -swift-version 5 -target %target-cpu-apple-macosx12.0 | %IRGenFileCheck %s
// REQUIRES: concurrency
// REQUIRES: objc_interop
// REQUIRES: OS=macosx

import Foundation

// CHECK: %T16actor_class_objc7MyClassC = type <{ %swift.refcounted, %swift.defaultactor, %TSi }>
// CHECK: %swift.defaultactor = type { [12 x ptr] }

// CHECK-LABEL: @"OBJC_METACLASS_$__TtC16actor_class_objc7MyClass" = global
//   Metaclass is an instance of the root class.
// CHECK-SAME: ptr {{.*}}@"OBJC_METACLASS_$_SwiftNativeNSObject{{(.ptrauth)?}}"

// CHECK: @"$s16actor_class_objc7MyClassCMf" = internal global
// CHECK-SAME: @"$s16actor_class_objc7MyClassCfD{{(.ptrauth)?}}"
// CHECK-SAME: @"OBJC_METACLASS_$__TtC16actor_class_objc7MyClass{{(.ptrauth)?}}"
// CHECK-SAME: @"OBJC_CLASS_$_SwiftNativeNSObject{{(.ptrauth)?}}"
//   Flags: uses Swift refcounting
// CHECK-SAME: i32 2,
//   Instance size
// CHECK-64-SAME: i32 120,
// CHECK-32-SAME: i32 60,
//   Alignment mask
// CHECK-64-SAME: i16 15,
// CHECK-32-SAME: i16 7,
//   Field offset for 'x'
// CHECK-64-SAME: i64 112,
// CHECK-32-SAME: i32 56,

@objc public actor MyClass {
  public var x: Int
  public init() { self.x = 0 }
}

// CHECK-LABEL: define {{.*}} @"$s16actor_class_objc7MyClassC1xSivg"
// CHECK: [[T0:%.*]] = getelementptr inbounds %T16actor_class_objc7MyClassC, ptr %0, i32 0, i32 2
// CHECK: [[T1:%.*]] = getelementptr inbounds %TSi, ptr [[T0]], i32 0, i32 0
// CHECK: load [[INT]], ptr [[T1]], align

// CHECK-LABEL: define {{.*}}swiftcc ptr @"$s16actor_class_objc7MyClassCACycfc"
// CHECK: swift_defaultActor_initialize
// CHECK-LABEL: ret ptr

// CHECK: swift_defaultActor_destroy

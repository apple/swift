// RUN: %target-swift-frontend -emit-ir %s -swift-version 5  -disable-availability-checking | %IRGenFileCheck %s
// REQUIRES: concurrency


// CHECK: %T11actor_class7MyClassC = type <{ %swift.refcounted, %swift.defaultactor, %TSi }>
// CHECK: %swift.defaultactor = type { [12 x ptr] }

// CHECK-objc-LABEL: @"$s11actor_class7MyClassCMm" = global
// CHECK-objc-SAME: @"OBJC_METACLASS_$__TtCs12_SwiftObject{{(.ptrauth)?}}"

// CHECK: @"$s11actor_class7MyClassCMf" = internal global
// CHECK-SAME: @"$s11actor_class7MyClassCfD{{(.ptrauth)?}}"
// CHECK-objc-SAME: @"OBJC_CLASS_$__TtCs12_SwiftObject{{(.ptrauth)?}}"
// CHECK-nonobjc-SAME: ptr null,
//   Flags: uses Swift refcounting
// CHECK-SAME: i32 2,
//   Instance size
 // CHECK-64-SAME: i32 120,
// CHECK-32-SAME: i32 60,
//   Alignment mask
// CHECK-64-SAME: i16 15,
// CHECK-32-SAME: i16 7,
//   Field offset for 'x'
// CHECK-objc-SAME: [[INT]] {{56|112}},

// Type descriptor.
// CHECK-LABEL: @"$s11actor_class9ExchangerCMn" = {{.*}}constant
//   superclass ref, negative bounds, positive bounds, num immediate members, num fields, field offset vector offset
// CHECK-SAME: i32 0, i32 3, i32 [[#CLASS_METADATA_HEADER+8]], i32 8, i32 2, i32 [[#CLASS_METADATA_HEADER+1]],

// Reflection field records.
// CHECK-LABEL: @"$s11actor_class9ExchangerCMF" = internal constant
//   Field descriptor kind, field size, num fields,
//   (artificial var, "BD", ...)
// CHECK-SAME: i16 1, i16 12, i32 2, i32 6,
// CHECK-SAME: @"symbolic BD"

public actor MyClass {
  public var x: Int
  public init() { self.x = 0 }
}

// CHECK-LABEL: define {{.*}}@"$s11actor_class7MyClassC1xSivg"
// CHECK: [[T0:%.*]] = getelementptr inbounds %T11actor_class7MyClassC, ptr %0, i32 0, i32 2
// CHECK: [[T1:%.*]] = getelementptr inbounds %TSi, ptr [[T0]], i32 0, i32 0
// CHECK: load [[INT]], ptr [[T1]], align

// CHECK-LABEL: define {{.*}}swiftcc ptr @"$s11actor_class7MyClassCACycfc"
// CHECK: swift_defaultActor_initialize
// CHECK-LABEL: ret ptr

// CHECK-LABEL: define {{.*}}swiftcc ptr @"$s11actor_class7MyClassCfd"
// CHECK: swift_defaultActor_destroy
// CHECK-LABEL: ret

public actor Exchanger<T> {
  public var value: T

  public init(value: T) { self.value = value }
  public func exchange(newValue: T) -> T {
    let oldValue = value
    value = newValue
    return oldValue
  }
}
// CHECK-LABEL: define{{.*}} void @"$s11actor_class9ExchangerC5valuexvg"(
//   Note that this is one more than the field offset vector offset from
//   the class descriptor, since this is the second field.
// CHECK:         [[T0:%.*]] = getelementptr inbounds [[INT]], ptr {{.*}}, [[INT]] [[#CLASS_METADATA_HEADER+2]]
// CHECK-NEXT:    [[OFFSET:%.*]] = load [[INT]], ptr [[T0]], align
// CHECK-NEXT:    getelementptr inbounds i8, ptr %1, [[INT]] [[OFFSET]]

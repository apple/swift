// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir -o - -primary-file %s | %FileCheck %s --check-prefix=CHECK

// REQUIRES: CPU=armv7k || CPU=arm64_32

// CHECK: %swift.type = type { [[INT:i32|i64]] }

import Foundation

public class FixedLayoutObjCSubclass : NSObject {
  // This field could use constant direct access because NSObject has
  // fixed layout, but we don't allow that right now.
  public final var field: Int32 = 0
};

// CHECK-LABEL: define hidden swiftcc void @"$s28class_resilience_objc_armv7k29testConstantDirectFieldAccessyyAA23FixedLayoutObjCSubclassCF"(ptr %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s28class_resilience_objc_armv7k23FixedLayoutObjCSubclassC5fields5Int32VvpWvd"
// CHECK-NEXT: [[ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[OFFSET]]
// CHECK:      call void @swift_beginAccess
// CHECK-NEXT: [[PAYLOAD_ADDR:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[ADDR]], i32 0, i32 0
// CHECK-NEXT: store i32 10, ptr [[PAYLOAD_ADDR]]

func testConstantDirectFieldAccess(_ o: FixedLayoutObjCSubclass) {
  o.field = 10
}

public class NonFixedLayoutObjCSubclass : NSCoder {
  // This field uses non-constant direct access because NSCoder has resilient
  // layout.
  public final var field: Int32 = 0
}

// CHECK-LABEL: define hidden swiftcc void @"$s28class_resilience_objc_armv7k32testNonConstantDirectFieldAccessyyAA0F23FixedLayoutObjCSubclassCF"(ptr %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s28class_resilience_objc_armv7k26NonFixedLayoutObjCSubclassC5fields5Int32VvpWvd"
// CHECK-NEXT: [[ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[OFFSET]]
// CHECK:      call void @swift_beginAccess
// CHECK-NEXT: [[PAYLOAD_ADDR:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[ADDR]], i32 0, i32 0
// CHECK-NEXT: store i32 10, ptr [[PAYLOAD_ADDR]]

func testNonConstantDirectFieldAccess(_ o: NonFixedLayoutObjCSubclass) {
  o.field = 10
}

public class GenericObjCSubclass<T> : NSCoder {
  public final var content: T
  public final var field: Int32 = 0

  public init(content: T) {
    self.content = content
  }
}

// CHECK-LABEL: define hidden swiftcc void @"$s28class_resilience_objc_armv7k31testConstantIndirectFieldAccessyyAA19GenericObjCSubclassCyxGlF"(ptr %0)

// FIXME: we could eliminate the unnecessary isa load by lazily emitting
// metadata sources in EmitPolymorphicParameters

// CHECK:      call ptr @object_getClass
// CHECK: [[KLASS:%.*]] = call ptr @object_getClass(ptr %0)
// CHECK-NEXT: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds [[INT]], ptr [[KLASS]], [[INT]] 15
// CHECK-NEXT: [[FIELD_OFFSET:%.*]] = load [[INT]], ptr [[FIELD_OFFSET_ADDR:%.*]]
// CHECK-NEXT: [[ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[FIELD_OFFSET]]
// CHECK:      call void @swift_beginAccess
// CHECK-NEXT: [[PAYLOAD_ADDR:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[ADDR]], i32 0, i32 0
// CHECK-NEXT: store i32 10, ptr [[PAYLOAD_ADDR]]

func testConstantIndirectFieldAccess<T>(_ o: GenericObjCSubclass<T>) {
  // This field uses constant indirect access because NSCoder has resilient
  // layout. Non-constant indirect is never needed for Objective-C classes
  // because the field offset vector only contains Swift field offsets.
  o.field = 10
}

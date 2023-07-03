// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_objc_class.swiftmodule -module-name=resilient_objc_class %S/../Inputs/resilient_objc_class.swift -I %t
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t -enable-library-evolution -emit-ir -o - -primary-file %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize -DINT=i%target-ptrsize

//   This is XFAILed on these targets because they're 32-bit but support tagged pointers.
//   The test is cloned as class_resilience_objc_armv7k.swift for them.
// XFAIL: CPU=armv7k
// XFAIL: CPU=arm64_32
// REQUIRES: objc_interop

// rdar://77399307
// UNSUPPORTED: CPU=arm64, CPU=arm64e

import Foundation
import resilient_struct
import resilient_objc_class

// Note that these are all mutable to allow for the runtime to slide them.
// CHECK: @"$s21class_resilience_objc27ClassWithEmptyThenResilientC9resilient0I7_struct0H3IntVvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc27ClassWithResilientThenEmptyC9resilient0I7_struct0F3IntVvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc34AnotherClassWithEmptyThenResilientC9resilient0J7_struct0I3IntVvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc34AnotherClassWithResilientThenEmptyC9resilient0J7_struct0G3IntVvpWvd" = hidden global [[INT]] 0,

// CHECK: @"$s21class_resilience_objc27ClassWithEmptyThenResilientC5emptyAA0F0VvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc27ClassWithResilientThenEmptyC5emptyAA0H0VvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc34AnotherClassWithEmptyThenResilientC5emptyAA0G0VvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s21class_resilience_objc34AnotherClassWithResilientThenEmptyC5emptyAA0I0VvpWvd" = hidden global [[INT]] 0,

public class FixedLayoutObjCSubclass : NSObject {
  // This field could use constant direct access because NSObject has
  // fixed layout, but we don't allow that right now.
  public final var field: Int32 = 0
};

// CHECK-LABEL: define hidden swiftcc void @"$s21class_resilience_objc29testConstantDirectFieldAccessyyAA23FixedLayoutObjCSubclassCF"(ptr %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s21class_resilience_objc23FixedLayoutObjCSubclassC5fields5Int32VvpWvd"
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

// CHECK-LABEL: define hidden swiftcc void @"$s21class_resilience_objc32testNonConstantDirectFieldAccessyyAA0E23FixedLayoutObjCSubclassCF"(ptr %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s21class_resilience_objc26NonFixedLayoutObjCSubclassC5fields5Int32VvpWvd"
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

// CHECK-LABEL: define hidden swiftcc void @"$s21class_resilience_objc31testConstantIndirectFieldAccessyyAA19GenericObjCSubclassCyxGlF"(ptr %0)

// FIXME: we could eliminate the unnecessary isa load by lazily emitting
// metadata sources in EmitPolymorphicParameters


// CHECK-32: [[ISA:%.*]] = load ptr, ptr %0

// CHECK: inttoptr
// CHECK-64: [[ISA:%.*]] = load [[INT]], ptr %0
// CHECK-64-NEXT: [[ISA_MASK:%.*]] = load [[INT]], ptr @swift_isaMask
// CHECK-64-NEXT: [[ISA_VALUE:%.*]] = and [[INT]] [[ISA]], [[ISA_MASK]]
// CHECK-64-NEXT: [[ISA:%.*]] = inttoptr [[INT]] [[ISA_VALUE]] to ptr

// CHECK-32-NEXT: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds [[INT]], ptr [[ISA]], [[INT]] 15

// CHECK-64-NEXT: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds [[INT]], ptr [[ISA]], [[INT]] 12

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

@frozen
public struct Empty {}

public class ClassWithEmptyThenResilient : DummyClass {
  public let empty: Empty
  public let resilient: ResilientInt

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

public class ClassWithResilientThenEmpty : DummyClass {
  public let resilient: ResilientInt
  public let empty: Empty

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

// Same as the above, but the superclass is resilient, and ultimately inherits
// from an Objective-C base class.

public class AnotherClassWithEmptyThenResilient : ResilientNSObjectOutsideParent {
  public let empty: Empty
  public let resilient: ResilientInt

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

public class AnotherClassWithResilientThenEmpty : ResilientNSObjectOutsideParent {
  public let resilient: ResilientInt
  public let empty: Empty

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

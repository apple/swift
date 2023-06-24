// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/class_resilience.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_enum.swiftmodule -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_class.swiftmodule -module-name=resilient_class -I %t %S/../Inputs/resilient_class.swift
// RUN: %target-swift-frontend -enable-objc-interop -I %t -emit-ir -enable-library-evolution %t/class_resilience.swift | %FileCheck %t/class_resilience.swift --check-prefixes=CHECK,CHECK-objc,CHECK-objc%target-ptrsize,CHECK-%target-ptrsize,CHECK-%target-cpu,CHECK-%target-import-type-objc-STABLE-ABI-%target-mandates-stable-abi,CHECK-%target-sdk-name -DINT=i%target-ptrsize -D#MDWORDS=7 -D#MDSIZE32=52 -D#MDSIZE64=80 -D#WORDSIZE=%target-alignment
// RUN: %target-swift-frontend -disable-objc-interop -I %t -emit-ir -enable-library-evolution %t/class_resilience.swift | %FileCheck %t/class_resilience.swift --check-prefixes=CHECK,CHECK-native,CHECK-native%target-ptrsize,CHECK-%target-ptrsize,CHECK-%target-cpu,CHECK-native-STABLE-ABI-%target-mandates-stable-abi,CHECK-%target-sdk-name -DINT=i%target-ptrsize -D#MDWORDS=4 -D#MDSIZE32=40 -D#MDSIZE64=56 -D#WORDSIZE=%target-alignment
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %t/class_resilience.swift

// CHECK: @"$s16class_resilience26ClassWithResilientPropertyC1s16resilient_struct4SizeVvpWvd" = hidden global [[INT]] 0
// CHECK: @"$s16class_resilience26ClassWithResilientPropertyC5colors5Int32VvpWvd" = hidden global [[INT]] 0

// CHECK: @"$s16class_resilience33ClassWithResilientlySizedPropertyC1r16resilient_struct9RectangleVvpWvd" = hidden global [[INT]] 0
// CHECK: @"$s16class_resilience33ClassWithResilientlySizedPropertyC5colors5Int32VvpWvd" = hidden global [[INT]] 0

// CHECK: @"$s16class_resilience14ResilientChildC5fields5Int32VvpWvd" = hidden global [[INT]] {{8|16}}

// CHECK: @"$s16class_resilience21ResilientGenericChildCMo" = {{(protected )?}}{{(dllexport )?}}global [[BOUNDS:{ (i32|i64), i32, i32 }]] zeroinitializer

// CHECK: @"$s16class_resilience27ClassWithEmptyThenResilientC9resilient0H7_struct0G3IntVvpWvd" = hidden global [[INT]] 0,
// CHECK: @"$s16class_resilience27ClassWithResilientThenEmptyC9resilient0H7_struct0E3IntVvpWvd" = hidden global [[INT]] 0,

// CHECK: @"$s16class_resilience26ClassWithResilientPropertyCMo" = {{(protected )?}}{{(dllexport )?}}constant [[BOUNDS]]
// CHECK-32-SAME: { [[INT]] [[#MDSIZE32]], i32 3, i32 [[#MDWORDS + 6 + 4]] }
// CHECK-64-SAME: { [[INT]] [[#MDSIZE64]], i32 3, i32 [[#MDWORDS + 3 + 4]] }

// CHECK: @"$s16class_resilience28ClassWithMyResilientPropertyC1rAA0eF6StructVvpWvd" = hidden constant [[INT]] {{8|16}}
// CHECK: @"$s16class_resilience28ClassWithMyResilientPropertyC5colors5Int32VvpWvd" = hidden constant [[INT]] {{12|20}}

// CHECK: @"$s16class_resilience30ClassWithIndirectResilientEnumC1s14resilient_enum10FunnyShapeOvpWvd" = hidden constant [[INT]] {{8|16}}
// CHECK: @"$s16class_resilience30ClassWithIndirectResilientEnumC5colors5Int32VvpWvd" = hidden constant [[INT]] {{12|24}}

// CHECK: [[RESILIENTCHILD_NAME:@.*]] = private constant [15 x i8] c"ResilientChild\00"

// CHECK: @"$s16class_resilience14ResilientChildCMo" = {{(protected )?}}{{(dllexport )?}}global [[BOUNDS]] zeroinitializer

// CHECK-macosx: @"$s15resilient_class22ResilientOutsideParentC8getValueSiyFTq" = external{{( dllimport)?}} global %swift.method_descriptor
// CHECK-iphoneos: @"$s15resilient_class22ResilientOutsideParentC8getValueSiyFTq" = external{{( dllimport)?}} global %swift.method_descriptor
// CHECK-watchos: @"$s15resilient_class22ResilientOutsideParentC8getValueSiyFTq" = external{{( dllimport)?}} global %swift.method_descriptor
// CHECK-tvos: @"$s15resilient_class22ResilientOutsideParentC8getValueSiyFTq" = external{{( dllimport)?}} global %swift.method_descriptor

// CHECK: @"$s16class_resilience14ResilientChildCMn" = {{(protected )?}}{{(dllexport )?}}constant <{{.*}}> <{
// --       flags: class, unique, has vtable, has override table, in-place initialization, has resilient superclass
// CHECK-SAME:   <i32 0xE201_0050>
// --       parent:
// CHECK-SAME:   @"$s16class_resilienceMXM"
// --       name:
// CHECK-SAME:   ptr [[RESILIENTCHILD_NAME]]
// --       metadata accessor function:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMa"
// --       field descriptor:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMF"
// -- metadata bounds:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMo"
// --       metadata positive size in words (not used):
// CHECK-SAME:   i32 0,
// --       num immediate members:
// CHECK-SAME:   i32 4,
// --       num fields:
// CHECK-SAME:   i32 1,
// --       field offset vector offset:
// CHECK-SAME:   i32 0,
// -- superclass:
// CHECK-SAME:   @"{{got.|\\01__imp__?}}$s15resilient_class22ResilientOutsideParentCMn"
// --       singleton metadata initialization cache:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMl"
// --       resilient pattern:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMP"
// --       completion function:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCMr"
// --       number of method overrides:
// CHECK-SAME:   i32 2,
// CHECK-SAME:   %swift.method_override_descriptor {
// --       base class:
// CHECK-SAME:   @"{{got.|\\01__imp__?}}$s15resilient_class22ResilientOutsideParentCMn"
// --       base method:
// CHECK-SAME:   @"{{got.|\\01__imp__?}}$s15resilient_class22ResilientOutsideParentC8getValueSiyFTq"
// --       implementation:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildC8getValueSiyF"
// CHECK-SAME:   }
// CHECK-SAME:   %swift.method_override_descriptor {
// --       base class:
// CHECK-SAME:   @"{{got.|\\01__imp__?}}$s15resilient_class22ResilientOutsideParentCMn"
// --       base method:
// CHECK-SAME:   @"{{got.|\\01__imp__?}}$s15resilient_class22ResilientOutsideParentCACycfCTq"
// --       implementation:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCACycfC"
// CHECK-SAME:   }
// CHECK-SAME: }>

// CHECK: @"$s16class_resilience14ResilientChildCMP" = internal constant <{{.*}}> <{
// --       instantiation function:
// CHECK-SAME:   i32 0,
// --       destructor:
// CHECK-SAME:   @"$s16class_resilience14ResilientChildCfD"
// --       ivar destroyer:
// CHECK-SAME:   i32 0,
// --       flags:
// CHECK-SAME:   i32 2,
// --       RO data:
// CHECK-objc-SAME: @_DATA__TtC16class_resilience14ResilientChild
// CHECK-native-SAME: i32 0,
// --       metaclass:
// CHECK-objc-SAME: @"$s16class_resilience14ResilientChildCMm"
// CHECK-native-SAME: i32 0

// CHECK: @"$s16class_resilience17MyResilientParentCMo" = {{(protected )?}}{{(dllexport )?}}constant [[BOUNDS]]
// CHECK-32-SAME: { [[INT]] [[#MDSIZE32]], i32 3, i32 [[#MDWORDS + 6 + 2]] }
// CHECK-64-SAME: { [[INT]] [[#MDSIZE64]], i32 3, i32 [[#MDWORDS + 3 + 2]] }

// CHECK: @"$s16class_resilience16MyResilientChildCMo" = {{(protected )?}}{{(dllexport )?}}constant [[BOUNDS]]
// CHECK-32-SAME: { [[INT]] [[#MDSIZE32 + WORDSIZE + WORDSIZE]], i32 3, i32 [[#MDWORDS + 6 + 3]] }
// CHECK-64-SAME: { [[INT]] [[#MDSIZE64 + WORDSIZE + WORDSIZE]], i32 3, i32 [[#MDWORDS + 3 + 3]] }

// CHECK: @"$s16class_resilience24MyResilientGenericParentCMo" = {{(protected )?}}{{(dllexport )?}}constant [[BOUNDS]]
// CHECK-32-SAME: { [[INT]] [[#MDSIZE32]], i32 3, i32 [[#MDWORDS + 6 + 3]] }
// CHECK-64-SAME: { [[INT]] [[#MDSIZE64]], i32 3, i32 [[#MDWORDS + 3 + 3]] }

// CHECK: @"$s16class_resilience24MyResilientConcreteChildCMo" = {{(protected )?}}{{(dllexport )?}}constant [[BOUNDS]]
// CHECK-32-SAME: { [[INT]] [[#MDSIZE32 + WORDSIZE + WORDSIZE + WORDSIZE]], i32 3, i32 [[#MDWORDS + 6 + 5]] }
// CHECK-64-SAME: { [[INT]] [[#MDSIZE64 + WORDSIZE + WORDSIZE + WORDSIZE]], i32 3, i32 [[#MDWORDS + 3 + 5]] }

// CHECK: @"$s16class_resilience27ClassWithEmptyThenResilientC5emptyAA0E0VvpWvd" = hidden constant [[INT]] 0,
// CHECK: @"$s16class_resilience27ClassWithResilientThenEmptyC5emptyAA0G0VvpWvd" = hidden constant [[INT]] 0,

// CHECK: @"$s16class_resilience14ResilientChildC5fields5Int32VvgTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience14ResilientChildC5fields5Int32VvsTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience14ResilientChildC5fields5Int32VvMTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds

// CHECK: @"$s16class_resilience21ResilientGenericChildC5fields5Int32VvgTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience21ResilientGenericChildC5fields5Int32VvsTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience21ResilientGenericChildC5fields5Int32VvMTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds

// CHECK: @"$s16class_resilience17MyResilientParentCACycfCTq" = hidden alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience24MyResilientGenericParentC1tACyxGx_tcfCTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds
// CHECK: @"$s16class_resilience24MyResilientConcreteChildC1xACSi_tcfCTq" = {{(protected )?}}{{(dllexport )?}}alias %swift.method_descriptor, getelementptr inbounds

import resilient_class
import resilient_struct
import resilient_enum


// Concrete class with resilient stored property

public class ClassWithResilientProperty {
  public let p: Point
  public let s: Size
  public let color: Int32

  public init(p: Point, s: Size, color: Int32) {
    self.p = p
    self.s = s
    self.color = color
  }
}


// Concrete class with non-fixed size stored property

public class ClassWithResilientlySizedProperty {
  public let r: Rectangle
  public let color: Int32

  public init(r: Rectangle, color: Int32) {
    self.r = r
    self.color = color
  }
}


// Concrete class with resilient stored property that
// is fixed-layout inside this resilience domain

public struct MyResilientStruct {
  public let x: Int32
}

public class ClassWithMyResilientProperty {
  public let r: MyResilientStruct
  public let color: Int32

  public init(r: MyResilientStruct, color: Int32) {
    self.r = r
    self.color = color
  }
}


// Enums with indirect payloads are fixed-size

public class ClassWithIndirectResilientEnum {
  public let s: FunnyShape
  public let color: Int32

  public init(s: FunnyShape, color: Int32) {
    self.s = s
    self.color = color
  }
}


// Superclass is resilient, so the number of fields and their
// offsets is not known at compile time

public class ResilientChild : ResilientOutsideParent {
  public var field: Int32 = 0

  public override func getValue() -> Int {
    return 1
  }
}

// Superclass is resilient, so the number of fields and their
// offsets is not known at compile time

public class ResilientGenericChild<T> : ResilientGenericOutsideParent<T> {
  public var field: Int32 = 0
}


// Superclass is resilient and has a resilient value type payload,
// but everything is in one module


public class MyResilientParent {
  public let s: MyResilientStruct = MyResilientStruct(x: 0)
}

public class MyResilientChild : MyResilientParent {
  public let field: Int32 = 0
}


public class MyResilientGenericParent<T> {
  public let t: T

  public init(t: T) {
    self.t = t
  }
}

public class MyResilientConcreteChild : MyResilientGenericParent<Int> {
  public let x: Int

  public init(x: Int) {
    self.x = x
    super.init(t: x)
  }
}

extension ResilientGenericOutsideParent {
  public func genericExtensionMethod() -> A.Type {
    return A.self
  }
}

// rdar://48031465
// Field offsets for empty fields in resilient classes should be initialized
// to their best-known value and made non-constant if that value might
// disagree with the dynamic value.

@frozen
public struct Empty {}

public class ClassWithEmptyThenResilient {
  public let empty: Empty
  public let resilient: ResilientInt

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

public class ClassWithResilientThenEmpty {
  public let resilient: ResilientInt
  public let empty: Empty

  public init(empty: Empty, resilient: ResilientInt) {
    self.empty = empty
    self.resilient = resilient
  }
}

// ClassWithResilientProperty.color getter

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i32 @"$s16class_resilience26ClassWithResilientPropertyC5colors5Int32Vvg"(ptr swiftself %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s16class_resilience26ClassWithResilientPropertyC5colors5Int32VvpWvd"
// CHECK-NEXT: [[FIELD_ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[OFFSET]]
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[FIELD_ADDR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_VALUE:%.*]] = load i32, ptr [[FIELD_PAYLOAD]]
// CHECK: ret i32 [[FIELD_VALUE]]

// ClassWithResilientlySizedProperty.color getter

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i32 @"$s16class_resilience33ClassWithResilientlySizedPropertyC5colors5Int32Vvg"(ptr swiftself %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyC5colors5Int32VvpWvd"
// CHECK-NEXT: [[FIELD_ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[OFFSET]]
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[FIELD_ADDR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_VALUE:%.*]] = load i32, ptr [[FIELD_PAYLOAD]]
// CHECK:      ret i32 [[FIELD_VALUE]]


// ClassWithIndirectResilientEnum.color getter

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i32 @"$s16class_resilience30ClassWithIndirectResilientEnumC5colors5Int32Vvg"(ptr swiftself %0)
// CHECK:      [[FIELD_PTR:%.*]] = getelementptr inbounds %T16class_resilience30ClassWithIndirectResilientEnumC, ptr %0, i32 0, i32 2
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[FIELD_PTR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_VALUE:%.*]] = load i32, ptr [[FIELD_PAYLOAD]]
// CHECK: ret i32 [[FIELD_VALUE]]


// ResilientChild.field getter

// CHECK-LABEL: define hidden swiftcc i32 @"$s16class_resilience14ResilientChildC5fields5Int32Vvg"(ptr swiftself %0)
// CHECK:      [[OFFSET:%.*]] = load [[INT]], ptr @"$s16class_resilience14ResilientChildC5fields5Int32VvpWvd"
// CHECK-NEXT: [[FIELD_ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[OFFSET]]
// CHECK: call void @swift_beginAccess
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[FIELD_ADDR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_VALUE:%.*]] = load i32, ptr [[FIELD_PAYLOAD]]
// CHECK-NEXT: call void @swift_endAccess
// CHECK: ret i32 [[FIELD_VALUE]]

// ResilientGenericChild.field getter

// CHECK-LABEL: define hidden swiftcc i32 @"$s16class_resilience21ResilientGenericChildC5fields5Int32Vvg"(ptr swiftself %0)

// FIXME: we could eliminate the unnecessary isa load by lazily emitting
// metadata sources in EmitPolymorphicParameters

// CHECK:      load ptr

// CHECK: [[ISA:%.*]] = load ptr, ptr %0
// CHECK-NEXT: [[BASE:%.*]] = load [[INT]], ptr @"$s16class_resilience21ResilientGenericChildCMo"
// CHECK-NEXT: [[METADATA_OFFSET:%.*]] = add [[INT]] [[BASE]], {{4|8}}
// CHECK-NEXT: [[FIELD_OFFSET_TMP:%.*]] = getelementptr inbounds i8, ptr [[ISA]], [[INT]] [[METADATA_OFFSET]]
// CHECK-NEXT: [[FIELD_OFFSET:%.*]] = load [[INT]], ptr [[FIELD_OFFSET_ADDR:%.*]]
// CHECK-NEXT: [[ADDR:%.*]] = getelementptr inbounds i8, ptr %0, [[INT]] [[FIELD_OFFSET]]
// CHECK:      call void @swift_beginAccess
// CHECK-NEXT: [[PAYLOAD_ADDR:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[ADDR]], i32 0, i32 0
// CHECK-NEXT: [[RESULT:%.*]] = load i32, ptr [[PAYLOAD_ADDR]]
// CHECK-NEXT: call void @swift_endAccess
// CHECK:      ret i32 [[RESULT]]


// MyResilientChild.field getter

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i32 @"$s16class_resilience16MyResilientChildC5fields5Int32Vvg"(ptr swiftself %0)
// CHECK:      [[FIELD_ADDR:%.*]] = getelementptr inbounds %T16class_resilience16MyResilientChildC, ptr %0, i32 0, i32 2
// CHECK-NEXT: [[PAYLOAD_ADDR:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[FIELD_ADDR]], i32 0, i32 0
// CHECK-NEXT: [[RESULT:%.*]] = load i32, ptr [[PAYLOAD_ADDR]]
// CHECK:      ret i32 [[RESULT]]


// ResilientGenericOutsideParent.genericExtensionMethod()

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s15resilient_class29ResilientGenericOutsideParentC0B11_resilienceE22genericExtensionMethodxmyF"(ptr swiftself %0) {{.*}} {
// CHECK: [[ISA:%.*]] = load ptr, ptr %0
// CHECK:      [[BASE:%.*]] = load [[INT]], ptr @"$s15resilient_class29ResilientGenericOutsideParentCMo"
// CHECK-NEXT: [[GENERIC_PARAM_OFFSET:%.*]] = add [[INT]] [[BASE]], 0
// CHECK-NEXT: [[GENERIC_PARAM_TMP:%.*]] = getelementptr inbounds i8, ptr [[ISA]], [[INT]] [[GENERIC_PARAM_OFFSET]]
// CHECK-NEXT: [[GENERIC_PARAM:%.*]] = load ptr, ptr [[GENERIC_PARAM_TMP]]
// CHECK:       ret ptr [[GENERIC_PARAM]]


// ClassWithResilientProperty metadata accessor

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s16class_resilience26ClassWithResilientPropertyCMa"(
// CHECK:      [[CACHE:%.*]] = load ptr, ptr @"$s16class_resilience26ClassWithResilientPropertyCMl"
// CHECK-NEXT: [[COND:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT: br i1 [[COND]], label %cacheIsNull, label %cont

// CHECK: cacheIsNull:
// CHECK-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @swift_getSingletonMetadata([[INT]] %0, ptr @"$s16class_resilience26ClassWithResilientPropertyCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK-NEXT: [[STATUS:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 1
// CHECK-NEXT: br label %cont

// CHECK: cont:
// CHECK-NEXT: [[NEW_METADATA:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[METADATA]], %cacheIsNull ]
// CHECK-NEXT: [[NEW_STATUS:%.*]] = phi [[INT]] [ 0, %entry ], [ [[STATUS]], %cacheIsNull ]
// CHECK-NEXT: [[T0:%.*]] = insertvalue %swift.metadata_response undef, ptr [[NEW_METADATA]], 0
// CHECK-NEXT: [[T1:%.*]] = insertvalue %swift.metadata_response [[T0]], [[INT]] [[NEW_STATUS]], 1
// CHECK-NEXT: ret %swift.metadata_response [[T1]]


// ClassWithResilientProperty metadata initialization function

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s16class_resilience26ClassWithResilientPropertyCMr"(ptr %0, ptr %1, ptr %2)
// CHECK: entry:
// CHECK-NEXT: [[FIELDS:%.*]] = alloca [3 x ptr]
// CHECK-objc-NEXT: [[FIELDS_DEST:%.*]] = getelementptr inbounds [[INT]], ptr %0, [[INT]] {{10|13}}
// CHECK-native-NEXT: [[FIELDS_DEST:%.*]] = getelementptr inbounds [[INT]], ptr %0, [[INT]] {{7|10}}
// CHECK-NEXT: call void @llvm.lifetime.start.p0(i64 {{12|24}}, ptr [[FIELDS]])
// CHECK-NEXT: [[FIELDS_PTR:%.*]] = getelementptr inbounds [3 x ptr], ptr [[FIELDS]], i32 0, i32 0

// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 319)
// CHECK-NEXT: [[SIZE_METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[STATUS:%.*]] = extractvalue %swift.metadata_response [[T0]], 1
// CHECK-NEXT: [[RESULT:%.*]] = icmp ule [[INT]] [[STATUS]], 63
// CHECK-NEXT: br i1 [[RESULT]], label %dependency-satisfied, label %metadata-dependencies.cont

// CHECK: dependency-satisfied:

// -- ClassLayoutFlags = 0x100 (HasStaticVTable)
// CHECK-native:               [[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 3, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-DIRECT-objc-STABLE-ABI-TRUE: [[T0:%.*]] = call swiftcc %swift.metadata_response @swift_updateClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 3, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-DIRECT-objc-STABLE-ABI-FALSE:[[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 3, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-INDIRECT-objc-STABLE-ABI-TRUE:[[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 3, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-NEXT: [[INITDEP_METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[INITDEP_STATUS:%.*]] = extractvalue %swift.metadata_response [[T0]], 1
// CHECK-NEXT: [[INITDEP_PRESENT:%.*]] = icmp eq ptr [[INITDEP_METADATA]], null
// CHECK-NEXT: br i1 [[INITDEP_PRESENT]], label %dependency-satisfied1, label %metadata-dependencies.cont

// CHECK: dependency-satisfied1:

// CHECK-native:      [[FIELD_OFFSET:%.*]] = load [[INT]], ptr {{.*}}
// CHECK-native-NEXT: store [[INT]] [[FIELD_OFFSET]], ptr @"$s16class_resilience26ClassWithResilientPropertyC1s16resilient_struct4SizeVvpWvd"

// CHECK-native:      [[FIELD_OFFSET:%.*]] = load [[INT]], ptr {{.*}}
// CHECK-native-NEXT: store [[INT]] [[FIELD_OFFSET]], ptr @"$s16class_resilience26ClassWithResilientPropertyC5colors5Int32VvpWvd"

// CHECK:      br label %metadata-dependencies.cont

// CHECK: metadata-dependencies.cont:

// CHECK-NEXT: [[PENDING_METADATA:%.*]] = phi ptr [ [[SIZE_METADATA]], %entry ], [ [[INITDEP_METADATA]], %dependency-satisfied ], [ null, %dependency-satisfied1 ]
// CHECK-NEXT: [[NEW_STATUS:%.*]] = phi [[INT]] [ 63, %entry ], [ [[INITDEP_STATUS]], %dependency-satisfied ], [ 0, %dependency-satisfied1 ]
// CHECK-NEXT: [[T0:%.*]] = insertvalue %swift.metadata_response undef, ptr [[PENDING_METADATA]], 0
// CHECK-NEXT: [[T1:%.*]] = insertvalue %swift.metadata_response [[T0]], [[INT]] [[NEW_STATUS]], 1
// CHECK-NEXT: ret %swift.metadata_response [[T1]]


// ClassWithResilientProperty method lookup function

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s16class_resilience26ClassWithResilientPropertyCMu"(ptr %0, ptr %1)
// CHECK-NEXT: entry:
// CHECK-NEXT:   [[RESULT:%.*]] = call ptr @swift_lookUpClassMethod(ptr %0, ptr %1, ptr @"$s16class_resilience26ClassWithResilientPropertyCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT:   ret ptr [[RESULT]]
// CHECK-NEXT: }


// ClassWithResilientlySizedProperty metadata accessor

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s16class_resilience33ClassWithResilientlySizedPropertyCMa"(
// CHECK:      [[CACHE:%.*]] = load ptr, ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyCMl"
// CHECK-NEXT: [[COND:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT: br i1 [[COND]], label %cacheIsNull, label %cont

// CHECK: cacheIsNull:
// CHECK-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @swift_getSingletonMetadata([[INT]] %0, ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK-NEXT: [[STATUS:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 1
// CHECK-NEXT: br label %cont

// CHECK: cont:
// CHECK-NEXT: [[NEW_METADATA:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[METADATA]], %cacheIsNull ]
// CHECK-NEXT: [[NEW_STATUS:%.*]] = phi [[INT]] [ 0, %entry ], [ [[STATUS]], %cacheIsNull ]
// CHECK-NEXT: [[T0:%.*]] = insertvalue %swift.metadata_response undef, ptr [[NEW_METADATA]], 0
// CHECK-NEXT: [[T1:%.*]] = insertvalue %swift.metadata_response [[T0]], [[INT]] [[NEW_STATUS]], 1
// CHECK-NEXT: ret %swift.metadata_response [[T1]]



// ClassWithResilientlySizedProperty metadata initialization function

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s16class_resilience33ClassWithResilientlySizedPropertyCMr"(ptr %0, ptr %1, ptr %2)
// CHECK: entry:
// CHECK-NEXT: [[FIELDS:%.*]] = alloca [2 x ptr]
// CHECK-objc-NEXT: [[FIELDS_DEST:%.*]] = getelementptr inbounds [[INT]], ptr %0, [[INT]] {{10|13}}
// CHECK-native-NEXT: [[FIELDS_DEST:%.*]] = getelementptr inbounds [[INT]], ptr %0, [[INT]] {{7|10}}
// CHECK-NEXT: call void @llvm.lifetime.start.p0(i64 {{8|16}}, ptr [[FIELDS]])
// CHECK-NEXT: [[FIELDS_PTR:%.*]] = getelementptr inbounds [2 x ptr], ptr [[FIELDS]], i32 0, i32 0

// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct9RectangleVMa"([[INT]] 319)
// CHECK-NEXT: [[SIZE_METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[STATUS:%.*]] = extractvalue %swift.metadata_response [[T0]], 1
// CHECK-NEXT: [[RESULT:%.*]] = icmp ule [[INT]] [[STATUS]], 63
// CHECK-NEXT: br i1 [[RESULT]], label %dependency-satisfied, label %metadata-dependencies.cont

// CHECK: dependency-satisfied:

// -- ClassLayoutFlags = 0x100 (HasStaticVTable)
// CHECK-native:               [[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 2, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-DIRECT-objc-STABLE-ABI-TRUE: [[T0:%.*]] = call swiftcc %swift.metadata_response @swift_updateClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 2, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-DIRECT-objc-STABLE-ABI-FALSE:[[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 2, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-INDIRECT-objc-STABLE-ABI-TRUE:[[T0:%.*]] = call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, [[INT]] 2, ptr [[FIELDS_PTR]], ptr [[FIELDS_DEST]])
// CHECK-NEXT: [[INITDEP_METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[INITDEP_STATUS:%.*]] = extractvalue %swift.metadata_response [[T0]], 1
// CHECK-NEXT: [[INITDEP_PRESENT:%.*]] = icmp eq ptr [[INITDEP_METADATA]], null
// CHECK-NEXT: br i1 [[INITDEP_PRESENT]], label %dependency-satisfied1, label %metadata-dependencies.cont

// CHECK: dependency-satisfied1:

// CHECK-native:      [[FIELD_OFFSET:%.*]] = load [[INT]], ptr {{.*}}
// CHECK-native-NEXT: store [[INT]] [[FIELD_OFFSET]], ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyC1r16resilient_struct9RectangleVvpWvd"

// CHECK-native:      [[FIELD_OFFSET:%.*]] = load [[INT]], ptr {{.*}}
// CHECK-native-NEXT: store [[INT]] [[FIELD_OFFSET]], ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyC5colors5Int32VvpWvd"

// CHECK: br label %metadata-dependencies.cont

// CHECK: metadata-dependencies.cont:

// CHECK-NEXT: [[PENDING_METADATA:%.*]] = phi ptr [ [[SIZE_METADATA]], %entry ], [ [[INITDEP_METADATA]], %dependency-satisfied ], [ null, %dependency-satisfied1 ]
// CHECK-NEXT: [[NEW_STATUS:%.*]] = phi [[INT]] [ 63, %entry ], [ [[INITDEP_STATUS]], %dependency-satisfied ], [ 0, %dependency-satisfied1 ]
// CHECK-NEXT: [[T0:%.*]] = insertvalue %swift.metadata_response undef, ptr [[PENDING_METADATA]], 0
// CHECK-NEXT: [[T1:%.*]] = insertvalue %swift.metadata_response [[T0]], [[INT]] [[NEW_STATUS]], 1
// CHECK-NEXT: ret %swift.metadata_response [[T1]]


// ClassWithResilientlySizedProperty method lookup function

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyCMu"(ptr %0, ptr %1)
// CHECK-NEXT: entry:
// CHECK-NEXT:   [[RESULT:%.*]] = call ptr @swift_lookUpClassMethod(ptr %0, ptr %1, ptr @"$s16class_resilience33ClassWithResilientlySizedPropertyCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT:   ret ptr [[RESULT]]
// CHECK-NEXT: }


// ResilientChild metadata initialization function

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s16class_resilience14ResilientChildCMr"(ptr %0, ptr %1, ptr %2)

// Initialize field offset vector...
// CHECK:      call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 0, [[INT]] 1, ptr {{.*}}, ptr {{.*}})

// CHECK: ret %swift.metadata_response


// ResilientChild method lookup function

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s16class_resilience14ResilientChildCMu"(ptr %0, ptr %1)
// CHECK-NEXT: entry:
// CHECK-NEXT:   [[RESULT:%.*]] = call ptr @swift_lookUpClassMethod(ptr %0, ptr %1, ptr @"$s16class_resilience14ResilientChildCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT:   ret ptr [[RESULT]]
// CHECK-NEXT: }

// ResilientChild.field getter dispatch thunk

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i32 @"$s16class_resilience14ResilientChildC5fields5Int32VvgTj"(ptr swiftself %0)
// CHECK: [[ISA:%.*]] = load ptr, ptr %0
// CHECK-NEXT: [[BASE_ADDR:%.*]] = load [[INT]], ptr @"$s16class_resilience14ResilientChildCMo"
// CHECK-NEXT: [[BASE:%.*]] = add [[INT]] [[BASE_ADDR]], {{4|8}}
// CHECK-NEXT: [[VTABLE_OFFSET_TMP:%.*]] = getelementptr inbounds i8, ptr [[ISA]], [[INT]] [[BASE]]
// CHECK-NEXT: [[METHOD:%.*]] = load ptr, ptr [[VTABLE_OFFSET_TMP]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[VTABLE_OFFSET_TMP]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc i32 [[METHOD]](ptr swiftself %0)
// CHECK-NEXT: ret i32 [[RESULT]]

// ResilientChild.field setter dispatch thunk

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience14ResilientChildC5fields5Int32VvsTj"(i32 %0, ptr swiftself %1)
// CHECK: [[ISA:%.*]] = load ptr, ptr %1
// CHECK-NEXT: [[BASE:%.*]] = load [[INT]], ptr @"$s16class_resilience14ResilientChildCMo"
// CHECK-NEXT: [[METADATA_OFFSET:%.*]] = add [[INT]] [[BASE]], {{8|16}}
// CHECK-NEXT: [[VTABLE_OFFSET_TMP:%.*]] = getelementptr inbounds i8, ptr [[ISA]], [[INT]] [[METADATA_OFFSET]]
// CHECK-NEXT: [[METHOD:%.*]] = load ptr, ptr [[VTABLE_OFFSET_TMP]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[VTABLE_OFFSET_TMP]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[METHOD]](i32 %0, ptr swiftself %1)
// CHECK-NEXT: ret void


// ResilientGenericChild metadata initialization function

// CHECK-LABEL: define internal ptr @"$s16class_resilience21ResilientGenericChildCMi"(ptr %0, ptr %1, ptr %2)
// CHECK:              [[METADATA:%.*]] = call ptr @swift_allocateGenericClassMetadata(ptr {{.*}}, ptr %1, ptr %2)
// CHECK:              ret ptr [[METADATA]]

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s16class_resilience21ResilientGenericChildCMr"
// CHECK-SAME:    (ptr [[METADATA:%.*]], ptr %0, ptr %1)

// CHECK:              call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr [[METADATA]], [[INT]] 0,
// CHECK:              ret %swift.metadata_response


// ResilientGenericChild method lookup function

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s16class_resilience21ResilientGenericChildCMu"(ptr %0, ptr %1)
// CHECK-NEXT: entry:
// CHECK-NEXT:   [[RESULT:%.*]] = call ptr @swift_lookUpClassMethod(ptr %0, ptr %1, ptr @"$s16class_resilience21ResilientGenericChildCMn{{(\.ptrauth.*)?}}")
// CHECK-NEXT:   ret ptr [[RESULT]]
// CHECK-NEXT: }


// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_enum.swiftmodule -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name struct_resilience -Xllvm -sil-disable-pass=MandatoryARCOpts -I %t -emit-ir -enable-library-evolution %s | %FileCheck %s
// RUN: %target-swift-frontend -module-name struct_resilience -Xllvm -sil-disable-pass=MandatoryARCOpts -I %t -emit-ir -enable-library-evolution %s
// RUN: %target-swift-frontend -module-name struct_resilience -I %t -emit-ir -enable-library-evolution -O %s

import resilient_struct
import resilient_enum

// CHECK: %TSi = type <{ [[INT:i32|i64]] }>

// CHECK-LABEL: @"$s17struct_resilience26StructWithResilientStorageVMf" = internal global

// Resilient structs from outside our resilience domain are manipulated via
// value witnesses

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s17struct_resilience30functionWithResilientTypesSize_1f010resilient_A00G0VAFn_A2FnXEtF"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture %1, i8* %2, %swift.opaque* %3)

public func functionWithResilientTypesSize(_ s: __owned Size, f: (__owned Size) -> Size) -> Size {
// CHECK: entry:
// CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
// CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
// CHECK: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT]] -1
// CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

// CHECK-NEXT: [[VWT_CAST:%.*]] = bitcast i8** [[VWT]] to %swift.vwtable*
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[VWT_CAST]], i32 0, i32 8
// CHECK: [[WITNESS_FOR_SIZE:%.*]] = load [[INT]], [[INT]]* [[WITNESS_ADDR]]
// CHECK: [[ALLOCA:%.*]] = alloca i8, {{.*}} [[WITNESS_FOR_SIZE]], align 16
// CHECK: [[STRUCT_ADDR:%.*]] = bitcast i8* [[ALLOCA]] to %swift.opaque*

// CHECK: [[WITNESS_PTR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 2
// CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_PTR]]
// CHECK: [[initializeWithCopy:%.*]] = bitcast i8* [[WITNESS]]
// CHECK: [[STRUCT_LOC:%.*]] = call %swift.opaque* [[initializeWithCopy]](%swift.opaque* noalias [[STRUCT_ADDR]], %swift.opaque* noalias %1, %swift.type* [[METADATA]])

// CHECK: [[FN:%.*]] = bitcast i8* %2 to void (%swift.opaque*, %swift.opaque*, %swift.refcounted*)*
// CHECK: [[SELF:%.*]] = bitcast %swift.opaque* %3 to %swift.refcounted*
// CHECK: call swiftcc void [[FN]](%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture [[STRUCT_ADDR]], %swift.refcounted* swiftself [[SELF]])

// CHECK: [[WITNESS_PTR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 1
// CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_PTR]]
// CHECK: [[destroy:%.*]] = bitcast i8* [[WITNESS]] to void (%swift.opaque*, %swift.type*)*
// CHECK: call void [[destroy]](%swift.opaque* noalias %1, %swift.type* [[METADATA]])
// CHECK-NEXT: bitcast
// CHECK-NEXT: call
// CHECK-NEXT: ret void

  return f(s)
}

// CHECK-LABEL: declare{{( dllimport)?}} swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"
// CHECK-SAME:    ([[INT]])

// Rectangle has fixed layout inside its resilience domain, and dynamic
// layout on the outside.
//
// Make sure we use a type metadata accessor function, and load indirect
// field offsets from it.

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s17struct_resilience35functionWithResilientTypesRectangleyy010resilient_A00G0VF"(%T16resilient_struct9RectangleV* noalias nocapture %0)
public func functionWithResilientTypesRectangle(_ r: Rectangle) {
// CHECK: entry:
// CHECK:      [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct9RectangleVMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i32*
// CHECK-NEXT: [[FIELD_OFFSET_PTR:%.*]] = getelementptr inbounds i32, i32* [[METADATA_ADDR]], [[INT]] [[IDX:2|4|6]]
// CHECK-NEXT: [[FIELD_OFFSET:%.*]] = load i32, i32* [[FIELD_OFFSET_PTR]]
// CHECK-NEXT: [[STRUCT_ADDR:%.*]] = bitcast %T16resilient_struct9RectangleV* %0 to i8*
// CHECK-NEXT: [[FIELD_ADDR:%.*]] = getelementptr inbounds i8, i8* [[STRUCT_ADDR]], i32 [[FIELD_OFFSET]]
// CHECK-NEXT: [[FIELD_PTR:%.*]] = bitcast i8* [[FIELD_ADDR]] to %TSi*
// CHECK-NEXT: [[FIELD_PAYLOAD_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[FIELD_PTR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = load [[INT]], [[INT]]* [[FIELD_PAYLOAD_PTR]]

  _ = r.color

// CHECK-NEXT: ret void

}


// Resilient structs from inside our resilience domain are manipulated
// directly.

public struct MySize {
  public let w: Int
  public let h: Int
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s17struct_resilience32functionWithMyResilientTypesSize_1fAA0eH0VAEn_A2EnXEtF"(%T17struct_resilience6MySizeV* noalias nocapture sret({{.*}}) %0, %T17struct_resilience6MySizeV* noalias nocapture dereferenceable({{8|(16)}}) %1, i8* %2, %swift.opaque* %3)
public func functionWithMyResilientTypesSize(_ s: __owned MySize, f: (__owned MySize) -> MySize) -> MySize {

// There's an alloca for debug info?
// CHECK: {{%.*}} = alloca %T17struct_resilience6MySizeV

// CHECK: [[DST:%.*]] = alloca %T17struct_resilience6MySizeV

// CHECK: [[W_ADDR:%.*]] = getelementptr inbounds %T17struct_resilience6MySizeV, %T17struct_resilience6MySizeV* %1, i32 0, i32 0
// CHECK: [[W_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[W_ADDR]], i32 0, i32 0
// CHECK: [[W:%.*]] = load [[INT]], [[INT]]* [[W_PTR]]

// CHECK: [[H_ADDR:%.*]] = getelementptr inbounds %T17struct_resilience6MySizeV, %T17struct_resilience6MySizeV* %1, i32 0, i32 1
// CHECK: [[H_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[H_ADDR]], i32 0, i32 0
// CHECK: [[H:%.*]] = load [[INT]], [[INT]]* [[H_PTR]]

// CHECK: [[DST_ADDR:%.*]] = bitcast %T17struct_resilience6MySizeV* [[DST]] to i8*

// CHECK: call void @llvm.lifetime.start.p0i8({{i32|i64}} {{8|16}}, i8* [[DST_ADDR]])

// CHECK: [[W_ADDR:%.*]] = getelementptr inbounds %T17struct_resilience6MySizeV, %T17struct_resilience6MySizeV* [[DST]], i32 0, i32 0
// CHECK: [[W_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[W_ADDR]], i32 0, i32 0
// CHECK: store [[INT]] [[W]], [[INT]]* [[W_PTR]]

// CHECK: [[H_ADDR:%.*]] = getelementptr inbounds %T17struct_resilience6MySizeV, %T17struct_resilience6MySizeV* [[DST]], i32 0, i32 1
// CHECK: [[H_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[H_ADDR]], i32 0, i32 0
// CHECK: store [[INT]] [[H]], [[INT]]* [[H_PTR]]

// CHECK: [[FN:%.*]] = bitcast i8* %2 to void (%T17struct_resilience6MySizeV*, %T17struct_resilience6MySizeV*, %swift.refcounted*)*
// CHECK: [[CONTEXT:%.*]] = bitcast %swift.opaque* %3 to %swift.refcounted*

// CHECK: call swiftcc void [[FN]](%T17struct_resilience6MySizeV* noalias nocapture sret({{.*}}) %0, %T17struct_resilience6MySizeV* noalias nocapture dereferenceable({{8|16}}) [[DST]], %swift.refcounted* swiftself [[CONTEXT]])
// CHECK: [[DST_ADDR:%.*]] = bitcast %T17struct_resilience6MySizeV* [[DST]] to i8*
// CHECK: call void @llvm.lifetime.end.p0i8({{i32|i64}} {{8|16}}, i8* [[DST_ADDR]])

// CHECK: ret void

  return f(s)
}

// Structs with resilient storage from a different resilience domain require
// runtime metadata instantiation, just like generics.

public struct StructWithResilientStorage {
  public let s: Size
  public let ss: (Size, Size)
  public let n: Int
  public let i: ResilientInt
}

// Make sure we call a function to access metadata of structs with
// resilient layout, and go through the field offset vector in the
// metadata when accessing stored properties.

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc {{i32|i64}} @"$s17struct_resilience26StructWithResilientStorageV1nSivg"(%T17struct_resilience26StructWithResilientStorageV* {{.*}})
// CHECK:      [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s17struct_resilience26StructWithResilientStorageVMa"([[INT]] 0)
// CHECK:      [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i32*
// CHECK-NEXT: [[FIELD_OFFSET_PTR:%.*]] = getelementptr inbounds i32, i32* [[METADATA_ADDR]], [[INT]] [[IDX:2|4|6]]
// CHECK-NEXT: [[FIELD_OFFSET:%.*]] = load i32, i32* [[FIELD_OFFSET_PTR]]
// CHECK-NEXT: [[STRUCT_ADDR:%.*]] = bitcast %T17struct_resilience26StructWithResilientStorageV* %0 to i8*
// CHECK-NEXT: [[FIELD_ADDR:%.*]] = getelementptr inbounds i8, i8* [[STRUCT_ADDR]], i32 [[FIELD_OFFSET]]
// CHECK-NEXT: [[FIELD_PTR:%.*]] = bitcast i8* [[FIELD_ADDR]] to %TSi*
// CHECK-NEXT: [[FIELD_PAYLOAD_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[FIELD_PTR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = load [[INT]], [[INT]]* [[FIELD_PAYLOAD_PTR]]
// CHECK-NEXT: ret [[INT]] [[FIELD_PAYLOAD]]


// Indirect enums with resilient payloads are still fixed-size.

public struct StructWithIndirectResilientEnum {
  public let s: FunnyShape
  public let n: Int
}


// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc {{i32|i64}} @"$s17struct_resilience31StructWithIndirectResilientEnumV1nSivg"(%T17struct_resilience31StructWithIndirectResilientEnumV* {{.*}})
// CHECK:      [[FIELD_PTR:%.*]] = getelementptr inbounds %T17struct_resilience31StructWithIndirectResilientEnumV, %T17struct_resilience31StructWithIndirectResilientEnumV* %0, i32 0, i32 1
// CHECK-NEXT: [[FIELD_PAYLOAD_PTR:%.*]] = getelementptr inbounds %TSi, %TSi* [[FIELD_PTR]], i32 0, i32 0
// CHECK-NEXT: [[FIELD_PAYLOAD:%.*]] = load [[INT]], [[INT]]* [[FIELD_PAYLOAD_PTR]]
// CHECK:      ret [[INT]] [[FIELD_PAYLOAD]]


// Partial application of methods on resilient value types

public struct ResilientStructWithMethod {
  public func method() {}
}

// Corner case -- type is address-only in SIL, but empty in IRGen

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s17struct_resilience29partialApplyOfResilientMethod1ryAA0f10StructWithG0V_tF"(%T17struct_resilience25ResilientStructWithMethodV* noalias nocapture %0)
public func partialApplyOfResilientMethod(r: ResilientStructWithMethod) {
  _ = r.method
}

// Type is address-only in SIL, and resilient in IRGen

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s17struct_resilience29partialApplyOfResilientMethod1sy010resilient_A04SizeV_tF"(%swift.opaque* noalias nocapture %0)
public func partialApplyOfResilientMethod(s: Size) {
  _ = s.method
}

public func wantsAny(_ any: Any) {}

public func resilientAny(s : ResilientWeakRef) {
  wantsAny(s)
}

// CHECK-LABEL: define{{.*}} swiftcc void @"$s17struct_resilience12resilientAny1sy0c1_A016ResilientWeakRefV_tF"(%swift.opaque* noalias nocapture %0)
// CHECK: entry:
// CHECK: [[ANY:%.*]] = alloca %Any
// CHECK: [[META:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct16ResilientWeakRefVMa"([[INT]] 0)
// CHECK: [[META2:%.*]] = extractvalue %swift.metadata_response [[META]], 0
// CHECK: [[TYADDR:%.*]] = getelementptr inbounds %Any, %Any* [[ANY]], i32 0, i32 1
// CHECK: store %swift.type* [[META2]], %swift.type** [[TYADDR]]
// CHECK: [[BITCAST:%.*]] = bitcast %Any* [[ANY]] to %__opaque_existential_type_0*
// CHECK: call %swift.opaque* @__swift_allocate_boxed_opaque_existential_0(%__opaque_existential_type_0* [[BITCAST]])
// CHECK: call swiftcc void @"$s17struct_resilience8wantsAnyyyypF"(%Any* noalias nocapture dereferenceable({{(32|16)}}) [[ANY]])
// CHECK: [[BITCAST:%.*]] = bitcast %Any* [[ANY]] to %__opaque_existential_type_0*
// CHECK: call void @__swift_destroy_boxed_opaque_existential_0(%__opaque_existential_type_0* [[BITCAST]])
// CHECK: ret void

// Make sure that MemoryLayout properties access resilient types' metadata
// instead of hardcoding sizes based on compile-time layouts.

// CHECK-LABEL: define{{.*}} swiftcc {{i32|i64}} @"$s17struct_resilience38memoryLayoutDotSizeWithResilientStructSiyF"()
public func memoryLayoutDotSizeWithResilientStruct() -> Int {
  // CHECK: entry:
  // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
  // CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
  // CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT]] -1
  // CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

  // CHECK-NEXT: [[VWT_CAST:%.*]] = bitcast i8** [[VWT]] to %swift.vwtable*
  // CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[VWT_CAST]], i32 0, i32 8
  // CHECK: [[WITNESS_FOR_SIZE:%.*]] = load [[INT]], [[INT]]* [[WITNESS_ADDR]]

  // CHECK: ret [[INT]] [[WITNESS_FOR_SIZE]]
  return MemoryLayout<Size>.size
}

// CHECK-LABEL: define{{.*}} swiftcc {{i32|i64}} @"$s17struct_resilience40memoryLayoutDotStrideWithResilientStructSiyF"()
public func memoryLayoutDotStrideWithResilientStruct() -> Int {
  // CHECK: entry:
  // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
  // CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
  // CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT]] -1
  // CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

  // CHECK-NEXT: [[VWT_CAST:%.*]] = bitcast i8** [[VWT]] to %swift.vwtable*
  // CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[VWT_CAST]], i32 0, i32 9
  // CHECK: [[WITNESS_FOR_STRIDE:%.*]] = load [[INT]], [[INT]]* [[WITNESS_ADDR]]

  // CHECK: ret [[INT]] [[WITNESS_FOR_STRIDE]]
  return MemoryLayout<Size>.stride
}

// CHECK-LABEL: define{{.*}} swiftcc {{i32|i64}} @"$s17struct_resilience43memoryLayoutDotAlignmentWithResilientStructSiyF"()
public func memoryLayoutDotAlignmentWithResilientStruct() -> Int {
  // CHECK: entry:
  // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
  // CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
  // CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT]] -1
  // CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

  // CHECK-NEXT: [[VWT_CAST:%.*]] = bitcast i8** [[VWT]] to %swift.vwtable*
  // CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[VWT_CAST]], i32 0, i32 10
  // CHECK: [[WITNESS_FOR_FLAGS:%.*]] = load i32, i32* [[WITNESS_ADDR]]

  // Not checked because it only exists on 64-bit: [[EXTENDED_FLAGS:%.*]] = zext i32 [[WITNESS_FOR_FLAGS]] to [[INT]]

  // CHECK: [[ALIGNMENT_MASK:%.*]] = and [[INT]] {{%.*}}, 255
  // CHECK: [[ALIGNMENT:%.*]] = add [[INT]] [[ALIGNMENT_MASK]], 1

  // CHECK: ret [[INT]] [[ALIGNMENT]]
  return MemoryLayout<Size>.alignment
}


// Make sure that MemoryLayout.offset(of:) on a resilient type uses the accessor
// in the key path instead of hardcoding offsets based on compile-time layouts.

// CHECK-LABEL: define{{.*}} swiftcc { {{i32|i64}}, i8 } @"$s17struct_resilience42memoryLayoutDotOffsetOfWithResilientStructSiSgyF"()
public func memoryLayoutDotOffsetOfWithResilientStruct() -> Int? {
  // CHECK-NEXT: entry:
  // CHECK: [[RAW_KEY_PATH:%.*]] = call %swift.refcounted* @swift_getKeyPath
  // CHECK: [[WRITABLE_KEY_PATH:%.*]] = bitcast %swift.refcounted* [[RAW_KEY_PATH]] to %Ts15WritableKeyPathCy16resilient_struct4SizeVSiG*
  // CHECK: [[PARTIAL_KEY_PATH:%.*]] = bitcast %Ts15WritableKeyPathCy16resilient_struct4SizeVSiG* [[WRITABLE_KEY_PATH]] to %Ts14PartialKeyPathCy16resilient_struct4SizeVG*
  // CHECK: [[ANY_KEY_PATH:%.*]] = bitcast %Ts14PartialKeyPathCy16resilient_struct4SizeVG* [[PARTIAL_KEY_PATH]] to %Ts10AnyKeyPathC*

  // CHECK: [[STORED_INLINE_OFFSET:%.*]] = call swiftcc { [[INT]], i8 } @"$ss10AnyKeyPathC19_storedInlineOffsetSiSgvgTj"(%Ts10AnyKeyPathC* swiftself [[ANY_KEY_PATH]])
  // CHECK: [[VALUE:%.*]] = extractvalue { [[INT]], i8 } [[STORED_INLINE_OFFSET]], 0

  // CHECK: [[RET_PARTIAL:%.*]] = insertvalue { [[INT]], i8 } undef, [[INT]] [[VALUE]], 0
  // CHECK: [[RET:%.*]] = insertvalue { [[INT]], i8 } [[RET_PARTIAL]]
  // CHECK: ret { [[INT]], i8 } [[RET]]
  return MemoryLayout<Size>.offset(of: \Size.w)
}


// Public metadata accessor for our resilient struct

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s17struct_resilience6MySizeVMa"
// CHECK-SAME:    ([[INT]] %0)
// CHECK: ret %swift.metadata_response { %swift.type* bitcast ([[INT]]* getelementptr inbounds {{.*}} @"$s17struct_resilience6MySizeVMf", i32 0, i32 2) to %swift.type*), [[INT]] 0 }


// CHECK-LABEL:  define internal swiftcc %swift.metadata_response @"$s17struct_resilience26StructWithResilientStorageVMr"(%swift.type* %0, i8* %1, i8** %2)
// CHECK: [[FIELDS:%.*]] = alloca [4 x i8**]
// CHECK: [[TUPLE_LAYOUT:%.*]] = alloca %swift.full_type_layout,

// CHECK: [[FIELDS_ADDR:%.*]] = getelementptr inbounds [4 x i8**], [4 x i8**]* [[FIELDS]], i32 0, i32 0

// public let s: Size

// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 319)
// CHECK: [[SIZE_METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK: [[T0:%.*]] = bitcast %swift.type* [[SIZE_METADATA]] to i8***
// CHECK: [[T1:%.*]] = getelementptr inbounds i8**, i8*** [[T0]], [[INT]] -1
// CHECK: [[SIZE_VWT:%.*]] = load i8**, i8*** [[T1]],
// CHECK: [[SIZE_LAYOUT_1:%.*]] = getelementptr inbounds i8*, i8** [[SIZE_VWT]], i32 8
// CHECK: [[FIELD_1:%.*]] = getelementptr inbounds i8**, i8*** [[FIELDS_ADDR]], i32 0
// CHECK: store i8** [[SIZE_LAYOUT_1:%.*]], i8*** [[FIELD_1]]

// public let ss: (Size, Size)

// CHECK: [[SIZE_LAYOUT_2:%.*]] = getelementptr inbounds i8*, i8** [[SIZE_VWT]], i32 8
// CHECK: [[SIZE_LAYOUT_3:%.*]] = getelementptr inbounds i8*, i8** [[SIZE_VWT]], i32 8
// CHECK: call swiftcc [[INT]] @swift_getTupleTypeLayout2(%swift.full_type_layout* [[TUPLE_LAYOUT]], i8** [[SIZE_LAYOUT_2]], i8** [[SIZE_LAYOUT_3]])
// CHECK: [[T0:%.*]] = bitcast %swift.full_type_layout* [[TUPLE_LAYOUT]] to i8**
// CHECK: [[FIELD_2:%.*]] = getelementptr inbounds i8**, i8*** [[FIELDS_ADDR]], i32 1
// CHECK: store i8** [[T0]], i8*** [[FIELD_2]]

// Fixed-layout aggregate -- we can reference a static value witness table
// public let n: Int

// CHECK: [[FIELD_3:%.*]] = getelementptr inbounds i8**, i8*** [[FIELDS_ADDR]], i32 2
// CHECK: store i8** getelementptr inbounds (i8*, i8** @"$sBi{{32|64}}_WV", i32 {{.*}}), i8*** [[FIELD_3]]

// Resilient aggregate with one field -- make sure we don't look inside it
// public let i: ResilientInt
// CHECK: call swiftcc %swift.metadata_response @"$s16resilient_struct12ResilientIntVMa"([[INT]] 319)
// CHECK: [[FIELD_4:%.*]] = getelementptr inbounds i8**, i8*** [[FIELDS_ADDR]], i32 3
// CHECK: store i8** [[SIZE_AND_ALIGNMENT:%.*]], i8*** [[FIELD_4]]

// CHECK: call void @swift_initStructMetadata(%swift.type* {{.*}}, [[INT]] 256, [[INT]] 4, i8*** [[FIELDS_ADDR]], i32* {{.*}})

// coverage for rdar://106669967 where a SIL crash can happen under `-enable-library-evolution -O`
public struct StructWithResilientInit {
  public init() {}
}

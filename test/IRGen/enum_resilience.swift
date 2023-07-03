
// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/enum_resilience.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -disable-type-layout -emit-ir -enable-library-evolution -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift | %FileCheck %t/enum_resilience.swift --check-prefix=ENUM_RES
// RUN: %target-swift-frontend -disable-type-layout -emit-ir -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift | %FileCheck %t/enum_resilience.swift --check-prefix=ENUM_NOT_RES
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_enum.swiftmodule -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift
// RUN: %target-swift-frontend -disable-type-layout -module-name enum_resilience -I %t -emit-ir -enable-library-evolution %s | %FileCheck %t/enum_resilience.swift -DINT=i%target-ptrsize --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize --check-prefix=CHECK-%target-cpu
// RUN: %target-swift-frontend -module-name enum_resilience -I %t -emit-ir -enable-library-evolution -O %s

import resilient_enum
import resilient_struct

// ENUM_RES: @"$s14resilient_enum6MediumO8PamphletyA2CcACmFWC" = {{.*}}constant i32 0
// ENUM_RES: @"$s14resilient_enum6MediumO8PostcardyAC0A7_struct4SizeVcACmFWC" = {{.*}}constant i32 1
// ENUM_RES: @"$s14resilient_enum6MediumO5PaperyA2CmFWC" = {{.*}}constant i32 2
// ENUM_RES: @"$s14resilient_enum6MediumO6CanvasyA2CmFWC" = {{.*}}constant i32 3

// ENUM_NOT_RES-NOT: @"$s14resilient_enum6MediumO8PamphletyA2CcACmFWC" =
// ENUM_NOT_RES-NOT: @"$s14resilient_enum6MediumO8PostcardyAC0A7_struct4SizeVcACmFWC" =
// ENUM_NOT_RES-NOT: @"$s14resilient_enum6MediumO5PaperyA2CmFWC" =
// ENUM_NOT_RES-NOT: @"$s14resilient_enum6MediumO6CanvasyA2CmFWC" =

// CHECK: %T15enum_resilience9ReferenceV = type <{ ptr }>

// Public fixed layout struct contains a public resilient struct,
// cannot use spare bits

// CHECK: %T15enum_resilience6EitherO = type <{ [[REFERENCE_TYPE:\[(4|8) x i8\]]], [1 x i8] }>

// CHECK: @"$s15enum_resilience24EnumWithResilientPayloadOMl" =
// CHECK-SAME: internal global { ptr, ptr } zeroinitializer, align

// CHECK: @"$s15enum_resilience24EnumWithResilientPayloadOMn" = {{.*}}constant
//              0x00010052
//              0x0001      - InPlaceMetadataInitialization
//              0x    0040  - IsUnique
//              0x    0012  - Enum
// CHECK-SAME: <i32 0x0001_0052>,
// CHECK-SAME: @"$s15enum_resilience24EnumWithResilientPayloadOMl"
// CHECK-SAME: @"$s15enum_resilience24EnumWithResilientPayloadOMf", i32 0, i32 2)
// CHECK-SAME: @"$s15enum_resilience24EnumWithResilientPayloadOMr"

public class Class {}

public struct Reference {
  public var n: Class
}

@frozen public enum Either {
  case Left(Reference)
  case Right(Reference)
}

public enum ResilientEither {
  case Left(Reference)
  case Right(Reference)
}

enum InternalEither {
  case Left(Reference)
  case Right(Reference)
}

@frozen public struct ReferenceFast {
  public var n: Class
}

@frozen public enum EitherFast {
  case Left(ReferenceFast)
  case Right(ReferenceFast)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience25functionWithResilientEnumy010resilient_A06MediumOAEF"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1)
public func functionWithResilientEnum(_ m: Medium) -> Medium {

// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s14resilient_enum6MediumOMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], [[INT]] -1
// CHECK-NEXT: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]
// This is copying the +0 argument to be used as a return value.
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK-NEXT: [[WITNESS_FN:%.*]]  = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call ptr [[WITNESS_FN]](ptr noalias %0, ptr noalias %1, ptr [[METADATA]])
// CHECK-NEXT: ret void

  return m
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience33functionWithIndirectResilientEnumy010resilient_A00E8ApproachOAEF"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1)
public func functionWithIndirectResilientEnum(_ ia: IndirectApproach) -> IndirectApproach {

// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s14resilient_enum16IndirectApproachOMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], [[INT]] -1
// CHECK-NEXT: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]
// This is copying the +0 argument into the return slot.
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK-NEXT: [[WITNESS_FN:%.*]]  = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call ptr [[WITNESS_FN]](ptr noalias %0, ptr noalias %1, ptr [[METADATA]])
// CHECK-NEXT: ret void

  return ia
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience31constructResilientEnumNoPayload010resilient_A06MediumOyF"
public func constructResilientEnumNoPayload() -> Medium {
// CHECK:      [[TAG:%.*]] = load i32, ptr @"$s14resilient_enum6MediumO5PaperyA2CmFWC"
// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s14resilient_enum6MediumOMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], [[INT]] -1
// CHECK-NEXT: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]

// CHECK-16-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 16
// CHECK-32-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 14
// CHECK-64-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 13
// CHECK-NEXT: [[WITNESS_FN:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call void [[WITNESS_FN]](ptr noalias %0, i32 [[TAG]], ptr [[METADATA]])

// CHECK-NEXT: ret void
  return Medium.Paper
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience29constructResilientEnumPayloady010resilient_A06MediumO0G7_struct4SizeVF"
public func constructResilientEnumPayload(_ s: Size) -> Medium {
// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], [[INT]] -1
// CHECK-NEXT: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], i32 0, i32 8
// CHECK-NEXT: [[WITNESS_FOR_SIZE:%size]] = load [[INT]], ptr [[WITNESS_ADDR]]
// CHECK-NEXT: [[ALLOCA:%.*]] = alloca i8, {{.*}} [[WITNESS_FOR_SIZE]], align 16
// CHECK-NEXT: call void @llvm.lifetime.start.p0({{(i32|i64)}} -1, ptr [[ALLOCA]])

// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call ptr [[WITNESS]](ptr noalias [[ALLOCA]], ptr noalias %1, ptr [[METADATA]])

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 4
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call ptr [[WITNESS]](ptr noalias %0, ptr noalias [[ALLOCA]], ptr [[METADATA]])

// CHECK-NEXT: [[TAG:%.*]] = load i32, ptr @"$s14resilient_enum6MediumO8PostcardyAC0A7_struct4SizeVcACmFWC"
// CHECK-NEXT: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s14resilient_enum6MediumOMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA2:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[VWT_ADDR2:%.*]] = getelementptr inbounds ptr, ptr [[METADATA2]], [[INT]] -1
// CHECK-NEXT: [[VWT2:%.*]] = load ptr, ptr [[VWT_ADDR2]]

// CHECK-16-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT2]], i32 16
// CHECK-32-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT2]], i32 14
// CHECK-64-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT2]], i32 13
// CHECK-NEXT: [[WITNESS_FN:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call void [[WITNESS_FN]](ptr noalias %0, i32 [[TAG]], ptr [[METADATA2]])
// CHECK-NEXT: call void @llvm.lifetime.end.p0({{(i32|i64)}} -1, ptr [[ALLOCA]])

// CHECK-NEXT: ret void

  return Medium.Postcard(s)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc {{i32|i64}} @"$s15enum_resilience19resilientSwitchTestySi0c1_A06MediumOF"(ptr noalias nocapture %0)
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s14resilient_enum6MediumOMa"([[INT]] 0)
// CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], [[INT]] -1
// CHECK: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]

// CHECK: [[WITNESS_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], i32 0, i32 8
// CHECK: [[WITNESS_FOR_SIZE:%size]] = load [[INT]], ptr [[WITNESS_ADDR]]
// CHECK: [[ALLOCA:%.*]] = alloca i8, {{.*}} [[WITNESS_FOR_SIZE]], align 16
// CHECK: [[ALLOCA:%.*]] = alloca i8, {{.*}} [[WITNESS_FOR_SIZE]], align 16

// CHECK: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK: [[WITNESS_FN:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK: [[ENUM_COPY:%.*]] = call ptr [[WITNESS_FN]](ptr noalias [[ALLOCA]], ptr noalias %0, ptr [[METADATA]])

// CHECK-16-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 14
// CHECK-32-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 12
// CHECK-64-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 11
// CHECK: [[WITNESS_FN:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK: [[TAG:%.*]] = call i32 [[WITNESS_FN]](ptr noalias [[ALLOCA]], ptr [[METADATA]])

// CHECK:  [[PAMPHLET_CASE_TAG:%.*]] = load i32, ptr @"$s14resilient_enum6MediumO8PamphletyA2CcACmFWC"
// CHECK:  [[PAMPHLET_CASE:%.*]] = icmp eq i32 [[TAG]], [[PAMPHLET_CASE_TAG]]
// CHECK:  br i1 [[PAMPHLET_CASE]], label %[[PAMPHLET_CASE_LABEL:.*]], label %[[PAPER_CHECK:.*]]

// CHECK:  [[PAPER_CHECK]]:
// CHECK:  [[PAPER_CASE_TAG:%.*]] = load i32, ptr @"$s14resilient_enum6MediumO5PaperyA2CmFWC"
// CHECK:  [[PAPER_CASE:%.*]] = icmp eq i32 [[TAG]], [[PAPER_CASE_TAG]]
// CHECK:  br i1 [[PAPER_CASE]], label %[[PAPER_CASE_LABEL:.*]], label %[[CANVAS_CHECK:.*]]

// CHECK:  [[CANVAS_CHECK]]:
// CHECK:  [[CANVAS_CASE_TAG:%.*]] = load i32, ptr @"$s14resilient_enum6MediumO6CanvasyA2CmFWC"
// CHECK:  [[CANVAS_CASE:%.*]] = icmp eq i32 [[TAG]], [[CANVAS_CASE_TAG]]
// CHECK:  br i1 [[CANVAS_CASE]], label %[[CANVAS_CASE_LABEL:.*]], label %[[DEFAULT_CASE:.*]]

// CHECK: [[PAPER_CASE_LABEL]]:
// CHECK: br label %[[END:.*]]

// CHECK: [[CANVAS_CASE_LABEL]]:
// CHECK: br label %[[END]]

// CHECK: [[PAMPHLET_CASE_LABEL]]:
// CHECK: swift_projectBox
// CHECK: br label %[[END]]

// CHECK: [[DEFAULT_CASE]]:
// CHeCK: call void %destroy
// CHECK: br label %[[END]]

// CHECK: [[END]]:
// CHECK: = phi [[INT]] [ 3, %[[DEFAULT_CASE]] ], [ {{.*}}, %[[PAMPHLET_CASE_LABEL]] ], [ 2, %[[CANVAS_CASE_LABEL]] ], [ 1, %[[PAPER_CASE_LABEL]] ]
// CHECK: ret

public func resilientSwitchTest(_ m: Medium) -> Int {
  switch m {
  case .Paper:
    return 1
  case .Canvas:
    return 2
  case .Pamphlet(let m):
    return resilientSwitchTest(m)
  default:
    return 3
  }
}

public func reabstraction<T>(_ f: (Medium) -> T) {}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience25resilientEnumPartialApplyyySi0c1_A06MediumOXEF"(ptr %0, ptr %1)
public func resilientEnumPartialApply(_ f: (Medium) -> Int) {

// CHECK:     [[STACKALLOC:%.*]] = alloca i8
// CHECK:     call swiftcc void @"$s15enum_resilience13reabstractionyyx010resilient_A06MediumOXElF"(ptr @"$s14resilient_enum6MediumOSiIgnd_ACSiIegnr_TRTA{{(\.ptrauth)?}}", ptr [[CONTEXT:%.*]], ptr @"$sSiN")
  reabstraction(f)

// CHECK:     ret void
}

// CHECK-LABEL: define internal swiftcc void @"$s14resilient_enum6MediumOSiIgnd_ACSiIegnr_TRTA"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr swiftself %2)


// Enums with resilient payloads from a different resilience domain
// require runtime metadata instantiation, just like generics.

public enum EnumWithResilientPayload {
  case OneSize(Size)
  case TwoSizes(Size, Size)
}

// Make sure we call a function to access metadata of enums with
// resilient layout.

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s15enum_resilience20getResilientEnumTypeypXpyF"()
// CHECK:      [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s15enum_resilience24EnumWithResilientPayloadOMa"([[INT]] 0)
// CHECK:      [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: ret ptr [[METADATA]]

public func getResilientEnumType() -> Any.Type {
  return EnumWithResilientPayload.self
}

// Public metadata accessor for our resilient enum
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15enum_resilience24EnumWithResilientPayloadOMa"(
// CHECK: [[LOAD_METADATA:%.*]] = load ptr, ptr @"$s15enum_resilience24EnumWithResilientPayloadOMl", align
// CHECK-NEXT: [[COND:%.*]] = icmp eq ptr [[LOAD_METADATA]], null
// CHECK-NEXT: br i1 [[COND]], label %cacheIsNull, label %cont

// CHECK: cacheIsNull:
// CHECK-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @swift_getSingletonMetadata([[INT]] %0, ptr @"$s15enum_resilience24EnumWithResilientPayloadOMn")
// CHECK-NEXT: [[RESPONSE_METADATA:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK-NEXT: [[RESPONSE_STATE:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 1
// CHECK-NEXT: br label %cont

// CHECK: cont:
// CHECK-NEXT: [[RESULT_METADATA:%.*]] = phi ptr [ [[LOAD_METADATA]], %entry ], [ [[RESPONSE_METADATA]], %cacheIsNull ]
// CHECK-NEXT: [[RESULT_STATE:%.*]] = phi [[INT]] [ 0, %entry ], [ [[RESPONSE_STATE]], %cacheIsNull ]
// CHECK-NEXT: [[T0:%.*]] = insertvalue %swift.metadata_response undef, ptr [[RESULT_METADATA]], 0
// CHECK-NEXT: [[T1:%.*]] = insertvalue %swift.metadata_response [[T0]], [[INT]] [[RESULT_STATE]], 1
// CHECK-NEXT: ret %swift.metadata_response [[T1]]

// Methods inside extensions of resilient enums fish out type parameters
// from metadata -- make sure we can do that
extension ResilientMultiPayloadGenericEnum {

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s14resilient_enum32ResilientMultiPayloadGenericEnumO0B11_resilienceE16getTypeParameterxmyF"(ptr %"ResilientMultiPayloadGenericEnum<T>", ptr noalias nocapture swiftself %0)
// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr %"ResilientMultiPayloadGenericEnum<T>", [[INT]] 2
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
  public func getTypeParameter() -> T.Type {
    return T.self
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15enum_resilience39constructExhaustiveWithResilientMembers010resilient_A011SimpleShapeOyF"(ptr noalias nocapture sret({{.*}}) %0)
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 0)
// CHECK-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: [[WITNESSTABLE_ADDR:%[0-9]+]] = getelementptr inbounds ptr, ptr [[METADATA]], {{(i64|i32)}} -1
// CHECK-NEXT: %.valueWitnesses = load ptr, ptr [[WITNESSTABLE_ADDR]]
// CHECK-NEXT: [[WITNESS_ADDR:%[0-9]+]] = getelementptr inbounds ptr, ptr %.valueWitnesses, i32 7
// CHECK-NEXT: [[WITNESS_FN:%[0-9]+]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr {{.*}} to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call void [[WITNESS_FN]](ptr noalias %0, i32 1, i32 1, ptr [[METADATA]])
// CHECK-NEXT: ret void
// CHECK-NEXT: {{^}$}}
public func constructExhaustiveWithResilientMembers() -> SimpleShape {
  return .KleinBottle
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc { i{{64|32}}, i8 } @"$s15enum_resilience19constructFullyFixed010resilient_A00dE6LayoutOyF"()
// CHECK: ret { [[INT]], i8 } { [[INT]] 0, i8 1 }
// CHECK-NEXT: {{^}$}}
public func constructFullyFixed() -> FullyFixedLayout {
  return .noPayload
}

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s15enum_resilience24EnumWithResilientPayloadOMr"(ptr %0, ptr %1, ptr %2)
// CHECK:        [[TUPLE_LAYOUT:%.*]] = alloca %swift.full_type_layout
// CHECK:        [[SIZE_RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s16resilient_struct4SizeVMa"([[INT]] 319)
// CHECK-NEXT:   [[SIZE_METADATA:%.*]] = extractvalue %swift.metadata_response [[SIZE_RESPONSE]], 0
// CHECK-NEXT:   [[SIZE_STATE:%.*]] = extractvalue %swift.metadata_response [[SIZE_RESPONSE]], 1
// CHECK-NEXT:   [[T0:%.*]] = icmp ule [[INT]] [[SIZE_STATE]], 63
// CHECK-NEXT:   br i1 [[T0]], label %[[SATISFIED1:.*]], label
// CHECK:      [[SATISFIED1]]:
// CHECK-NEXT:   [[T1:%.*]] = getelementptr inbounds ptr, ptr [[SIZE_METADATA]], [[INT]] -1
// CHECK-NEXT:   [[SIZE_VWT:%.*]] = load ptr, ptr [[T1]],
// CHECK-NEXT:   [[SIZE_LAYOUT_1:%.*]] = getelementptr inbounds ptr, ptr [[SIZE_VWT]], i32 8
// CHECK-NEXT:   store ptr [[SIZE_LAYOUT_1]],
// CHECK-NEXT:   getelementptr
// CHECK-NEXT:   [[SIZE_LAYOUT_2:%.*]] = getelementptr inbounds ptr, ptr [[SIZE_VWT]], i32 8
// CHECK-NEXT:   [[SIZE_LAYOUT_3:%.*]] = getelementptr inbounds ptr, ptr [[SIZE_VWT]], i32 8
// CHECK-NEXT:   call swiftcc [[INT]] @swift_getTupleTypeLayout2(ptr [[TUPLE_LAYOUT]], ptr [[SIZE_LAYOUT_2]], ptr [[SIZE_LAYOUT_3]])
// CHECK-NEXT:   store ptr [[TUPLE_LAYOUT]],
// CHECK:        call void @swift_initEnumMetadataMultiPayload
// CHECK:        phi ptr [ [[SIZE_METADATA]], %entry ], [ null, %[[SATISFIED1]] ]
// CHECK:        phi [[INT]] [ 63, %entry ], [ 0, %[[SATISFIED1]] ]


public protocol Prot {
}

private enum ProtGenEnumWithSize<T: Prot> {
    case c1(s1: Size)
    case c2(s2: Size)
}

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s15enum_resilience19ProtGenEnumWithSize33_59077B69D65A4A3BEE0C93708067D5F0LLOyxGAA0C0RzlWOh"(ptr
// CHECK:   ret ptr %0

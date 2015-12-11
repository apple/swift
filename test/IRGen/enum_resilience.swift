// RUN: %target-swift-frontend -I %S/../Inputs -enable-source-import -emit-ir -enable-resilience %s | FileCheck %s
// RUN: %target-swift-frontend -I %S/../Inputs -enable-source-import -emit-ir -enable-resilience -O %s

import resilient_enum
import resilient_struct

// CHECK: %C15enum_resilience5Class = type <{ %swift.refcounted }>
// CHECK: %V15enum_resilience9Reference = type <{ %C15enum_resilience5Class* }>

// Public fixed layout struct contains a public resilient struct,
// cannot use spare bits

// CHECK: %O15enum_resilience6Either = type <{ [[REFERENCE_TYPE:\[(4|8) x i8\]]], [1 x i8] }>

// Public resilient struct contains a public resilient struct,
// can use spare bits (FIXME)

// CHECK: %O15enum_resilience15ResilientEither = type <{ [[REFERENCE_TYPE]], [1 x i8] }>

// Internal fixed layout struct contains a public resilient struct,
// can use spare bits (FIXME)

// CHECK: %O15enum_resilience14InternalEither = type <{ [[REFERENCE_TYPE]], [1 x i8] }>

// Public fixed layout struct contains a fixed layout struct,
// can use spare bits

// CHECK: %O15enum_resilience10EitherFast = type <{ [[REFERENCE_TYPE]] }>

public class Class {}

public struct Reference {
  public var n: Class
}

@_fixed_layout public enum Either {
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

@_fixed_layout public struct ReferenceFast {
  public var n: Class
}

@_fixed_layout public enum EitherFast {
  case Left(ReferenceFast)
  case Right(ReferenceFast)
}

// CHECK-LABEL: define void @_TF15enum_resilience25functionWithResilientEnumFO14resilient_enum6MediumS1_(%swift.opaque* noalias nocapture sret, %swift.opaque* noalias nocapture)
public func functionWithResilientEnum(m: Medium) -> Medium {

// CHECK:      [[METADATA:%.*]] = call %swift.type* @_TMaO14resilient_enum6Medium()
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT:i32|i64]] -1
// CHECK-NEXT: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 9
// CHECK-NEXT: [[WITNESS:%.*]]  = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: call %swift.opaque* [[WITNESS_FN]](%swift.opaque* %0, %swift.opaque* %1, %swift.type* [[METADATA]])
// CHECK-NEXT: ret void

  return m
}

// CHECK-LABEL: define void @_TF15enum_resilience33functionWithIndirectResilientEnumFO14resilient_enum16IndirectApproachS1_(%swift.opaque* noalias nocapture sret, %swift.opaque* noalias nocapture)
public func functionWithIndirectResilientEnum(ia: IndirectApproach) -> IndirectApproach {

// CHECK:      [[METADATA:%.*]] = call %swift.type* @_TMaO14resilient_enum16IndirectApproach()
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT:i32|i64]] -1
// CHECK-NEXT: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 9
// CHECK-NEXT: [[WITNESS:%.*]]  = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: call %swift.opaque* [[WITNESS_FN]](%swift.opaque* %0, %swift.opaque* %1, %swift.type* [[METADATA]])
// CHECK-NEXT: ret void

  return ia
}

// CHECK-LABEL: define void @_TF15enum_resilience31constructResilientEnumNoPayloadFT_O14resilient_enum6Medium
public func constructResilientEnumNoPayload() -> Medium {
// CHECK:      [[METADATA:%.*]] = call %swift.type* @_TMaO14resilient_enum6Medium()
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT:i32|i64]] -1
// CHECK-NEXT: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 25
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: call void [[WITNESS_FN]](%swift.opaque* %0, i32 2, %swift.type* [[METADATA]])

// CHECK-NEXT: ret void
  return Medium.Paper
}

// CHECK-LABEL: define void @_TF15enum_resilience29constructResilientEnumPayloadFV16resilient_struct4SizeO14resilient_enum6Medium
public func constructResilientEnumPayload(s: Size) -> Medium {
// CHECK:      [[METADATA:%.*]] = call %swift.type* @_TMaV16resilient_struct4Size()
// CHECK-NEXT: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK-NEXT: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT:i32|i64]] -1
// CHECK-NEXT: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 6
// CHECK-NEXT: [[WITNESS:%.*]]  = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: [[COPY:%.*]] = call %swift.opaque* %initializeWithCopy(%swift.opaque* %0, %swift.opaque* %1, %swift.type* [[METADATA]])

// CHECK-NEXT: [[METADATA2:%.*]] = call %swift.type* @_TMaO14resilient_enum6Medium()
// CHECK-NEXT: [[METADATA_ADDR2:%.*]] = bitcast %swift.type* [[METADATA2]] to i8***
// CHECK-NEXT: [[VWT_ADDR2:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR2]], [[INT:i32|i64]] -1
// CHECK-NEXT: [[VWT2:%.*]] = load i8**, i8*** [[VWT_ADDR2]]

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT2]], i32 25
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: call void [[WITNESS_FN]](%swift.opaque* %0, i32 1, %swift.type* [[METADATA2]])

// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 4
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK-NEXT: call void [[WITNESS_FN]](%swift.opaque* %1, %swift.type* [[METADATA]])

// CHECK-NEXT: ret void
  return Medium.Postcard(s)
}

// CHECK-LABEL: define {{i32|i64}} @_TF15enum_resilience19resilientSwitchTestFO14resilient_enum6MediumSi(%swift.opaque* noalias nocapture)
// CHECK: [[BUFFER:%.*]] = alloca [[BUFFER_TYPE:\[(12|24) x i8\]]]

// CHECK: [[METADATA:%.*]] = call %swift.type* @_TMaO14resilient_enum6Medium()
// CHECK: [[METADATA_ADDR:%.*]] = bitcast %swift.type* [[METADATA]] to i8***
// CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[METADATA_ADDR]], [[INT]] -1
// CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]

// CHECK: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 5
// CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK: [[ENUM_COPY:%.*]] = call %swift.opaque* [[WITNESS_FN]]([[BUFFER_TYPE]]* [[BUFFER]], %swift.opaque* %0, %swift.type* [[METADATA]])

// CHECK: [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** [[VWT]], i32 23
// CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]]
// CHECK: [[TAG:%.*]] = call i32 %getEnumTag(%swift.opaque* [[ENUM_COPY]], %swift.type* [[METADATA]])

// CHECK: switch i32 [[TAG]], label %[[DEFAULT_CASE:.*]] [
// CHECK:   i32 0, label %[[PAMPHLET_CASE:.*]]
// CHECK:   i32 2, label %[[PAPER_CASE:.*]]
// CHECK:   i32 3, label %[[CANVAS_CASE:.*]]
// CHECK: ]

// CHECK: ; <label>:[[PAPER_CASE]]
// CHECK: br label %[[END:.*]]

// CHECK: ; <label>:[[CANVAS_CASE]]
// CHECK: br label %[[END]]

// CHECK: ; <label>:[[PAMPHLET_CASE]]
// CHECK: br label %[[END]]

// CHECK: ; <label>:[[DEFAULT_CASE]]
// CHECK: br label %[[END]]

// CHECK: ; <label>:[[END]]
// CHECK: ret

public func resilientSwitchTest(m: Medium) -> Int {
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

public func reabstraction<T>(f: Medium -> T) {}

// CHECK-LABEL: define void @_TF15enum_resilience25resilientEnumPartialApplyFFO14resilient_enum6MediumSiT_(i8*, %swift.refcounted*)
public func resilientEnumPartialApply(f: Medium -> Int) {

// CHECK:     [[CONTEXT:%.*]] = call noalias %swift.refcounted* @swift_allocObject
// CHECK:     call void @_TF15enum_resilience13reabstractionurFFO14resilient_enum6MediumxT_(i8* bitcast (void (%Si*, %swift.opaque*, %swift.refcounted*)* @_TPA__TTRXFo_iO14resilient_enum6Medium_dSi_XFo_iS0__iSi_ to i8*), %swift.refcounted* [[CONTEXT:%.*]], %swift.type* @_TMSi)
  reabstraction(f)

// CHECK:     ret void
}

// CHECK-LABEL: define internal void @_TPA__TTRXFo_iO14resilient_enum6Medium_dSi_XFo_iS0__iSi_(%Si* noalias nocapture sret, %swift.opaque* noalias nocapture, %swift.refcounted*)

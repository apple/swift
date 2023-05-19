// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module -target %target-swift-abi-5.8-triple | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-os

// REQUIRES: CPU=x86_64

import Foundation
import gizmo

// -- Protocol records for cast-to ObjC protocols
// CHECK: @_PROTOCOL__TtP13generic_casts10ObjCProto1_ = weak hidden constant
// CHECK: @"\01l_OBJC_LABEL_PROTOCOL_$__TtP13generic_casts10ObjCProto1_" = weak hidden global i8* bitcast ({{.*}} @_PROTOCOL__TtP13generic_casts10ObjCProto1_ to i8*), section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK: @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP13generic_casts10ObjCProto1_" = weak hidden global i8* bitcast ({{.*}} @_PROTOCOL__TtP13generic_casts10ObjCProto1_ to i8*), section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}

// CHECK-macosx: @"_OBJC_PROTOCOL_$_NSRuncing" = weak hidden global
// CHECK-macosx: @"\01l_OBJC_LABEL_PROTOCOL_$_NSRuncing" = weak hidden global i8* bitcast ({{.*}} @"_OBJC_PROTOCOL_$_NSRuncing" to i8*), section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK-macosx: @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing" = weak hidden global i8* bitcast ({{.*}} @"_OBJC_PROTOCOL_$_NSRuncing" to i8*), section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}


// CHECK-linux: @_PROTOCOL_NSRuncing = weak hidden constant
// CHECK-linux: @"\01l_OBJC_LABEL_PROTOCOL_$_NSRuncing" = weak hidden global i8* bitcast ({{.*}} @_PROTOCOL_NSRuncing to i8*), section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK-linux: @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing" = weak hidden global i8* bitcast ({{.*}} @_PROTOCOL_NSRuncing to i8*), section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}

// CHECK: @_PROTOCOLS__TtC13generic_casts10ObjCClass2 = internal constant { i64, [1 x i8*] } {
// CHECK:   i64 1, 
// CHECK:   @_PROTOCOL__TtP13generic_casts10ObjCProto2_
// CHECK: }

// CHECK: @_DATA__TtC13generic_casts10ObjCClass2 = internal constant {{.*}} @_PROTOCOLS__TtC13generic_casts10ObjCClass2

// CHECK: @_PROTOCOL_PROTOCOLS__TtP13generic_casts10ObjCProto2_ = weak hidden constant { i64, [1 x i8*] } {
// CHECK:   i64 1, 
// CHECK:   @_PROTOCOL__TtP13generic_casts10ObjCProto1_
// CHECK: }

// CHECK: define hidden swiftcc i64 @"$s13generic_casts8allToIntySixlF"(%swift.opaque* noalias nocapture %0, %swift.type* %T)
func allToInt<T>(_ x: T) -> Int {
  return x as! Int
  // CHECK: [[INT_TEMP:%.*]] = alloca %TSi,
	// CHECK: [[TYPE_ADDR:%.*]] = bitcast %swift.type* %T to i8***
  // CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[TYPE_ADDR]], i64 -1
  // CHECK: [[VWT:%.*]] = load i8**, i8*** [[VWT_ADDR]]
  // CHECK: [[VWT_CAST:%.*]] = bitcast i8** [[VWT]] to %swift.vwtable*
  // CHECK: [[SIZE_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[VWT_CAST]], i32 0, i32 8
  // CHECK: [[SIZE:%.*]] = load i64, i64* [[SIZE_ADDR]]
  // CHECK: [[T_ALLOCA:%.*]] = alloca i8, {{.*}} [[SIZE]], align 16
  // CHECK: [[T_TMP:%.*]] = bitcast i8* [[T_ALLOCA]] to %swift.opaque*
  // CHECK: [[TEMP:%.*]] = call %swift.opaque* {{.*}}(%swift.opaque* noalias [[T_TMP]], %swift.opaque* noalias %0, %swift.type* %T)
  // CHECK: [[T0:%.*]] = bitcast %TSi* [[INT_TEMP]] to %swift.opaque*
  // CHECK: call zeroext i1 @swift_dynamicCast(%swift.opaque* [[T0]], %swift.opaque* [[T_TMP]], %swift.type* %T, %swift.type* @"$sSiN", i64 7)
  // CHECK: [[T0:%.*]] = getelementptr inbounds %TSi, %TSi* [[INT_TEMP]], i32 0, i32 0
  // CHECK: [[INT_RESULT:%.*]] = load i64, i64* [[T0]],
  // CHECK: ret i64 [[INT_RESULT]]
}

// CHECK: define hidden swiftcc void @"$s13generic_casts8intToAllyxSilF"(%swift.opaque* noalias nocapture sret({{.*}}) %0, i64 %1, %swift.type* %T) {{.*}} {
func intToAll<T>(_ x: Int) -> T {
  // CHECK: [[INT_TEMP:%.*]] = alloca %TSi,
  // CHECK: [[T0:%.*]] = getelementptr inbounds %TSi, %TSi* [[INT_TEMP]], i32 0, i32 0
  // CHECK: store i64 %1, i64* [[T0]],
  // CHECK: [[T0:%.*]] = bitcast %TSi* [[INT_TEMP]] to %swift.opaque*
  // CHECK: call zeroext i1 @swift_dynamicCast(%swift.opaque* %0, %swift.opaque* [[T0]], %swift.type* @"$sSiN", %swift.type* %T, i64 7)
  return x as! T
}

// CHECK: define hidden swiftcc i64 @"$s13generic_casts8anyToIntySiypF"(%Any* noalias nocapture dereferenceable({{.*}}) %0) {{.*}} {
func anyToInt(_ x: Any) -> Int {
  return x as! Int
}

@objc protocol ObjCProto1 {
  static func forClass()
  static func forInstance()

  var prop: NSObject { get }
}

@objc protocol ObjCProto2 : ObjCProto1 {}

@objc class ObjCClass {}

// CHECK: define hidden swiftcc %objc_object* @"$s13generic_casts9protoCastyAA10ObjCProto1_So9NSRuncingpAA0E6CClassCF"(%T13generic_casts9ObjCClassC* %0) {{.*}} {
func protoCast(_ x: ObjCClass) -> ObjCProto1 & NSRuncing {
  // CHECK: load i8*, i8** @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP13generic_casts10ObjCProto1_"
  // CHECK: load i8*, i8** @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing"
  // CHECK: call %objc_object* @swift_dynamicCastObjCProtocolUnconditional(%objc_object* {{%.*}}, i64 2, i8** {{%.*}})
  return x as! ObjCProto1 & NSRuncing
}

@objc class ObjCClass2 : NSObject, ObjCProto2 {
  class func forClass() {}
  class func forInstance() {}

  var prop: NSObject { return self }
}

// <rdar://problem/15313840>
// Class existential to opaque archetype cast
// CHECK: define hidden swiftcc void @"$s13generic_casts33classExistentialToOpaqueArchetypeyxAA10ObjCProto1_plF"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %objc_object* %1, %swift.type* %T)
func classExistentialToOpaqueArchetype<T>(_ x: ObjCProto1) -> T {
  var x = x
  // CHECK: [[X:%.*]] = alloca %T13generic_casts10ObjCProto1P
  // CHECK: [[LOCAL:%.*]] = alloca %T13generic_casts10ObjCProto1P
  // CHECK: [[LOCAL_OPAQUE:%.*]] = bitcast %T13generic_casts10ObjCProto1P* [[LOCAL]] to %swift.opaque*
  // CHECK: [[PROTO_TYPE:%.*]] = call {{.*}}@"$s13generic_casts10ObjCProto1_pMD"
  // CHECK: call zeroext i1 @swift_dynamicCast(%swift.opaque* %0, %swift.opaque* [[LOCAL_OPAQUE]], %swift.type* [[PROTO_TYPE]], %swift.type* %T, i64 7)
  return x as! T
}

protocol P {}
protocol Q {}

// CHECK: define hidden swiftcc void @"$s13generic_casts19compositionToMemberyAA1P_pAaC_AA1QpF{{.*}}"(%T13generic_casts1PP* noalias nocapture sret({{.*}}) %0, %T13generic_casts1P_AA1Qp* noalias nocapture dereferenceable({{.*}}) %1) {{.*}} {
func compositionToMember(_ a: P & Q) -> P {
  return a
}


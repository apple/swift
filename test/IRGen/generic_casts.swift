// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module -target %target-swift-abi-5.8-triple | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-os

// REQUIRES: CPU=x86_64

import Foundation
import gizmo

// -- Protocol records for cast-to ObjC protocols
// CHECK: @_PROTOCOL__TtP13generic_casts10ObjCProto1_ = weak hidden constant
// CHECK: @"\01l_OBJC_LABEL_PROTOCOL_$__TtP13generic_casts10ObjCProto1_" = weak hidden global ptr @_PROTOCOL__TtP13generic_casts10ObjCProto1_, section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK: @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP13generic_casts10ObjCProto1_" = weak hidden global ptr @_PROTOCOL__TtP13generic_casts10ObjCProto1_, section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}

// CHECK-macosx: @"_OBJC_PROTOCOL_$_NSRuncing" = weak hidden global
// CHECK-macosx: @"\01l_OBJC_LABEL_PROTOCOL_$_NSRuncing" = weak hidden global ptr @"_OBJC_PROTOCOL_$_NSRuncing", section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK-macosx: @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing" = weak hidden global ptr @"_OBJC_PROTOCOL_$_NSRuncing", section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}


// CHECK-linux: @_PROTOCOL_NSRuncing = weak hidden constant
// CHECK-linux: @"\01l_OBJC_LABEL_PROTOCOL_$_NSRuncing" = weak hidden global ptr @_PROTOCOL_NSRuncing, section {{"__DATA,__objc_protolist,coalesced,no_dead_strip"|"objc_protolist"|".objc_protolist\$B"}}
// CHECK-linux: @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing" = weak hidden global ptr @_PROTOCOL_NSRuncing, section {{"__DATA,__objc_protorefs,coalesced,no_dead_strip"|"objc_protorefs"|".objc_protorefs\$B"}}

// CHECK: @_PROTOCOLS__TtC13generic_casts10ObjCClass2 = internal constant { i64, [1 x ptr] } {
// CHECK:   i64 1, 
// CHECK:   @_PROTOCOL__TtP13generic_casts10ObjCProto2_
// CHECK: }

// CHECK: @_DATA__TtC13generic_casts10ObjCClass2 = internal constant {{.*}} @_PROTOCOLS__TtC13generic_casts10ObjCClass2

// CHECK: @_PROTOCOL_PROTOCOLS__TtP13generic_casts10ObjCProto2_ = weak hidden constant { i64, [1 x ptr] } {
// CHECK:   i64 1, 
// CHECK:   @_PROTOCOL__TtP13generic_casts10ObjCProto1_
// CHECK: }

// CHECK: define hidden swiftcc i64 @"$s13generic_casts8allToIntySixlF"(ptr noalias nocapture %0, ptr %T)
func allToInt<T>(_ x: T) -> Int {
  return x as! Int
  // CHECK: [[INT_TEMP:%.*]] = alloca %TSi,
  // CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T, i64 -1
  // CHECK: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]
  // CHECK: [[SIZE_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], i32 0, i32 8
  // CHECK: [[SIZE:%.*]] = load i64, ptr [[SIZE_ADDR]]
  // CHECK: [[T_ALLOCA:%.*]] = alloca i8, {{.*}} [[SIZE]], align 16
  // CHECK: [[TEMP:%.*]] = call ptr {{.*}}(ptr noalias [[T_ALLOCA]], ptr noalias %0, ptr %T)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr [[INT_TEMP]], ptr [[T_ALLOCA]], ptr %T, ptr @"$sSiN", i64 7)
  // CHECK: [[T0:%.*]] = getelementptr inbounds %TSi, ptr [[INT_TEMP]], i32 0, i32 0
  // CHECK: [[INT_RESULT:%.*]] = load i64, ptr [[T0]],
  // CHECK: ret i64 [[INT_RESULT]]
}

// CHECK: define hidden swiftcc void @"$s13generic_casts8intToAllyxSilF"(ptr noalias nocapture sret({{.*}}) %0, i64 %1, ptr %T) {{.*}} {
func intToAll<T>(_ x: Int) -> T {
  // CHECK: [[INT_TEMP:%.*]] = alloca %TSi,
  // CHECK: [[T0:%.*]] = getelementptr inbounds %TSi, ptr [[INT_TEMP]], i32 0, i32 0
  // CHECK: store i64 %1, ptr [[T0]],
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr %0, ptr [[INT_TEMP]], ptr @"$sSiN", ptr %T, i64 7)
  return x as! T
}

// CHECK: define hidden swiftcc i64 @"$s13generic_casts8anyToIntySiypF"(ptr noalias nocapture dereferenceable({{.*}}) %0) {{.*}} {
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

// CHECK: define hidden swiftcc ptr @"$s13generic_casts9protoCastyAA10ObjCProto1_So9NSRuncingpAA0E6CClassCF"(ptr %0) {{.*}} {
func protoCast(_ x: ObjCClass) -> ObjCProto1 & NSRuncing {
  // CHECK: load ptr, ptr @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP13generic_casts10ObjCProto1_"
  // CHECK: load ptr, ptr @"\01l_OBJC_PROTOCOL_REFERENCE_$_NSRuncing"
  // CHECK: call ptr @swift_dynamicCastObjCProtocolUnconditional(ptr {{%.*}}, i64 2, ptr {{%.*}})
  return x as! ObjCProto1 & NSRuncing
}

@objc class ObjCClass2 : NSObject, ObjCProto2 {
  class func forClass() {}
  class func forInstance() {}

  var prop: NSObject { return self }
}

// <rdar://problem/15313840>
// Class existential to opaque archetype cast
// CHECK: define hidden swiftcc void @"$s13generic_casts33classExistentialToOpaqueArchetypeyxAA10ObjCProto1_plF"(ptr noalias nocapture sret({{.*}}) %0, ptr %1, ptr %T)
func classExistentialToOpaqueArchetype<T>(_ x: ObjCProto1) -> T {
  var x = x
  // CHECK: [[X:%.*]] = alloca %T13generic_casts10ObjCProto1P
  // CHECK: [[LOCAL:%.*]] = alloca %T13generic_casts10ObjCProto1P
  // CHECK: [[PROTO_TYPE:%.*]] = call {{.*}}@"$s13generic_casts10ObjCProto1_pMD"
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr %0, ptr [[LOCAL]], ptr [[PROTO_TYPE]], ptr %T, i64 7)
  return x as! T
}

protocol P {}
protocol Q {}

// CHECK: define hidden swiftcc void @"$s13generic_casts19compositionToMemberyAA1P_pAaC_AA1QpF{{.*}}"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture dereferenceable({{.*}}) %1) {{.*}} {
func compositionToMember(_ a: P & Q) -> P {
  return a
}


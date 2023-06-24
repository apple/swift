// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize -DINT=i%target-ptrsize
// REQUIRES: objc_interop

import Foundation

class C {}
class O: NSObject {}

struct SSing { var x: Int64 }
struct SMult { var x, y: Int64 }
struct SMult2 { var x, y: C }
struct SMult3 { var x, y: C }

struct GSing<T> { var x: T }
struct GMult<T> { var x, y: T }

enum ESing { case X(Int64) }
enum EMult { case X(Int64), Y(Int64) }

@_alignment(4)
struct CommonLayout { var x,y,z,w: Int8 }

// CHECK:       @"$s16type_layout_objc14TypeLayoutTestVMn" = hidden constant {{.*}} @"$s16type_layout_objc14TypeLayoutTestVMP"
// CHECK:       define internal ptr @"$s16type_layout_objc14TypeLayoutTestVMi"
// CHECK:       define internal swiftcc %swift.metadata_response @"$s16type_layout_objc14TypeLayoutTestVMr"
struct TypeLayoutTest<T> {
  // -- dynamic layout, projected from metadata
  // CHECK:       [[T0:%.*]] = call{{( tail)?}} swiftcc %swift.metadata_response @swift_checkMetadataState([[INT]] 319, ptr %T)
  // CHECK:       [[T_CHECKED:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
  // CHECK:       [[T_STATUS:%.*]] = extractvalue %swift.metadata_response [[T0]], 1
  // CHECK:       [[T_OK:%.*]] = icmp ule [[INT]] [[T_STATUS]], 63
  // CHECK:       br i1 [[T_OK]],
  // CHECK:       [[T1:%.*]] = getelementptr inbounds ptr, ptr [[T_CHECKED]], {{i32|i64}} -1
  // CHECK:       [[T_VALUE_WITNESSES:%.*]] = load ptr, ptr [[T1]]
  // CHECK:       [[T_LAYOUT:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       store ptr [[T_LAYOUT]]
  var z: T
  // -- native class, use standard NativeObject value witness
  // CHECK:       store ptr getelementptr inbounds (ptr, ptr @"$sBoWV", i32 8)
  var a: C
  // -- ObjC class, use standard UnknownObject value witness
  // CHECK:       store ptr getelementptr inbounds (ptr, ptr @"$sBOWV", i32 8)
  var b: O
  // -- Single-element struct, shares layout of its field (Builtin.Int64)
  // CHECK:       store ptr getelementptr inbounds (ptr, ptr @"$sBi64_WV", i32 8)
  var c: SSing
  // -- Multi-element structs use open-coded layouts
  // CHECK:    store ptr @type_layout_16_8_0_pod
  var d: SMult
  // CHECK-64:    store ptr @type_layout_16_8_7fffffff_bt
  // CHECK-32:    store ptr @type_layout_8_4_1000_bt
  var e: SMult2
  // CHECK-64:    store ptr @type_layout_16_8_7fffffff_bt
  // CHECK-32:    store ptr @type_layout_8_4_1000_bt
  var f: SMult3
  // -- Single-case enum, shares layout of its field (Builtin.Int64)
  // CHECK:       store ptr getelementptr inbounds (ptr, ptr @"$sBi64_WV", i32 8)
  var g: ESing
  // -- Multi-case enum, open-coded layout
  // CHECK:    store ptr @type_layout_9_8_fe_pod
  var h: EMult
  // -- Single-element generic struct, shares layout of its field (T)
  // CHECK:       [[T_LAYOUT:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       store ptr [[T_LAYOUT]]
  var i: GSing<T>
  // -- Multi-element generic struct, need to derive from metadata
  // CHECK:       [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s16type_layout_objc5GMultVMa"([[INT]] 319, ptr [[T_CHECKED]])
  // CHECK:       [[METADATA:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK:       [[METADATA_STATUS:%.*]] = extractvalue %swift.metadata_response [[TMP]], 1
  // CHECK:       [[METADATA_OK:%.*]] = icmp ule [[INT]] [[METADATA_STATUS]], 63
  // CHECK:       br i1 [[METADATA_OK]],
  // CHECK:       [[T1:%.*]] = getelementptr inbounds ptr, ptr [[METADATA]], {{i32|i64}} -1
  // CHECK:       [[VALUE_WITNESSES:%.*]] = load ptr, ptr [[T1]]
  // CHECK:       [[LAYOUT:%.*]] = getelementptr inbounds ptr, ptr [[VALUE_WITNESSES]], i32 8
  // CHECK:       store ptr [[LAYOUT]]
  var j: GMult<T>
  // -- Common layout, reuse common value witness table layout
  // CHECK:       store ptr getelementptr (ptr, ptr @"$sBi32_WV", i32 8)
  var k: CommonLayout
}

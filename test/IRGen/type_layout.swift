// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize -DINT=i%target-ptrsize

class C {}

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

struct FourInts { var x,y,z,w: Int32 }

@_alignment(16)
struct AlignedFourInts { var x: FourInts }

// CHECK:       @"$s11type_layout14TypeLayoutTestVMn" = hidden constant {{.*}} @"$s11type_layout14TypeLayoutTestVMP"
// CHECK:       define internal ptr @"$s11type_layout14TypeLayoutTestVMi"
// CHECK:       define internal swiftcc %swift.metadata_response @"$s11type_layout14TypeLayoutTestVMr"
struct TypeLayoutTest<T> {
  // CHECK:       [[TUPLE_LAYOUT_M:%.*]] = alloca %swift.full_type_layout,
  // CHECK:       [[TUPLE_LAYOUT_N:%.*]] = alloca %swift.full_type_layout,
  // CHECK:       [[TUPLE_LAYOUT_O:%.*]] = alloca %swift.full_type_layout,
  // CHECK:       [[TUPLE_ELT_LAYOUTS_O:%.*]] = alloca ptr, [[INT]] 4,
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
  // -- Single-element struct, shares layout of its field (Builtin.Int64)
  // CHECK:       store ptr getelementptr inbounds (ptr, ptr @"$sBi64_WV", i32 8)
  var c: SSing
  // -- Multi-element structs use open-coded layouts
  // CHECK:    store ptr @type_layout_16_8_0_pod
  var d: SMult
  // CHECK-64:    store ptr @type_layout_16_8_[[REF_XI:[0-9a-f]+]]_bt
  // CHECK-32:    store ptr @type_layout_8_4_[[REF_XI:[0-9a-f]+]]_bt
  var e: SMult2
  // CHECK-64:    store ptr @type_layout_16_8_[[REF_XI]]_bt
  // CHECK-32:    store ptr @type_layout_8_4_[[REF_XI]]_bt
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
  // CHECK:       [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s11type_layout5GMultVMa"([[INT]] 319, ptr [[T_CHECKED]])
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
  // -- Single-field aggregate with alignment
  // CHECK:       store ptr getelementptr (ptr, ptr @"$sBi128_WV", i32 8)
  var l: AlignedFourInts
  // -- Tuple with two elements
  // CHECK:       [[T_LAYOUT_1:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T_LAYOUT_2:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       call swiftcc [[INT]] @swift_getTupleTypeLayout2(ptr [[TUPLE_LAYOUT_M]], ptr [[T_LAYOUT_1]], ptr [[T_LAYOUT_2]])
  // CHECK:       store ptr [[TUPLE_LAYOUT_M]]
  var m: (T, T)
  // -- Tuple with three elements
  // CHECK:       [[T_LAYOUT_1:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T_LAYOUT_2:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T_LAYOUT_3:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       call swiftcc { [[INT]], [[INT]] } @swift_getTupleTypeLayout3(ptr [[TUPLE_LAYOUT_N]], ptr [[T_LAYOUT_1]], ptr [[T_LAYOUT_2]], ptr [[T_LAYOUT_3]])
  // CHECK:       store ptr [[TUPLE_LAYOUT_N]]
  var n: (T, T, T)
  // -- Tuple with four elements
  // CHECK:       [[T_LAYOUT_1:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       store ptr [[T_LAYOUT_1]], ptr [[TUPLE_ELT_LAYOUTS_O]],
  // CHECK:       [[T_LAYOUT_2:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T0:%.*]] = getelementptr inbounds ptr, ptr [[TUPLE_ELT_LAYOUTS_O]], i32 1
  // CHECK:       store ptr [[T_LAYOUT_2]], ptr [[T0]],
  // CHECK:       [[T_LAYOUT_3:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T0:%.*]] = getelementptr inbounds ptr, ptr [[TUPLE_ELT_LAYOUTS_O]], i32 2
  // CHECK:       store ptr [[T_LAYOUT_3]], ptr [[T0]],
  // CHECK:       [[T_LAYOUT_4:%.*]] = getelementptr inbounds ptr, ptr [[T_VALUE_WITNESSES]], i32 8
  // CHECK:       [[T0:%.*]] = getelementptr inbounds ptr, ptr [[TUPLE_ELT_LAYOUTS_O]], i32 3
  // CHECK:       store ptr [[T_LAYOUT_4]], ptr [[T0]],
  // CHECK:       call swiftcc void @swift_getTupleTypeLayout(ptr [[TUPLE_LAYOUT_O]], ptr null, [[INT]] 4, ptr [[TUPLE_ELT_LAYOUTS_O]])
  // CHECK:       store ptr [[TUPLE_LAYOUT_O]]
  var o: (T, T, T, T)
}

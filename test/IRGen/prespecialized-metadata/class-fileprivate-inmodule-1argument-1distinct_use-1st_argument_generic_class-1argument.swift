// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main9Argument1[[UNIQUE_ID_1:[0-9A-Z_]+]]CySiGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global 
// CHECK-unknown-SAME: constant
//                   : <{ 
//                   :   void (ptr, 
//                   :   ptr, 
//                   :   i64, 
//                   :   ptr, 
//                   :   ptr, 
//                   :   ptr, 
//                   :   i64, 
//                   :   i32, 
//                   :   i32, 
//                   :   i32, 
//                   :   i16, 
//                   :   i16, 
//                   :   i32, 
//                   :   i32, 
//                   :   ptr, 
//                   :   ptr, 
//                   :   ptr, 
//                   :   i64, 
//                   :   ptr (ptr, ptr 
//                   : }> <{ 
//                   :   void (ptr @"$s4main9Argument1[[UNIQUE_ID_1]]CfD", 
//                   :   $sBoWV
//                   :   i64 ptrtoint (ptr @"$s4main9Argument1[[UNIQUE_ID_1]]CySiGMM" to i64), 
//                   :   OBJC_CLASS_$__TtCs12_SwiftObject
//                   :   _objc_empty_cache
//                   :   ptr null, 
//                   :   i64 add (
//                   :     i64 ptrtoint (
//                   :       { 
//                   :         i32, 
//                   :         i32, 
//                   :         i32, 
//                   :         i32, 
//                   :         ptr, 
//                   :         ptr, 
//                   :         ptr, 
//                   :         ptr, 
//                   :         { 
//                   :           i32, 
//                   :           i32, 
//                   :           [1 x { i64*, ptr, ptr, i32, i32 }] 
//                   :         }*, 
//                   :         ptr, 
//                   :         ptr 
//                   :       }* @"_DATA_$s4main9Argument1[[UNIQUE_ID_1]]CySiGMf" to i64
//                   :     ), 
//                   :     i64 2
//                   :   ), 
//                   :   i32 26, 
//                   :   i32 0, 
//                   :   i32 16, 
//                   :   i16 7, 
//                   :   i16 0, 
//                   :   i32 120, 
//                   :   i32 16, 
//                   :   ptr bitcast (
//                   :     <{ 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i32, 
//                   :       i16, 
//                   :       i16, 
//                   :       i16, 
//                   :       i16, 
//                   :       i8, 
//                   :       i8, 
//                   :       i8, 
//                   :       i8, 
//                   :       i32, 
//                   :       i32, 
//                   :       %swift.method_descriptor 
//                   :     }>* @"$s4main9Argument1[[UNIQUE_ID_1]]CMn" 
//                   :     to ptr
//                   :   ), 
//                   :   ptr null, 
//                   :   $sSiN
//                   :   i64 16, 
//                   :   ptr (ptr, ptr @"$s4main9Argument1[[UNIQUE_ID_1]]C5firstADyxGx_tcfC" 
//                   : }>, align 8

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1]]CyAA9Argument1ACLLCySiGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global 
// CHECK-unknown-SAME: constant
//         CHECK-SAME: <{ 
//         CHECK-SAME:   ptr
//         CHECK-SAME:   ptr, 
//                   :   [[INT]], 
//   CHECK-SAME-apple:   ptr, 
// CHECK-SAME-unknown:   ptr, 
//   CHECK-apple-SAME:   ptr, 
//   CHECK-apple-SAME:   ptr, 
//   CHECK-apple-SAME:   [[INT]], 
//         CHECK-SAME:   i32, 
//         CHECK-SAME:   i32, 
//         CHECK-SAME:   i32, 
//         CHECK-SAME:   i16, 
//         CHECK-SAME:   i16, 
//         CHECK-SAME:   i32, 
//         CHECK-SAME:   i32, 
//         CHECK-SAME:   ptr, 
//         CHECK-SAME:   ptr, 
//         CHECK-SAME:   ptr, 
//         CHECK-SAME:   [[INT]], 
//         CHECK-SAME:   ptr
//         CHECK-SAME: }> <{ 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CyAA9Argument1ACLLCySiGGMM
//   CHECK-apple-SAME:   OBJC_CLASS_$__TtCs12_SwiftObject
//   CHECK-apple-SAME:   _objc_empty_cache
//   CHECK-apple-SAME:   ptr null, 
//   CHECK-apple-SAME:   [[INT]] add (
//   CHECK-apple-SAME:     [[INT]] ptrtoint (
//   CHECK-apple-SAME:       ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]CyAA9Argument1ACLLCySiGGMf" to [[INT]]
//   CHECK-apple-SAME:     ), 
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ), 
//         CHECK-SAME:   i32 26, 
//         CHECK-SAME:   i32 0, 
//         CHECK-SAME:   i32 {{(24|12)}}, 
//         CHECK-SAME:   i16 {{(7|3)}}, 
//         CHECK-SAME:   i16 0, 
//   CHECK-apple-SAME:   i32 {{(128|76)}}, 
// CHECK-unknown-SAME:   i32 104,
//         CHECK-SAME:   i32 {{(24|12)}}, 
//                   :   $s4main5Value[[UNIQUE_ID_1]]CMn
//         CHECK-SAME:   ptr null, 
//         CHECK-SAME:   ptr getelementptr inbounds (
//         CHECK-SAME:     %swift.full_heapmetadata, 
//         CHECK-SAME:     $s4main9Argument1[[UNIQUE_ID_1]]CySiGMf
//         CHECK-SAME:     i32 0, 
//         CHECK-SAME:     i32 3
//         CHECK-SAME:   ), 
//         CHECK-SAME:   [[INT]] {{(16|8)}}, 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGx_tcfC
//         CHECK-SAME: }>, align [[ALIGNMENT]]

fileprivate class Argument1<First> {
  let first_Argument1: First

  init(first: First) {
    self.first_Argument1 = first
  }
}

fileprivate class Value<First> {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

func doit() {
  consume( Value(first: Argument1(first: 13)) )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], ptr %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
//      CHECK:     [[INT]] [[METADATA_REQUEST]], 
//      CHECK:     ptr %1, 
//      CHECK:     ptr undef, 
//      CHECK:     ptr undef, 
// CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]CMn
//      CHECK:   )
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

// CHECK:; Function Attrs: noinline nounwind readnone
//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CyAA9Argument1ACLLCySiGGMb"([[INT]] [[REQUEST:%[0-9]+]]) #{{[0-9]+}} {{(section)?.*}}{
// CHECK-unknown:    ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call ptr @objc_opt_self(
//    CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]CyAA9Argument1ACLLCySiGGMf
//   CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[INITIALIZED_CLASS]], 0
//   CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
// CHECK: }

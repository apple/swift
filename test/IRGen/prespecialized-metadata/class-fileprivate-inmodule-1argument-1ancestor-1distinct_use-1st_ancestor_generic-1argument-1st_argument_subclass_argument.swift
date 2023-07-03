// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//         CHECK: @"$s4main9Ancestor1[[UNIQUE_ID_1:[A-Za-z_0-9]+]]CySiGMf" = linkonce_odr hidden 
//   CHECK-apple: global 
// CHECK-unknown: constant 
//              : <{ 
//              :   ptr, 
//              :   ptr, 
//              :   i64, 
//              :   ptr, 
//              :   ptr, 
//              :   ptr, 
//              :   i64, 
//              :   i32, 
//              :   i32, 
//              :   i32, 
//              :   i16, 
//              :   i16, 
//              :   i32, 
//              :   i32, 
//              :   ptr, 
//              :   ptr, 
//              :   ptr, 
//              :   i64, 
//              :   ptr (ptr, ptr 
//              : }> <{ 
//              :   @"$s4main9Ancestor1[[UNIQUE_ID_1]]CfD",
//              :   $sBoWV
//              :   i64 ptrtoint (
//              :     ptr @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMM" to [[INT]]
//              :   ), 
//              :   OBJC_CLASS_$__TtCs12_SwiftObject
//              :   _objc_empty_cache
//              :   ptr null, 
//              :   i64 add (
//              :     i64 ptrtoint (
//              :       ptr {{[^@]*}}@"_DATA_$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMf" to [[INT]]
//              :     ), 
//              :     i64 2
//              :   ), 
//              :   i32 26, 
//              :   i32 0, 
//              :   i32 16, 
//              :   i16 7, 
//              :   i16 0, 
//              :   i32 120, 
//              :   i32 16, 
//              :   $s4main9Ancestor1[[UNIQUE_ID_1]]CMn
//              :   ptr null, 
//              :   $sSiN
//              :   i64 16, 
//              :   @"$s4main9Ancestor1[[UNIQUE_ID_1]]C5firstADyxGx_tcfC" 
//              : }>, align [[ALIGNMENT]]


//              CHECK: @"$s4main5Value[[UNIQUE_ID_1]]CySiGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global
// CHECK-unknown-SAME: constant
//         CHECK-SAME: <{
//         CHECK-SAME:   ptr, 
//         CHECK-SAME:   ptr, 
//                   :   [[INT]], 
//         CHECK-SAME:   ptr, 
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
//         CHECK-SAME:   ptr,
//         CHECK-SAME:   ptr, 
//         CHECK-SAME:   [[INT]] 
//         CHECK-SAME: }> <{ 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CySiGMM
// CHECK-unknown-SAME:   [[INT]] 0,
//                   :   ptr getelementptr inbounds (
//                   :     %swift.full_heapmetadata, 
//                   :     $s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMf
//                   :     i32 0, 
//                   :     i32 2
//                   :   ), 
//   CHECK-apple-SAME:   _objc_empty_cache
//   CHECK-apple-SAME:   ptr null, 
//   CHECK-apple-SAME:   [[INT]] add (
//   CHECK-apple-SAME:     [[INT]] ptrtoint (
//   CHECK-apple-SAME:       ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]CySiGMf" to [[INT]]
//   CHECK-apple-SAME:     ), 
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ), 
//         CHECK-SAME:   i32 26, 
//         CHECK-SAME:   i32 0, 
//         CHECK-SAME:   i32 {{(32|16)}}, 
//         CHECK-SAME:   i16 {{(7|3)}}, 
//         CHECK-SAME:   i16 0, 
//   CHECK-apple-SAME:   i32 {{(144|84)}}, 
// CHECK-unknown-SAME:   i32 120,
//         CHECK-SAME:   i32 {{(24|12)}}, 
//                   :   $s4main5Value[[UNIQUE_ID_1]]CMn
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfE
//         CHECK-SAME:   $sSiN
//         CHECK-SAME:   [[INT]] {{(16|8)}}, 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGx_tcfC
//         CHECK-SAME:   $sSiN
//         CHECK-SAME:   [[INT]] {{(24|12)}}
//         CHECK-SAME: }>, align [[ALIGNMENT]]


fileprivate class Ancestor1<First> {
  let first_Ancestor1: First

  init(first: First) {
    self.first_Ancestor1 = first
  }
}

fileprivate class Value<First> : Ancestor1<First> {
  let first_Value: First

  override init(first: First) {
    self.first_Value = first
    super.init(first: first)
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

func doit() {
  consume( Value(first: 13) )
}
doit()

//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CySiGMb"
//         CHECK:   [[SUPER_CLASS_METADATA:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMb"([[INT]] 0)
//   CHECK-apple:   [[THIS_CLASS_METADATA:%[0-9]+]] = call ptr @objc_opt_self(
// CHECK-unknown:     ret
//              :     ptr bitcast (
//              :       ptr getelementptr inbounds (
//              :         %swift.full_heapmetadata, 
//    CHECK-SAME:         $s4main5Value[[UNIQUE_ID_1]]CySiGMf
//              :         i32 0, 
//              :         i32 2
//              :       ) to ptr
//              :     )
//              :   )
//   CHECK-apple:   [[RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[THIS_CLASS_METADATA]], 0
//   CHECK-apple:   [[COMPLETE_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[COMPLETE_RESPONSE]]
//         CHECK: }

//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMb"


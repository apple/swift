// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[A-Za-z0-9_]+]]CyAA4LeftACLLVySiGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global 
// CHECK-unknown-SAME: constant 
//         CHECK-SAME: <{
//         CHECK-SAME:   ptr,
//                   :   [[INT]],
//   CHECK-apple-SAME:   ptr,
// CHECK-unknown-SAME:   ptr,
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
//                   :     $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMM
// CHECK-unknown-SAME:   [[INT]] 0,
//   CHECK-apple-SAME:   OBJC_CLASS_$__TtCs12_SwiftObject
// CHECK-unknown-SAME:   ptr null,
//   CHECK-apple-SAME:   _objc_empty_cache
//   CHECK-apple-SAME:   ptr null,
//   CHECK-apple-SAME:   [[INT]] add (
//   CHECK-apple-SAME:     [[INT]] ptrtoint (
//   CHECK-apple-SAME:       ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMf"{{[^,]*}} to [[INT]]
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
//         CHECK-SAME:     %swift.full_type,
//         CHECK-SAME:     $s4main4Left[[UNIQUE_ID_1]]VySiGMf
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 2
//         CHECK-SAME:   ),
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGx_tcfC
//         CHECK-SAME: }>,
//         CHECK-SAME: align [[ALIGNMENT]]

// CHECK: @"$s4main4Left[[UNIQUE_ID_1]]VySiGMf" =

fileprivate class Value<First> {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
  }
}

fileprivate struct Left<Value> {
    let left: Value
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4:[0-9A-Z_]+]]CyAA4LeftACLLVySiGGMb"([[INT]] 0)
// CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     ptr noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     ptr [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: Left(left: 13)) )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], ptr %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]CMn
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//         CHECK: ; Function Attrs: noinline nounwind readnone
//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]+}} {{(section)?.*}}{
// CHECK-unknown: ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call ptr @objc_opt_self(
//    CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMf
//              :   )
//   CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[INITIALIZED_CLASS]], 0
//   CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }

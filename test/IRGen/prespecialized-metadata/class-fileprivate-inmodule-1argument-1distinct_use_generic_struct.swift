// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[A-Za-z0-9_]+]]CyAA4LeftACLLVySiGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global 
// CHECK-unknown-SAME: constant 
//         CHECK-SAME: <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   i8**,
//                   :   [[INT]],
//   CHECK-apple-SAME:   %objc_class*,
// CHECK-unknown-SAME:   %swift.type*,
//   CHECK-apple-SAME:   %swift.opaque*,
//   CHECK-apple-SAME:   %swift.opaque*,
//   CHECK-apple-SAME:   [[INT]],
//         CHECK-SAME:   i32,
//         CHECK-SAME:   i32,
//         CHECK-SAME:   i32,
//         CHECK-SAME:   i16,
//         CHECK-SAME:   i16,
//         CHECK-SAME:   i32,
//         CHECK-SAME:   i32,
//         CHECK-SAME:   %swift.type_descriptor*,
//         CHECK-SAME:   i8*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   )*
//         CHECK-SAME: }> <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//                   :   $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMM
// CHECK-unknown-SAME:   [[INT]] 0,
//   CHECK-apple-SAME:   OBJC_CLASS_$__TtCs12_SwiftObject
// CHECK-unknown-SAME:   %swift.type* null,
//   CHECK-apple-SAME:   %swift.opaque* @_objc_empty_cache,
//   CHECK-apple-SAME:   %swift.opaque* null,
//   CHECK-apple-SAME:   [[INT]] add (
//   CHECK-apple-SAME:     [[INT]] ptrtoint (
//   CHECK-apple-SAME:       {
//   CHECK-apple-SAME:         i32,
//   CHECK-apple-SAME:         i32,
//   CHECK-apple-SAME:         i32,
//                   :         i32,
//   CHECK-apple-SAME:         i8*,
//   CHECK-apple-SAME:         i8*,
//   CHECK-apple-SAME:         i8*,
//   CHECK-apple-SAME:         i8*,
//   CHECK-apple-SAME:         {
//   CHECK-apple-SAME:           i32,
//   CHECK-apple-SAME:           i32,
//   CHECK-apple-SAME:           [
//   CHECK-apple-SAME:             1 x {
//   CHECK-apple-SAME:               [[INT]]*,
//   CHECK-apple-SAME:               i8*,
//   CHECK-apple-SAME:               i8*,
//   CHECK-apple-SAME:               i32,
//   CHECK-apple-SAME:               i32
//   CHECK-apple-SAME:             }
//   CHECK-apple-SAME:           ]
//   CHECK-apple-SAME:         }*,
//   CHECK-apple-SAME:         i8*,
//   CHECK-apple-SAME:         i8*
//   CHECK-apple-SAME:       }* @"_DATA_$s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMf" to [[INT]]
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
//                   :   %swift.type_descriptor* bitcast (
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
//                   :     }>* @"$s4main5Value[[UNIQUE_ID_1]]CMn" to %swift.type_descriptor*
//                   :   ),
//         CHECK-SAME:   i8* null,
//         CHECK-SAME:   %swift.type* getelementptr inbounds (
//         CHECK-SAME:     %swift.full_type,
//         CHECK-SAME:     %swift.full_type* bitcast (
//         CHECK-SAME:       <{
//         CHECK-SAME:         i8**,
//         CHECK-SAME:         [[INT]],
//         CHECK-SAME:         %swift.type_descriptor*,
//         CHECK-SAME:         %swift.type*,
//         CHECK-SAME:         i32,
//                   :         [
//                   :           4 x i8
//                   :         ],
//         CHECK-SAME:         i64
//         CHECK-SAME:       }>* @"$s4main4Left[[UNIQUE_ID_1]]VySiGMf" to %swift.full_type*
//         CHECK-SAME:     ),
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 2
//         CHECK-SAME:   ),
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
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
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: Left(left: 13)) )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
//      CHECK:     [[INT]] [[METADATA_REQUEST]], 
//      CHECK:     i8* [[ERASED_TYPE]], 
//      CHECK:     i8* undef, 
//      CHECK:     i8* undef, 
//      CHECK:     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* 
//      CHECK:       $s4main5Value[[UNIQUE_ID_1]]CMn
//      CHECK:       to %swift.type_descriptor*
//      CHECK:     )
//      CHECK:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//         CHECK: ; Function Attrs: noinline nounwind readnone
//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]+}} {{(section)?.*}}{
//         CHECK: entry:
// CHECK-unknown: ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call %objc_class* @objc_opt_self(
//              :     %objc_class* bitcast (
//              :       %swift.type* getelementptr inbounds (
//              :         %swift.full_heapmetadata,
//              :         %swift.full_heapmetadata* bitcast (
//              :           <{
//              :             void (
//              :               %T4main5Value[
//              :                 [
//              :                   UNIQUE_ID_1
//              :                 ]
//              :               ]C*
//              :             )*,
//              :             i8**,
//              :             [[INT]],
//              :             %objc_class*,
//              :             %swift.opaque*,
//              :             %swift.opaque*,
//              :             [[INT]],
//              :             i32,
//              :             i32,
//              :             i32,
//              :             i16,
//              :             i16,
//              :             i32,
//              :             i32,
//              :             %swift.type_descriptor*,
//              :             i8*,
//              :             %swift.type*,
//              :             [[INT]],
//              :             %T4main5Value[
//              :               [
//              :                 UNIQUE_ID_1
//              :               ]
//              :             ]C* (
//              :               %swift.opaque*,
//              :               %swift.type*
//              :             )*
//              :           }>* 
//    CHECK-SAME:           @"$s4main5Value[[UNIQUE_ID_1]]CyAA4LeftACLLVySiGGMf" 
//              :           to %swift.full_heapmetadata*
//              :         ),
//              :         i32 0,
//              :         i32 2
//              :       ) to %objc_class*
//              :     )
//              :   )
//   CHECK-apple:   [[INITIALIZED_METADATA:%[0-9]+]] = bitcast %objc_class* [[INITIALIZED_CLASS]] to %swift.type*
//   CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[INITIALIZED_METADATA]], 0
//   CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }

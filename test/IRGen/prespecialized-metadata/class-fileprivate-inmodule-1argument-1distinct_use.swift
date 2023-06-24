// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[0-9A-Z_]+]]CySiGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global
// CHECK-unknown-SAME: constant 
//         CHECK-SAME: <{
//         CHECK-SAME:   void (%T4main5Value[[UNIQUE_ID_1]]C*)*, 
//         CHECK-SAME:   i8**, 
//                   :   [[INT]], 
//   CHECK-apple-SAME:   %objc_class*
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
//   CHECK-apple-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (%swift.opaque*, %swift.type*)* 
//         CHECK-SAME: }> <{ 
//         CHECK-SAME:   void (%T4main5Value[[UNIQUE_ID_1]]C*)* 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CySiGMM
//   CHECK-apple-SAME:   OBJC_CLASS_$__TtCs12_SwiftObject
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
//   CHECK-apple-SAME:         { i32, i32, [1 x { [[INT]]*, i8*, i8*, i32, i32 }] }*, 
//   CHECK-apple-SAME:         i8*, 
//   CHECK-apple-SAME:         i8* 
//   CHECK-apple-SAME:       }* @"_DATA_$s4main5Value[[UNIQUE_ID_1]]CySiGMf" to [[INT]]
//   CHECK-apple-SAME:     ), 
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ), 
// CHECK-unknown-SAME:  i64 0, 
//         CHECK-SAME:   i32 26, 
//         CHECK-SAME:   i32 0, 
//         CHECK-SAME:   i32 {{(24|12)}}, 
//         CHECK-SAME:   i16 {{(7|3)}}, 
//         CHECK-SAME:   i16 0, 
//   CHECK-apple-SAME:   i32 {{(128|76)}}, 
// CHECK-unknown-SAME:   i32 104,
//         CHECK-SAME:   i32 {{(24|12)}}, 
//                   :   %swift.type_descriptor* bitcast (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* @"$s4main5Value[[UNIQUE_ID_1]]CMn" to %swift.type_descriptor*), 
//         CHECK-SAME:   i8* null, 
//         CHECK-SAME:   %swift.type* @"$sSiN", 
//         CHECK-SAME:   [[INT]] {{(16|8)}}, 
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (%swift.opaque*, %swift.type*)* 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGx_tcfC
//         CHECK-SAME: }>, align [[ALIGNMENT]]

fileprivate class Value<First> {
  let first: First

  init(first: First) {
    self.first = first
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4:[0-9A-Z_]+]]CySiGMb"([[INT]] 0)
// CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: 13) )
}
doit()

// CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK:     [[INT]] [[METADATA_REQUEST]], 
// CHECK:     i8* [[ERASED_TYPE]], 
// CHECK:     i8* undef, 
// CHECK:     i8* undef, 
// CHECK:     %swift.type_descriptor* bitcast (
//      :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* 
// CHECK:       $s4main5Value[[UNIQUE_ID_1]]CMn
// CHECK:       to %swift.type_descriptor*
// CHECK:     )
// CHECK:   ) #{{[0-9]+}}
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }


// CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4]]CySiGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]}} {{(section)?.*}}{
// CHECK: entry:
// CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call %objc_class* @objc_opt_self(
// CHECK-apple:    %objc_class* bitcast (
// CHECK-unknown:    ret
// CHECK-SAME:   @"$s4main5Value[[UNIQUE_ID_1]]CySiGMf" 
// CHECK-apple:   [[INITIALIZED_METADATA:%[0-9]+]] = bitcast %objc_class* [[INITIALIZED_CLASS]] to %swift.type*
// CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[INITIALIZED_METADATA]], 0
// CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
// CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
// CHECK: }

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -prespecialize-generic-metadata -target %module-target-future %S/Inputs/struct-public-frozen-1argument.swift %S/Inputs/struct-public-frozen-0argument.swift -emit-library -o %t/%target-library-name(Generic) -emit-module -module-name Generic -emit-module-path %t/Generic.swiftmodule -enable-library-evolution
// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s -L %t -I %t -lGeneric | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment 
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s -L %t -I %t -lGeneric

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//      CHECK: @"$s7Generic11OneArgumentVyAA7IntegerVGMN" = linkonce_odr hidden constant <{
// CHECK-SAME:   i8**,
// CHECK-SAME:   [[INT]],
// CHECK-SAME:   %swift.type_descriptor*,
// CHECK-SAME:   %swift.type*,
// CHECK-SAME:   i32,
// CHECK-SAME:   i32,
// CHECK-SAME:   i64
// CHECK-SAME: }> <{
//           :   i8** getelementptr inbounds (
//           :     %swift.vwtable,
//           :     %swift.vwtable* @"
// CHECK-SAME:     $s7Generic11OneArgumentVyAA7IntegerVGWV
//           :     ",
//           :     i32 0,
//           :     i32 0
//           :   ),
// CHECK-SAME:   [[INT]] 512,
//           :   %swift.type_descriptor* @"
// CHECK-SAME:   $s7Generic11OneArgumentVMn
//           :   ",
// CHECK-SAME:   %swift.type* @"$s7Generic7IntegerVN",
// CHECK-SAME:   i32 0,
// CHECK-SAME:   i32 {{4|8}},
// CHECK-SAME:   i64 1
// CHECK-SAME: }>,
// CHECK-SAME: align [[ALIGNMENT]]

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

import Generic

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
//      CHECK:   [[CANONICALIZED_METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @swift_getCanonicalSpecializedMetadata(
// CHECK-SAME:     [[INT]] 0, 
// CHECK-SAME:     %swift.type* getelementptr inbounds (
// CHECK-SAME:       %swift.full_type,
// CHECK-SAME:       %swift.full_type* bitcast (
// CHECK-SAME:         <{
// CHECK-SAME:           i8*,
// CHECK-SAME:           i8**,
// CHECK-SAME:           [[INT]],
// CHECK-SAME:           %swift.type_descriptor*,
// CHECK-SAME:           %swift.type*,
// CHECK-SAME:           i32,
// CHECK-SAME:           i32,
// CHECK-SAME:           i64
// CHECK-SAME:         }>* @"$s7Generic11OneArgumentVyAA7IntegerVGMN" to %swift.full_type*
// CHECK-SAME:       ),
// CHECK-SAME:       i32 0,
// CHECK-SAME:       i32 2
// CHECK-SAME:     ),
// CHECK-SAME:   %swift.type** @"$s7Generic11OneArgumentVyAA7IntegerVGMJ"
// CHECK-SAME:   )
// CHECK-NEXT:   [[CANONICALIZED_METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[CANONICALIZED_METADATA_RESPONSE]], 0
// CHECK-NEXT:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[CANONICALIZED_METADATA]]
// CHECK-SAME:   )
// CHECK: }
func doit() {
  consume( OneArgument(Integer(13)) )
}
doit()




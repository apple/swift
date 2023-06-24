// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[0-9A-Z_]+]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global
// CHECK-unknown-SAME: constant 
//         CHECK-SAME: <{
//         CHECK-SAME:   void (%T4main5Value[[UNIQUE_ID_1]]LLC*)*, 
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
// CHECK-unknown-SAME:   %swift.type*, 
//         CHECK-SAME:   [[INT]], 
// CHECK-unknown-SAME:   [[INT]], 
//   CHECK-apple-SAME:   %T4main5Value[[UNIQUE_ID_1]]LLC* (
//   CHECK-apple-SAME:     %swift.opaque*, 
//   CHECK-apple-SAME:     %swift.opaque*, 
//   CHECK-apple-SAME:     %swift.type*
//         CHECK-SAME:   )* 
//         CHECK-SAME: }> <{ 
//         CHECK-SAME:   void (%T4main5Value[[UNIQUE_ID_1]]LLC*)* 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMM
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
//   CHECK-apple-SAME:         {
//   CHECK-apple-SAME:           i32,
//   CHECK-apple-SAME:           i32,
//   CHECK-apple-SAME:           [
//   CHECK-apple-SAME:             2 x {
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
//   CHECK-apple-SAME:       }* @"_DATA_$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf" to [[INT]]
//   CHECK-apple-SAME:     ), 
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ), 
// CHECK-unknown-SAME:  [[INT]] 0, 
// CHECK-unknown-SAME:  %swift.type* null, 
//         CHECK-SAME:   i32 26, 
//         CHECK-SAME:   i32 0, 
//         CHECK-SAME:   i32 {{(32|16)}}, 
//         CHECK-SAME:   i16 {{(7|3)}}, 
//         CHECK-SAME:   i16 0, 
//   CHECK-apple-SAME:   i32 {{(144|84)}}, 
// CHECK-unknown-SAME:   i32 120,
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
//         CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]LLCMn
//                   :   ),
//         CHECK-SAME:   i8* null,
//         CHECK-SAME:   %swift.type* getelementptr inbounds (
//         CHECK-SAME:     %swift.full_heapmetadata,
//         CHECK-SAME:     %swift.full_heapmetadata* bitcast (
//         CHECK-SAME:       <{
//         CHECK-SAME:         void (
//         CHECK-SAME:           %T4main9Argument1[[UNIQUE_ID_1]]LLC*
//         CHECK-SAME:         )*,
//         CHECK-SAME:         i8**,
//                   :         [[INT]],
//   CHECK-apple-SAME:         %objc_class*,
// CHECK-unknown-SAME:         %swift.type*,
//   CHECK-apple-SAME:         %swift.opaque*,
//   CHECK-apple-SAME:         %swift.opaque*,
//   CHECK-apple-SAME:         [[INT]],
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i16,
//         CHECK-SAME:         i16,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         %swift.type_descriptor*,
//         CHECK-SAME:         i8*,
//         CHECK-SAME:         %swift.type*,
//         CHECK-SAME:         [[INT]],
//         CHECK-SAME:         %T4main9Argument1[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:           %swift.opaque*,
//         CHECK-SAME:           %swift.type*
//         CHECK-SAME:         )*
//         CHECK-SAME:       }>* @"$s4main9Argument1[[UNIQUE_ID_1]]LLCySiGMf" to %swift.full_heapmetadata*
//         CHECK-SAME:     ),
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 3
//         CHECK-SAME:   ),
//         CHECK-SAME:   %swift.type* getelementptr inbounds (
//         CHECK-SAME:     %swift.full_heapmetadata,
//         CHECK-SAME:     %swift.full_heapmetadata* bitcast (
//         CHECK-SAME:       <{
//         CHECK-SAME:         void (
//         CHECK-SAME:           %T4main9Argument2[[UNIQUE_ID_1]]LLC*
//         CHECK-SAME:         )*,
//         CHECK-SAME:         i8**,
//                   :         [[INT]],
//   CHECK-apple-SAME:         %objc_class*,
// CHECK-unknown-SAME:         %swift.type*,
//   CHECK-apple-SAME:         %swift.opaque*,
//   CHECK-apple-SAME:         %swift.opaque*,
//   CHECK-apple-SAME:         [[INT]],
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i16,
//         CHECK-SAME:         i16,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         i32,
//         CHECK-SAME:         %swift.type_descriptor*,
//         CHECK-SAME:         i8*,
//         CHECK-SAME:         %swift.type*,
//         CHECK-SAME:         [[INT]],
//         CHECK-SAME:         %T4main9Argument2[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:           %swift.opaque*,
//         CHECK-SAME:           %swift.type*
//         CHECK-SAME:         )*
//         CHECK-SAME:       }>* @"$s4main9Argument2[[UNIQUE_ID_1]]LLCySSGMf" to %swift.full_heapmetadata*
//         CHECK-SAME:     ),
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 3
//         CHECK-SAME:   ),
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   [[INT]] {{(24|12)}},
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLC5first6secondADyxq_Gx_q_tcfC
//         CHECK-SAME: }>,
//         CHECK-SAME: align [[ALIGNMENT]]

fileprivate class Value<First, Second> {
  let first: First
  let second: Second

  init(first: First, second: Second) {
    self.first = first
    self.second = second
  }
}

fileprivate class Argument1<Value> {
  let value: Value

  init(value: Value) {
    self.value = value
  }
}

fileprivate class Argument2<Value> {
  let value: Value

  init(value: Value) {
    self.value = value
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4:[0-9A-Z_]+]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMb"([[INT]] 0)
// CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: Argument1(value: 13), second: Argument2(value: "13")) )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* [[ARGUMENT1_METADATA:%[0-9]+]], %swift.type* [[ARGUMENT2_METADATA:%[0-9]+]]) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   [[ERASED_ARGUMENT1:%[0-9]+]] = bitcast %swift.type* [[ARGUMENT1_METADATA]] to i8*
//      CHECK:   [[ERASED_ARGUMENT2:%[0-9]+]] = bitcast %swift.type* [[ARGUMENT2_METADATA]] to i8*
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
//      CHECK:     [[INT]] [[METADATA_REQUEST]], 
//      CHECK:     i8* [[ERASED_ARGUMENT1]], 
//      CHECK:     i8* [[ERASED_ARGUMENT2]], 
//      CHECK:     i8* undef, 
//      CHECK:     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* 
//      CHECK:       $s4main5Value[[UNIQUE_ID_1]]LLCMn
//      CHECK:       to %swift.type_descriptor*
//      CHECK:     )
//      CHECK:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }


//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]}} {{(section)?.*}}{
//         CHECK: entry:
//         CHECK:  call swiftcc %swift.metadata_response @"$s4main9Argument1[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] 0)
//         CHECK:  call swiftcc %swift.metadata_response @"$s4main9Argument2[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] 0)
// CHECK-unknown:  ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call %objc_class* @objc_opt_self(
//              :    %objc_class* bitcast (
//              :      %swift.type* getelementptr inbounds (
//              :        %swift.full_heapmetadata,
//              :        %swift.full_heapmetadata* bitcast (
//              :          <{
//              :            void (
//              :              %T4main5Value[[UNIQUE_ID_1]]LLC*
//              :            )*,
//              :            i8**,
//              :            [[INT]],
//              :            %objc_class*,
//              :            %swift.opaque*,
//              :            %swift.opaque*,
//              :            [[INT]],
//              :            i32,
//              :            i32,
//              :            i32,
//              :            i16,
//              :            i16,
//              :            i32,
//              :            i32,
//              :            %swift.type_descriptor*,
//              :            i8*,
//              :            %swift.type*,
//              :            %swift.type*,
//              :            [[INT]],
//              :            [[INT]],
//              :            %T4main5Value[[UNIQUE_ID_1]]LLC* (
//              :              %swift.opaque*,
//              :              %swift.opaque*,
//              :              %swift.type*
//              :            )*
//              :          }>* 
//    CHECK-SAME:          @"$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf" 
//              :          to %swift.full_heapmetadata*
//              :        ),
//              :        i32 0,
//              :        i32 2
//              :      ) to %objc_class*
//              :    )
//              :  )
//   CHECK-apple:  [[INITIALIZED_METADATA:%[0-9]+]] = bitcast %objc_class* [[INITIALIZED_CLASS]] to %swift.type*
//   CHECK-apple:  [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[INITIALIZED_METADATA]], 0
//   CHECK-apple:  [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:  ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }


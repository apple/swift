// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: OS=macosx || OS=ios || OS=tvos || OS=watchos || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK-DAG: @"$s4main9Ancestor1[[UNIQUE_ID_1:[0-9a-zA-Z_]+]]CySiGMf" =
// CHECK-DAG: @"$s4main9Ancestor2[[UNIQUE_ID_1]]CySiGMf" = 

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1]]CySSGMf" = linkonce_odr hidden 
// CHECK-unknown-SAME: constant 
//   CHECK-apple-SAME: global 
//         CHECK-SAME: <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   i8**,
//                   :   [[INT]],
//         CHECK-SAME:   %swift.type*,
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
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %TSi*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   )*
//         CHECK-SAME: }> <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]CySSGMM
// CHECK-unknown-SAME:   [[INT]] 0,
//                   :   %swift.type* getelementptr inbounds (
//                   :     %swift.full_heapmetadata,
//                   :     %swift.full_heapmetadata* bitcast (
//                   :       <{
//                   :         void (
//                   :           %T4main9Ancestor1[[UNIQUE_ID_1]]C*
//                   :         )*,
//                   :         i8**,
//                   :         [[INT]],
//                   :         %swift.type*,
//                   :         %swift.opaque*,
//                   :         %swift.opaque*,
//                   :         [[INT]],
//                   :         i32,
//                   :         i32,
//                   :         i32,
//                   :         i16,
//                   :         i16,
//                   :         i32,
//                   :         i32,
//                   :         %swift.type_descriptor*,
//                   :         void (
//                   :           %T4main9Ancestor1[[UNIQUE_ID_1]]C*
//                   :         )*,
//                   :         %swift.type*,
//                   :         [[INT]],
//                   :         %T4main9Ancestor1[[UNIQUE_ID_1]]C* (
//                   :           %swift.opaque*,
//                   :           %swift.type*
//                   :         )*,
//                   :         %swift.type*,
//                   :         [[INT]]
//                   :       }>* @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMf" to %swift.full_heapmetadata*
//                   :     ),
//                   :     i32 0,
//                   :     i32 2
//                   :   ),
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
//   CHECK-apple-SAME:       }* @_DATA__TtC4mainP[[UNIQUE_ID_2:[0-9A-Za-z_]+]]5Value to [[INT]]
//   CHECK-apple-SAME:     ),
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ),
//         CHECK-SAME:   i32 26,
//         CHECK-SAME:   i32 0,
//         CHECK-SAME:   i32 {{(48|28)}},
//         CHECK-SAME:   i16 {{(7|3)}},
//         CHECK-SAME:   i16 0,
//   CHECK-apple-SAME:   i32 {{(160|92)}},
// CHECK-unknown-SAME:   i32 136,
//         CHECK-SAME:   i32 {{(16|8)}},
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
//                   :       %swift.method_override_descriptor
//                   :     }>* @"$s4main5Value[[UNIQUE_ID_1]]CMn" to %swift.type_descriptor*
//                   :   ),
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]C*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]CfE
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %TSi*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGSi_tcfCAA9Ancestor2ACLLCAeHyxGx_tcfCTV
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(24|12)}},
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(32|16)}},
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]C* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]C5firstADyxGx_tcfC
//         CHECK-SAME: }>,
//         CHECK-SAME: align [[ALIGNMENT]]

fileprivate class Ancestor2<First> {
  let first_Ancestor2: First

  init(first: First) {
    self.first_Ancestor2 = first
  }
}

fileprivate class Ancestor1<First> : Ancestor2<First> {
  let first_Ancestor1: First

  override init(first: First) {
    self.first_Ancestor1 = first
    super.init(first: first)
  }
}

fileprivate class Value<First> : Ancestor1<Int> {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
    super.init(first: 13)
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CySSGMb"([[INT]] 0)
// CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: "13") )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   br label %[[TYPE_COMPARISON_LABEL:[0-9]+]]
//      CHECK: [[TYPE_COMPARISON_LABEL]]:
//      CHECK:   [[EQUAL_TYPE:%[0-9]+]] = icmp eq i8* bitcast (
// CHECK-SAME: @"$sSiN"
// CHECK-SAME: ), [[ERASED_TYPE]]
//      CHECK:   [[EQUAL_TYPES:%[0-9]+]] = and i1 true, [[EQUAL_TYPE]]
//      CHECK:   br i1 [[EQUAL_TYPES]], label %[[EXIT_PRESPECIALIZED:[0-9]+]], label %[[EXIT_NORMAL:[0-9]+]]
//      CHECK: [[EXIT_PRESPECIALIZED]]:
// CHECK-NEXT:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_3:[0-9A-Z_]+]]CySiGMb"([[INT]] [[METADATA_REQUEST]])
//      CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
//      CHECK:   [[PARTIAL_RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[METADATA]], 0
//      CHECK:   [[RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_RESULT_METADATA]], [[INT]] 0, 1
//      CHECK:   ret %swift.metadata_response [[RESULT_METADATA]] 
//      CHECK: [[EXIT_NORMAL]]:
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* 
// CHECK-SAME:       $s4main9Ancestor2[[UNIQUE_ID_1]]CMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   br label %[[TYPE_COMPARISON_LABEL:[0-9]+]]
//      CHECK: [[TYPE_COMPARISON_LABEL]]:
//      CHECK:   [[EQUAL_TYPE:%[0-9]+]] = icmp eq i8* bitcast (
// CHECK-SAME: @"$sSiN"
// CHECK-SAME: ), [[ERASED_TYPE]]
//      CHECK:   [[EQUAL_TYPES:%[0-9]+]] = and i1 true, [[EQUAL_TYPE]]
//      CHECK:   br i1 [[EQUAL_TYPES]], label %[[EXIT_PRESPECIALIZED:[0-9]+]], label %[[EXIT_NORMAL:[0-9]+]]
//      CHECK: [[EXIT_PRESPECIALIZED]]:
// CHECK-NEXT:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_3:[0-9A-Z_]+]]CySiGMb"([[INT]] [[METADATA_REQUEST]])
//      CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
//      CHECK:   [[PARTIAL_RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[METADATA]], 0
//      CHECK:   [[RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_RESULT_METADATA]], [[INT]] 0, 1
//      CHECK:   ret %swift.metadata_response [[RESULT_METADATA]] 
//      CHECK: [[EXIT_NORMAL]]:
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor, i32, %swift.method_override_descriptor }>* 
// CHECK-SAME:       $s4main9Ancestor1[[UNIQUE_ID_1]]CMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   br label %[[TYPE_COMPARISON_LABEL:[0-9]+]]
//      CHECK: [[TYPE_COMPARISON_LABEL]]:
//      CHECK:   [[EQUAL_TYPE:%[0-9]+]] = icmp eq i8* bitcast (
// CHECK-SAME: @"$sSSN"
// CHECK-SAME: ), [[ERASED_TYPE]]
//      CHECK:   [[EQUAL_TYPES:%[0-9]+]] = and i1 true, [[EQUAL_TYPE]]
//      CHECK:   br i1 [[EQUAL_TYPES]], label %[[EXIT_PRESPECIALIZED:[0-9]+]], label %[[EXIT_NORMAL:[0-9]+]]
//      CHECK: [[EXIT_PRESPECIALIZED]]:
// CHECK-NEXT:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_3:[0-9A-Z_]+]]CySSGMb"([[INT]] [[METADATA_REQUEST]])
//      CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
//      CHECK:   [[PARTIAL_RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[METADATA]], 0
//      CHECK:   [[RESULT_METADATA:%[\" a-zA-Z0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_RESULT_METADATA]], [[INT]] 0, 1
//      CHECK:   ret %swift.metadata_response [[RESULT_METADATA]] 
//      CHECK: [[EXIT_NORMAL]]:
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, %swift.method_override_descriptor }>* 
// CHECK-SAME:       $s4main5Value[[UNIQUE_ID_1]]CMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//         CHECK: ; Function Attrs: noinline nounwind readnone
//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]CySSGMb"([[INT]] {{%[0-9]+}}) #{{[0-9]+}} {
//         CHECK: entry:
//         CHECK:   call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMb"([[INT]] 0)
//     CHECK-NOT:   call swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]CySiGMb"([[INT]] 0)
// CHECK-unknown:   ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call %objc_class* @objc_opt_self(
//    CHECK-SAME:        @"$s4main5Value[[UNIQUE_ID_1]]CySSGMf" 
//   CHECK-apple:   [[INITIALIZED_METADATA:%[0-9]+]] = bitcast %objc_class* [[INITIALIZED_CLASS]] to %swift.type*
//   CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[INITIALIZED_METADATA]], 0
//   CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }

// CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]CySiGMb"([[INT]] {{%[0-9]+}})

// CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]CySiGMb"([[INT]] {{%[0-9]+}})


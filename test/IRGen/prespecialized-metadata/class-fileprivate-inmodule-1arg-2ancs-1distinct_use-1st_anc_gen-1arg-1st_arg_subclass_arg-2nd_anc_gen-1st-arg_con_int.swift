// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK-DAG: @"$s4main9Ancestor1[[UNIQUE_ID_1:[0-9a-zA-Z_]+]]LLCySSGMf" =
// CHECK-DAG: @"$s4main9Ancestor2[[UNIQUE_ID_1]]LLCySiGMf" = 

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1]]LLCySSGMf" = linkonce_odr hidden 
// CHECK-unknown-SAME: constant 
//   CHECK-apple-SAME: global 
//         CHECK-SAME: <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]LLC*
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
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]LLC*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %T4main9Ancestor1[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:     %TSi*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]],
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   )*,
//         CHECK-SAME:   %swift.type*,
//         CHECK-SAME:   [[INT]]
//         CHECK-SAME: }> <{
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]LLC*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCySSGMM
// CHECK-unknown-SAME:   [[INT]] 0,
//                   :   %swift.type* getelementptr inbounds (
//                   :     %swift.full_heapmetadata,
//                   :     %swift.full_heapmetadata* bitcast (
//                   :       <{
//                   :         void (
//                   :           %T4main9Ancestor1[[UNIQUE_ID_1]]LLC*
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
//                   :           %T4main9Ancestor1[[UNIQUE_ID_1]]LLC*
//                   :         )*,
//                   :         %swift.type*,
//                   :         [[INT]],
//                   :         %T4main9Ancestor1[[UNIQUE_ID_1]]LLC* (
//                   :           %TSi*,
//                   :           %swift.type*
//                   :         )*,
//                   :         %swift.type*,
//                   :         [[INT]],
//                   :         %T4main9Ancestor1[[UNIQUE_ID_1]]LLC* (
//                   :           %swift.opaque*,
//                   :           %swift.type*
//                   :         )*
//                   :       }>* @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMf" to %swift.full_heapmetadata*
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
//   CHECK-apple-SAME:       }* @"_DATA_$s4main5Value[[UNIQUE_ID_1]]LLCySSGMf" to [[INT]]
//   CHECK-apple-SAME:     ),
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ),
//         CHECK-SAME:   i32 26,
//         CHECK-SAME:   i32 0,
//         CHECK-SAME:   i32 {{(56|36)}},
//         CHECK-SAME:   i16 {{(7|3)}},
//         CHECK-SAME:   i16 0,
//   CHECK-apple-SAME:   i32 {{(168|96)}},
// CHECK-unknown-SAME:   i32 144,
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
//                   :       %swift.method_override_descriptor
//                   :     }>* @"$s4main5Value[[UNIQUE_ID_1]]LLCMn" to %swift.type_descriptor*
//                   :   ),
//         CHECK-SAME:   void (
//         CHECK-SAME:     %T4main5Value[[UNIQUE_ID_1]]LLC*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCfE
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   %T4main9Ancestor1[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:     %TSi*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   $s4main9Ancestor1[[UNIQUE_ID_1]]LLC5firstADyxGSi_tcfCAA9Ancestor2ACLLCAeHyxGx_tcfCTV
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(24|12)}},
//         CHECK-SAME:   %T4main5Value[[UNIQUE_ID_1]]LLC* (
//         CHECK-SAME:     %swift.opaque*,
//         CHECK-SAME:     %swift.type*
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLC5firstADyxGx_tcfC
//         CHECK-SAME:   %swift.type* @"$sSSN",
//         CHECK-SAME:   [[INT]] {{(40|24)}}
//         CHECK-SAME: }>,
//         CHECK-SAME: align [[ALIGNMENT]]

fileprivate class Ancestor2<First> {
  let first_Ancestor2: First

  init(first: First) {
    self.first_Ancestor2 = first
  }
}

fileprivate class Ancestor1<First> : Ancestor2<Int> {
  let first_Ancestor1: First

  init(first: First) {
    self.first_Ancestor1 = first
    super.init(first: 13)
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

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[METADATA_RESPONSE:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] 0)
// CHECK:   [[METADATA:%[0-9]+]] = extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     %swift.opaque* noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     %swift.type* [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: "13") )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]LLCMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor }>* 
// CHECK-SAME:       $s4main9Ancestor2[[UNIQUE_ID_1]]LLCMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, i32, %swift.method_descriptor, i32, %swift.method_override_descriptor }>* 
// CHECK-SAME:       $s4main9Ancestor1[[UNIQUE_ID_1]]LLCMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK: entry:
//      CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
//      CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     i8* [[ERASED_TYPE]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     i8* undef, 
//           :     %swift.type_descriptor* bitcast (
//           :       <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i16, i16, i8, i8, i8, i8, i32, %swift.method_override_descriptor }>* 
// CHECK-SAME:       $s4main5Value[[UNIQUE_ID_1]]LLCMn
//           :       to %swift.type_descriptor*
//           :     )
// CHECK-SAME:   ) #{{[0-9]+}}
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

//         CHECK: ; Function Attrs: noinline nounwind readnone
//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] {{%[0-9]+}}) #{{[0-9]+}} {{(section)?.*}}{
//         CHECK: entry:
//         CHECK:   call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] 0)
//     CHECK-NOT:   call swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] 0)
// CHECK-unknown:   ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call %objc_class* @objc_opt_self(
//    CHECK-SAME:        @"$s4main5Value[[UNIQUE_ID_1]]LLCySSGMf" 
//   CHECK-apple:   [[INITIALIZED_METADATA:%[0-9]+]] = bitcast %objc_class* [[INITIALIZED_CLASS]] to %swift.type*
//   CHECK-apple:   [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, %swift.type* [[INITIALIZED_METADATA]], 0
//   CHECK-apple:   [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:   ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }

// CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] {{%[0-9]+}})

// CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] {{%[0-9]+}})

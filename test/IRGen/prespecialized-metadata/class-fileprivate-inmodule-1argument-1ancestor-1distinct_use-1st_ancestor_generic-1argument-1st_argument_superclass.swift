// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios


// CHECK: @"$s4main9Ancestor1[[UNIQUE_ID_1:[A-Za-z_0-9]+]]LLCyADySSGGMf" =
//              CHECK: @"$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global
// CHECK-unknown-SAME: constant
//         CHECK-SAME: <{
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//                   :  [[INT]],
//         CHECK-SAME:  ptr,
//   CHECK-apple-SAME:  ptr,
//   CHECK-apple-SAME:  ptr,
//   CHECK-apple-SAME:  [[INT]],
//         CHECK-SAME:  i32,
//         CHECK-SAME:  i32,
//         CHECK-SAME:  i32,
//         CHECK-SAME:  i16,
//         CHECK-SAME:  i16,
//         CHECK-SAME:  i32,
//         CHECK-SAME:  i32,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  [[INT]],
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  [[INT]]
//         CHECK-SAME:}> <{
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:  $sBoWV
//   CHECK-apple-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMM
// CHECK-unknown-SAME:  [[INT]] 0,
//                   :  ptr getelementptr inbounds (
//                   :    %swift.full_heapmetadata,
//                   :    $s4main9Ancestor1[[UNIQUE_ID_1]]LLCyADySSGGMf
//                   :    i32 0,
//                   :    i32 2
//                   :  ),
//   CHECK-apple-SAME:  _objc_empty_cache
//   CHECK-apple-SAME:  ptr null,
//   CHECK-apple-SAME:  [[INT]] add (
//   CHECK-apple-SAME:    [[INT]] ptrtoint (
//   CHECK-apple-SAME:      ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMf" to [[INT]]
//   CHECK-apple-SAME:    ),
//   CHECK-apple-SAME:    [[INT]] 2
//   CHECK-apple-SAME:  ),
//         CHECK-SAME:  i32 26,
//         CHECK-SAME:  i32 0,
//         CHECK-SAME:  i32 {{(32|16)}},
//         CHECK-SAME:  i16 {{(7|3)}},
//         CHECK-SAME:  i16 0,
//   CHECK-apple-SAME:  i32 {{(144|84)}},
// CHECK-unknown-SAME:  i32 120,
//   CHECK-apple-SAME:  i32 {{(24|12)}},
//                   :  $s4main5Value[[UNIQUE_ID_1]]LLCMn
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfE
//         CHECK-SAME:  ptr getelementptr inbounds (
//         CHECK-SAME:    %swift.full_heapmetadata,
//         CHECK-SAME:    $s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMf
//         CHECK-SAME:    i32 0,
//         CHECK-SAME:    i32 3
//         CHECK-SAME:  ),
//         CHECK-SAME:  [[INT]] {{16|8}},
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLC5firstADyxGx_tcfC
//         CHECK-SAME:  ptr getelementptr inbounds (
//         CHECK-SAME:    %swift.full_heapmetadata,
//         CHECK-SAME:    $s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMf
//         CHECK-SAME:    i32 0,
//         CHECK-SAME:    i32 3
//         CHECK-SAME:  ),
//         CHECK-SAME:  [[INT]] {{24|12}}
//         CHECK-SAME:}>,
//         CHECK-SAME:align [[ALIGNMENT]]


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
  consume( Value(first: Ancestor1(first: "13")) )
}
doit()

//        CHECK-LABEL: define hidden swiftcc void @"$s4main4doityyF"()
//              CHECK:   call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMb"
//              CHECK: }

//              CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMb"

//              CHECK: ; Function Attrs: noinline nounwind readnone
//              CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]+}} {{(section)?.*}}{
//         CHECK-NEXT: entry:
//              CHECK:   [[ARGUMENT_CLASS_METADATA:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] 0)
//              CHECK:   [[SUPER_CLASS_METADATA:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor1[[UNIQUE_ID_1]]LLCyADySSGGMb"([[INT]] 0)
//      CHECK-unknown:   ret
//        CHECK-apple:   [[THIS_CLASS_METADATA:%[0-9]+]] = call ptr @objc_opt_self(
//                   :     ptr bitcast (
//                   :       ptr getelementptr inbounds (
//                   :         %swift.full_heapmetadata,
//         CHECK-SAME:         $s4main5Value[[UNIQUE_ID_1]]LLCyAA9Ancestor1ACLLCySSGGMf
//                   :         i32 0,
//                   :         i32 2
//                   :       ) to ptr
//                   :     )
//   CHECK-apple-SAME:   )
//        CHECK-apple:   [[RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[THIS_CLASS_METADATA]], 0
//        CHECK-apple:   [[COMPLETE_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[RESPONSE]], [[INT]] 0, 1
//        CHECK-apple:   ret %swift.metadata_response [[COMPLETE_RESPONSE]]
//              CHECK: }

//              CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main9Ancestor133_51697B6EAB71ECF43599389041BB2421LLCyADySSGGMb"([[INT]] {{%[0-9]+}})


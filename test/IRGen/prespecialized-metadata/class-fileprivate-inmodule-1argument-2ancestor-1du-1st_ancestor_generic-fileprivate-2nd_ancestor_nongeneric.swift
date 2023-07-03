// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s > %t/out.ir
// RUN: %FileCheck --input-file=%t/out.ir %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[A-Za-z_0-9]+]]LLCySiGMf" = linkonce_odr hidden 
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
//         CHECK-SAME:  [[INT]],
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  [[INT]]
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  ptr,
//         CHECK-SAME:  [[INT]]
//         CHECK-SAME:}> <{
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:  $sBoWV
//   CHECK-apple-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCySiGMM
// CHECK-unknown-SAME:  [[INT]] 0,
//                   :  $s4main9Ancestor2[[UNIQUE_ID_1]]LLCySiGMf
//   CHECK-apple-SAME:  _objc_empty_cache
//   CHECK-apple-SAME:  ptr null,
//   CHECK-apple-SAME:  [[INT]] add (
//   CHECK-apple-SAME:    [[INT]] ptrtoint (
//   CHECK-apple-SAME:      ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]LLCySiGMf" to [[INT]]
//   CHECK-apple-SAME:    ),
//   CHECK-apple-SAME:    [[INT]] 2
//   CHECK-apple-SAME:  ),
//         CHECK-SAME:  i32 26,
//         CHECK-SAME:  i32 0,
//         CHECK-SAME:  i32 {{(40|20)}},
//         CHECK-SAME:  i16 {{(7|3)}},
//         CHECK-SAME:  i16 0,
//   CHECK-apple-SAME:  i32 {{(160|92)}},
// CHECK-unknown-SAME:  i32 136,
//         CHECK-SAME:  i32 {{(24|12)}},
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_2:[A-Za-z_0-9]+]]LLCMn
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfE
//         CHECK-SAME:  [[INT]] {{16|8}},
//         CHECK-SAME:  $s4main9Ancestor2[[UNIQUE_ID_1]]LLCyADyxGSicfC
//         CHECK-SAME:  $sSiN
//         CHECK-SAME:  [[INT]] {{24|12}}
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLC5firstADyxGx_tcfC
//         CHECK-SAME:  $sSiN
//         CHECK-SAME:  [[INT]] {{32|16}}
//         CHECK-SAME:}>,
//         CHECK-SAME:align [[ALIGNMENT]]

fileprivate class Ancestor1 {
  let first_Ancestor1: Int

  init(_ int: Int) {
    self.first_Ancestor1 = int
  }
}

fileprivate class Ancestor2<First> : Ancestor1 {
  let first_Ancestor2: First

  init(first: First) {
    self.first_Ancestor2 = first
    super.init(7573672)
  }
}

fileprivate class Value<First> : Ancestor2<First> {
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

//        CHECK-LABEL: define hidden swiftcc void @"$s4main4doityyF"()
//              CHECK:   call swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCySiGMb"
//              CHECK: }

//              CHECK: ; Function Attrs: noinline nounwind readnone
//              CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]+}} {{(section)?.*}}{
//              CHECK:   [[SUPERCLASS_METADATA:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s4main9Ancestor2[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] 0)
//      CHECK-unknown:   ret
//        CHECK-apple:   [[THIS_CLASS_METADATA:%[0-9]+]] = call ptr @objc_opt_self(
//                   :     ptr bitcast (
//                   :       ptr getelementptr inbounds (
//                   :         %swift.full_heapmetadata,
//         CHECK-SAME:         $s4main5Value[[UNIQUE_ID_1]]LLCySiGMf
//                   :         i32 0,
//                   :         i32 2
//                   :       ) to ptr
//                   :     )
//                   :   )
//        CHECK-apple:   [[RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[THIS_CLASS_METADATA]], 0
//        CHECK-apple:   [[COMPLETE_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[RESPONSE]], [[INT]] 0, 1
//        CHECK-apple:   ret %swift.metadata_response [[COMPLETE_RESPONSE]]
//              CHECK: }

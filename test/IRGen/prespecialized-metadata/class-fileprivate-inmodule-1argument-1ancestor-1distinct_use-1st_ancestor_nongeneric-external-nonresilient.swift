// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-library -module-name TestModule -module-link-name TestModule %S/Inputs/class-open-0argument.swift -emit-module-interface -swift-version 5 -o %t/%target-library-name(TestModule)
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

import TestModule

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
//         CHECK-SAME:  ptr
//         CHECK-SAME:}> <{
//         CHECK-SAME:  ptr
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:  $sBoWV
//   CHECK-apple-SAME:    $s4main5Value[[UNIQUE_ID_1]]LLCySiGMM
// CHECK-unknown-SAME:  [[INT]] 0,
//         CHECK-SAME:  $s10TestModule9Ancestor1CN
//   CHECK-apple-SAME:  _objc_empty_cache
//   CHECK-apple-SAME:  ptr null,
//   CHECK-apple-SAME:  [[INT]] add (
//   CHECK-apple-SAME:    _DATA_$s4main5Value[[UNIQUE_ID_1]]LLCySiGMf
//   CHECK-apple-SAME:    [[INT]] 2
//   CHECK-apple-SAME:  ),
//         CHECK-SAME:  i32 26,
//         CHECK-SAME:  i32 0,
//         CHECK-SAME:  i32 {{(32|16)}},
//         CHECK-SAME:  i16 {{(7|3)}},
//         CHECK-SAME:  i16 0,
//   CHECK-apple-SAME:  i32 {{(144|84)}},
// CHECK-unknown-SAME:  i32 120,
//         CHECK-SAME:  i32 {{(24|12)}},
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCMn
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCfE
//         CHECK-SAME:  [[INT]] {{16|8}},
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLCyADyxGSicfC
//         CHECK-SAME:  $sSiN
//         CHECK-SAME:  [[INT]] {{24|12}}
//         CHECK-SAME:  $s4main5Value[[UNIQUE_ID_1]]LLC5firstADyxGx_tcfC
//         CHECK-SAME:}>,
//         CHECK-SAME:align [[ALIGNMENT]]

fileprivate class Value<First> : Ancestor1 {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
    super.init(32)
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
//              CHECK:   [[SUPERCLASS_METADATA:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s10TestModule9Ancestor1CMa"([[INT]] 0)
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

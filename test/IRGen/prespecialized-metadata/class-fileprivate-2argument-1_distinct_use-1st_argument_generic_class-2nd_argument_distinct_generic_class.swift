// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//              CHECK: @"$s4main5Value[[UNIQUE_ID_1:[0-9A-Z_]+]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf" = linkonce_odr hidden 
//   CHECK-apple-SAME: global
// CHECK-unknown-SAME: constant 
//         CHECK-SAME: <{
//         CHECK-SAME:   ptr, 
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
// CHECK-unknown-SAME:   ptr, 
//         CHECK-SAME:   [[INT]], 
// CHECK-unknown-SAME:   [[INT]], 
//   CHECK-apple-SAME:   ptr
//         CHECK-SAME: }> <{ 
//         CHECK-SAME:   ptr
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCfD
//         CHECK-SAME:   $sBoWV
//   CHECK-apple-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMM
//   CHECK-apple-SAME:   OBJC_CLASS_$__TtCs12_SwiftObject
//   CHECK-apple-SAME:   _objc_empty_cache
//   CHECK-apple-SAME:   ptr null, 
//   CHECK-apple-SAME:   [[INT]] add (
//   CHECK-apple-SAME:     ptr {{[^@]*}}@"_DATA_$s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf"{{[^,]*}} to [[INT]]
//   CHECK-apple-SAME:     [[INT]] 2
//   CHECK-apple-SAME:   ), 
// CHECK-unknown-SAME:  [[INT]] 0, 
// CHECK-unknown-SAME:  ptr null, 
//         CHECK-SAME:   i32 26, 
//         CHECK-SAME:   i32 0, 
//         CHECK-SAME:   i32 {{(32|16)}}, 
//         CHECK-SAME:   i16 {{(7|3)}}, 
//         CHECK-SAME:   i16 0, 
//   CHECK-apple-SAME:   i32 {{(144|84)}}, 
// CHECK-unknown-SAME:   i32 120,
//         CHECK-SAME:   i32 {{(24|12)}}, 
//         CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]LLCMn
//         CHECK-SAME:   ptr null,
//         CHECK-SAME:   ptr getelementptr inbounds (
//         CHECK-SAME:     %swift.full_heapmetadata,
//         CHECK-SAME:     $s4main9Argument1[[UNIQUE_ID_1]]LLCySiGMf
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 3
//         CHECK-SAME:   ),
//         CHECK-SAME:   ptr getelementptr inbounds (
//         CHECK-SAME:     %swift.full_heapmetadata,
//         CHECK-SAME:     $s4main9Argument2[[UNIQUE_ID_1]]LLCySSGMf
//         CHECK-SAME:     i32 0,
//         CHECK-SAME:     i32 3
//         CHECK-SAME:   ),
//         CHECK-SAME:   [[INT]] {{(16|8)}},
//         CHECK-SAME:   [[INT]] {{(24|12)}},
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
// CHECK-SAME:     ptr noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     ptr [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: Argument1(value: 13), second: Argument2(value: "13")) )
}
doit()

//      CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]LLCMa"([[INT]] [[METADATA_REQUEST:%[0-9]+]], ptr [[ARGUMENT1_METADATA:%[0-9]+]], ptr [[ARGUMENT2_METADATA:%[0-9]+]]) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK:   call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] [[METADATA_REQUEST]], 
// CHECK-SAME:     ptr [[ARGUMENT1_METADATA]], 
// CHECK-SAME:     ptr [[ARGUMENT2_METADATA]], 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]LLCMn
// CHECK-SAME:   )
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }


//         CHECK: define linkonce_odr hidden swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_4]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMb"([[INT]] {{%[0-9]+}}) {{#[0-9]}} {{(section)?.*}}{
//         CHECK: entry:
//         CHECK:  call swiftcc %swift.metadata_response @"$s4main9Argument1[[UNIQUE_ID_1]]LLCySiGMb"([[INT]] 0)
//         CHECK:  call swiftcc %swift.metadata_response @"$s4main9Argument2[[UNIQUE_ID_1]]LLCySSGMb"([[INT]] 0)
// CHECK-unknown:  ret
//   CHECK-apple:  [[INITIALIZED_CLASS:%[0-9]+]] = call ptr @objc_opt_self(
//              :   ptr getelementptr inbounds (
//              :     %swift.full_heapmetadata,
// CHECK-SAME:        $s4main5Value[[UNIQUE_ID_1]]LLCyAA9Argument1ACLLCySiGAA9Argument2ACLLCySSGGMf
//              :     i32 0,
//              :     i32 2
//              :    )
//              :  )
//   CHECK-apple:  [[PARTIAL_METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response undef, ptr [[INITIALIZED_CLASS]], 0
//   CHECK-apple:  [[METADATA_RESPONSE:%[0-9]+]] = insertvalue %swift.metadata_response [[PARTIAL_METADATA_RESPONSE]], [[INT]] 0, 1
//   CHECK-apple:  ret %swift.metadata_response [[METADATA_RESPONSE]]
//         CHECK: }


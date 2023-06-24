// RUN: %target-swift-frontend %use_no_opaque_pointers -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment
// RUN: %target-swift-frontend -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

//      CHECK: [[EXTRA_DATA_PATTERN:@[0-9]+]]  = internal constant <{ 
// CHECK-SAME:     i32
// CHECK-SAME:   , i32
// CHECK-SAME:   , i32
//           :   , [4 x i8] 
// CHECK-SAME: }> <{ 
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i32 8, 
// CHECK-SAME:   i32 16
//           :   , [4 x i8] zeroinitializer 
// CHECK-SAME: }>, align [[ALIGNMENT]]
//      CHECK: @"$s4main4PairVMP" = internal constant <{ 
//           :   i32, 
//           :   i32, 
//           :   i32, 
//           :   i32, 
//           :   i32, 
//           :   i16, 
//           :   i16 
//           : }> <{ 
//           :   i32 trunc (
//           :     i64 sub (
//           :       i64 ptrtoint (
//           :         %swift.type* (
//           :           %swift.type_descriptor*, 
//           :           i8**, 
//           :           i8*
//           :         )* @"$s4main4PairVMi" to i64
//           :       ), 
//           :       i64 ptrtoint (
//           :         <{ i32, i32, i32, i32, i32, i16, i16 }>* 
//           :         @"$s4main4PairVMP" to i64
//           :       )
//           :     ) to i32
//           :   ), 
//           :   i32 0, 
//           :   i32 1073741827, 
//           :   i32 trunc (
//           :     i64 sub (
//           :       i64 ptrtoint (
//           :         %swift.vwtable* @"$s4main4PairVWV" to i64
//           :       ), 
//           :       i64 ptrtoint (
//           :         i32* getelementptr inbounds (
//           :           <{ i32, i32, i32, i32, i32, i16, i16 }>, 
//           :           <{ i32, i32, i32, i32, i32, i16, i16 }>* @"$s4main4PairVMP", 
//           :           i32 0, 
//           :           i32 3
//           :         ) to i64
//           :       )
//           :     ) to i32
//           :   ), 
//           :   i32 trunc (
// CHECK-SAME:     [[INT]] sub (
// CHECK-SAME:       [[INT]] ptrtoint (
// CHECK-SAME:         <{ i32
// CHECK-SAME:         , i32
// CHECK-SAME:         , i32
//           :         , [4 x i8] 
// CHECK-SAME:         }>* [[EXTRA_DATA_PATTERN]] to [[INT]]
// CHECK-SAME:       ), 
// CHECK-SAME:       [[INT]] ptrtoint (
// CHECK-SAME:         i32* getelementptr inbounds (
// CHECK-SAME:           <{ i32, i32, i32, i32, i32, i16, i16 }>, 
// CHECK-SAME:           <{ i32, i32, i32, i32, i32, i16, i16 }>* @"$s4main4PairVMP", 
// CHECK-SAME:           i32 0, 
// CHECK-SAME:           i32 4
// CHECK-SAME:         ) to [[INT]]
// CHECK-SAME:       )
// CHECK-SAME:     )
//           :   ), 
//           :   i16 3, 
//           :   i16 3 
//           : }>, align 8

struct Pair<First, Second, Third> {
    let first: Int64
    let second: Int64
    let third: Int64
}



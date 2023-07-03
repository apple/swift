// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -enable-library-evolution -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios


//      CHECK: [[EXTRA_DATA_PATTERN:@[0-9]+]] = internal constant <{ [[INT]], i64 }> <{ 
// The payload size is 8: the larger payload is the size of an Int64.
// CHECK-SAME:   [[INT]] 8, 
// CHECK-SAME:   i64 0 
// CHECK-SAME: }>, align [[ALIGNMENT]]


//      CHECK: @"$s4main6EitherOMP" = internal constant <{ 
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
//           :         ptr (
//           :           ptr, 
//           :           ptr, 
//           :           ptr
//           :         )* @"$s4main6EitherOMi" to i64
//           :       ), 
//           :       i64 ptrtoint (
//           :         ptr @"$s4main6EitherOMP" to i64
//           :       )
//           :     ) to i32
//           :   ), 
//           :   i32 0, 
//           :   i32 1075838979, 
//           :   i32 trunc (
//           :     i64 sub (
//           :       i64 ptrtoint (
//           :         ptr @"$s4main6EitherOWV" to i64
//           :       ), 
//           :       i64 ptrtoint (
//           :         ptr getelementptr inbounds (
//           :           <{ i32, i32, i32, i32, i32, i16, i16 }>, 
//           :           $s4main6EitherOMP
//           :           i32 0, 
//           :           i32 3
//           :         ) to i64
//           :       )
//           :     ) to i32
//           :   ), 
//           :   i32 trunc (
// CHECK-SAME:     [[INT]] sub (
// CHECK-SAME:       [[INT]] ptrtoint (
// CHECK-SAME:         ptr [[EXTRA_DATA_PATTERN]] to [[INT]]
// CHECK-SAME:       ), 
// CHECK-SAME:       [[INT]] ptrtoint (
// CHECK-SAME:         ptr getelementptr inbounds (
// CHECK-SAME:           <{ i32, i32, i32, i32, i32, i16, i16 }>, 
// CHECK-SAME:           $s4main6EitherOMP
// CHECK-SAME:           i32 0, 
// CHECK-SAME:           i32 4
// CHECK-SAME:         ) to [[INT]]
// CHECK-SAME:       )
// CHECK-SAME:     )
//           :   ), 
//           :   i16 1, 
//           :   i16 2 
//           : }>, 
//           : align 8

public struct First<T> {
  public let value: Int32
}

public struct Second {
  public let value: Int64
}

@frozen
public enum Either<T> {
  case first(First<T>)
  case second(Second)
}

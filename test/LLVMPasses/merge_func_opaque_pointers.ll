; RUN: %swift-llvm-opt -swift-merge-functions -swiftmergefunc-threshold=4 -opaque-pointers %s | %FileCheck %s

; REQUIRES: PTRSIZE=64

@g1 = external global i32

define internal i64 @return_0(i32 %x, i32 %y) {
  %sum = add i32 %x, %y
  %sum2 = add i32 %sum, %y
  %l = load i32, i32* @g1, align 4
  %sum3 = add i32 %sum2, %y
  ret i64 0
}

define internal ptr @return_null(i32 %x, i32 %y) {
  %sum = add i32 %x, %y
  %sum2 = add i32 %sum, %y
  %l = load i32, i32* @g1, align 4
  %sum3 = add i32 %sum2, %y
  ret ptr null
}

declare void @take_results(i64, ptr)


; CHECK-LABEL: define void @call_return_null
; CHECK:         %1 = call i64 @return_0Tm(i32 0, i32 0)
; CHECK:         %2 = call ptr @return_0Tm(i32 0, i32 0)
define void @call_return_null()  {
  %r1 = call i64 @return_0(i32 0, i32 0)
  %r2 = call ptr @return_null(i32 0, i32 0)
  call void @take_results(i64 %r1, ptr %r2)
  ret void
}

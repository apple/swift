; RUN: %swift-llvm-opt -passes=swift-llvm-arc-optimize %s | %FileCheck %s

target datalayout = "e-p:64:64:64-S128-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f16:16:16-f32:32:32-f64:64:64-f128:128:128-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-apple-macosx10.9"

declare void @swift_release(ptr nocapture)
declare void @swift_retain(ptr) nounwind
declare ptr @_Znwm(i64)
declare i32 @__gxx_personality_v0(...)

define ptr @foo(ptr %0) personality ptr @__gxx_personality_v0 {
entry:
  %1 = tail call ptr @swift_retain(ptr %0)
  %2 = invoke ptr @_Znwm(i64 16)
          to label %continue unwind label %unwind
continue:
  tail call void @swift_release(ptr %1)
  ret ptr %1
unwind:
  %3 = landingpad { ptr, i32 }
          cleanup
  tail call void @swift_release(ptr %1)
  resume { ptr, i32 } %3
}

; CHECK:      unwind:
; CHECK-NEXT: %3 = landingpad { ptr, i32 }
; CHECK-NEXT:         cleanup
; CHECK-NEXT: tail call void @swift_release(ptr %1)

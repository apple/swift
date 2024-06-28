// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-irgen %s | %FileCheck --dump-input-filter=all %s

// REQUIRES: OS=macosx || OS=linux-android

import Closure

// CHECK: define swiftcc void @"$s4main14testNonTrivialyyF"()
// CHECK: %[[V0:.*]] = alloca %{{.*}}, align 8
// CHECK: call void @llvm.lifetime.start.p0(i64 8, ptr %[[V0]])
// CHECK: call {{(void|ptr)}} @__swift_cxx_ctor_ZN10NonTrivialC1Ev(ptr %[[V0]])
// CHECK: call void @_Z5cfunc10NonTrivial(ptr %[[V0]])
// CHECK: call {{(void|ptr)}} @_ZN10NonTrivialD1Ev(ptr %[[V0]])
// CHECK: call void @llvm.lifetime.end.p0(i64 8, ptr %[[V0]])
// CHECK: ret void

public func testNonTrivial() {
  cfunc(NonTrivial());
}

// CHECK: define swiftcc void @"$s4main29testNonTrivialFunctionPointeryyF"()
// CHECK: %[[F_DEBUG:.*]] = alloca ptr, align 8
// CHECK: call void @llvm.memset.p0.i64(ptr align 8 %[[F_DEBUG]], i8 0, i64 8, i1 false)
// CHECK: %[[V0:.*]] = alloca %{{.*}}, align 8
// CHECK: %[[V1:.*]] = call ptr @_Z8getFnPtrv()
// CHECK: store ptr %[[V1]], ptr %[[F_DEBUG]], align 8
// CHECK: call void @llvm.lifetime.start.p0(i64 8, ptr %[[V0]])
// CHECK: call {{(void|ptr)}} @__swift_cxx_ctor_ZN10NonTrivialC1Ev(ptr %[[V0]])
// CHECK: invoke void %[[V1]](ptr %[[V0]])
// CHECK: to label %[[INVOKE_CONT:.*]] unwind label %{{.*}}

// CHECK: [[INVOKE_CONT]]:
// CHECK: call {{(void|ptr)}} @_ZN10NonTrivialD1Ev(ptr %[[V0]])
// CHECK: call void @llvm.lifetime.end.p0(i64 8, ptr %[[V0]])
// CHECK: ret void

public func testNonTrivialFunctionPointer() {
  let f = getFnPtr()
  f(NonTrivial())
}

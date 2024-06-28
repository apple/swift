// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-irgen %s | %FileCheck %s

// REQUIRES: OS=macosx || OS=linux-android

import Closure

// CHECK: define linkonce_odr hidden void @"$sSo10NonTrivialVIegn_ABIeyBX_TR"(ptr %[[V0:.*]], ptr %[[V1:.*]])
// CHECK: %[[V2:.*]] = getelementptr inbounds { %{{.*}}, %{{.*}} }, ptr %[[V0]], i32 0, i32 1
// CHECK-NEXT: %[[_FN:.*]] = getelementptr inbounds %{{.*}}, ptr %[[V2]], i32 0, i32 0
// CHECK-NEXT: %[[V3:.*]] = load ptr, ptr %[[_FN]], align 8
// CHECK-NEXT: %[[_DATA:.*]] = getelementptr inbounds %{{.*}}, ptr %[[V2]], i32 0, i32 1
// CHECK-NEXT: %[[V4:.*]] = load ptr, ptr %[[_DATA]], align 8
// CHECK-NEXT: call ptr @swift_retain(ptr returned %[[V4]])
// CHECK-NEXT: call swiftcc void %[[V3]](ptr noalias dereferenceable(8) %[[V1]], ptr swiftself %[[V4]])
// CHECK-NEXT: call void @swift_release(ptr %[[V4]])
// CHECK-NEXT: ret void

// NonTrivial is destroyed by the caller.
public func testClosureToBlock() {
  cfunc({NonTrivial in})
}

// CHECK: define internal void @"$s4main20testClosureToFuncPtryyFySo10NonTrivialVcfU_To"(ptr %[[V0:.*]])
// CHECK: %[[V1:.*]] = alloca %{{.*}}, align 8
// CHECK-NEXT: call void @llvm.lifetime.start.p0(i64 8, ptr %[[V1]])
// CHECK-NEXT: call {{void|ptr}} @_ZN10NonTrivialC1ERKS_(ptr %[[V1]], ptr %[[V0]])
// CHECK-NEXT: call swiftcc void @"$s4main20testClosureToFuncPtryyFySo10NonTrivialVcfU_"(ptr noalias dereferenceable(8) %[[V1]])
// CHECK-NEXT: call {{void|ptr}} @_ZN10NonTrivialD1Ev(ptr %[[V1]])
// CHECK-NEXT: call void @llvm.lifetime.end.p0(i64 8, ptr %[[V1]])
// CHECK-NEXT: ret void

public func testClosureToFuncPtr() {
  cfunc2({N in})
}

// CHECK: define internal void @"$s4main36testClosureToFuncPtrReturnNonTrivialyyFSo0hI0VycfU_To"(ptr noalias sret(%{{.*}}) %[[V0:.*]])
// CHECK: call swiftcc void @"$s4main36testClosureToFuncPtrReturnNonTrivialyyFSo0hI0VycfU_"(ptr noalias sret(%{{.*}}) %[[V0]])
// CHECK: ret void

public func testClosureToFuncPtrReturnNonTrivial() {
  cfuncReturnNonTrivial2({() -> NonTrivial in return NonTrivial()});
}

// CHECK: define swiftcc { ptr, ptr } @"$s4main13returnFuncPtrySo10NonTrivialVcyF"()
// CHECK: %[[V0:.*]] = call ptr @_Z8getFnPtrv()
// CHECK: %[[V1:.*]] = call noalias ptr @swift_allocObject(ptr getelementptr inbounds (%{{.*}}, ptr @{{.*}}, i32 0, i32 2), i64 24, i64 7) #4
// CHECK: %[[V2:.*]] = getelementptr inbounds <{ %{{.*}}, ptr }>, ptr %[[V1]], i32 0, i32 1
// CHECK: store ptr %[[V0]], ptr %[[V2]], align 8
// CHECK: %[[V3:.*]] = insertvalue { ptr, ptr } { ptr @"$sSo10NonTrivialVIetCX_ABIegn_TRTA", ptr undef }, ptr %[[V1]], 1
// CHECK: ret { ptr, ptr } %[[V3]]

// CHECK: define linkonce_odr hidden swiftcc void @"$sSo10NonTrivialVIetCX_ABIegn_TR"(ptr noalias dereferenceable(8) %[[V0:.*]], ptr %[[V1:.*]])
// CHECK: %[[V2:.*]] = alloca %{{.*}}, align 8
// CHECK: call void @llvm.lifetime.start.p0(i64 8, ptr %[[V2]])
// CHECK: call {{(void|ptr)}} @_ZN10NonTrivialC1ERKS_(ptr %[[V2]], ptr %[[V0]])
// CHECK: invoke void %[[V1]](ptr %[[V2]])
// CHECK: to label %[[INVOKE_CONT:.*]] unwind label %{{.*}}

// CHECK: [[INVOKE_CONT]]:
// CHECK-NEXT: call {{(void|ptr)}} @_ZN10NonTrivialD1Ev(ptr %[[V2]])
// CHECK-NEXT: call void @llvm.lifetime.end.p0(i64 8, ptr %[[V2]])
// CHECK-NEXT: ret void

// CHECK: define internal swiftcc void @"$sSo10NonTrivialVIetCX_ABIegn_TRTA"(ptr noalias dereferenceable(8) %[[V0]], ptr swiftself %[[V1]])
// CHECK: %[[V2]] = getelementptr inbounds <{ %{{.*}}, ptr }>, ptr %[[V1]], i32 0, i32 1
// CHECK-NEXT: %[[V3]] = load ptr, ptr %[[V2]], align 8
// CHECK-NEXT: tail call swiftcc void @"$sSo10NonTrivialVIetCX_ABIegn_TR"(ptr noalias dereferenceable(8) %[[V0]], ptr %[[V3]])
// CHECK-NEXT: ret void

public func returnFuncPtr() -> (NonTrivial) -> () {
  return getFnPtr()
}

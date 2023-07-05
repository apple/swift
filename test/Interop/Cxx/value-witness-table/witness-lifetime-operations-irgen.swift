// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions | %FileCheck %s

// Temporarily restrict to x86 (rdar://89908618)
// REQUIRES: CPU=x86_64

import WitnessLifetimeOperations

struct Holder<T> {
  var holding: T
}

let h = Holder(holding: NonTrivial())

// $sSo10NonTrivialVwxx ---> destroy value witness for __C.NonTrivial
// CHECK-LABEL: define linkonce_odr hidden void @"$sSo10NonTrivialVwxx"
// CHECK-NOT: call
// CHECK: call void @{{_ZN10NonTrivialD(1|2)Ev|"\?\?1NonTrivial@@QEAA@XZ"}}(ptr %{{.*}})
// CHECK-NOT: call
// CHECK: ret void

// $sSo10NonTrivialVwcp ---> initializeWithCopy value witness for __C.NonTrivial
// CHECK-LABEL: define linkonce_odr hidden ptr @"$sSo10NonTrivialVwcp"
// CHECK-NOT: call
// CHECK: call {{void|ptr}} @{{_ZN10NonTrivialC(1|2)ERKS_|"\?\?0NonTrivial@@QEAA@AEBU0@@Z"}}(ptr %{{.*}}, ptr %{{.*}})
// CHECK-NOT: call
// CHECK: ret ptr

// $sSo10NonTrivialVwca ---> assignWithCopy value witness for __C.NonTrivia
// CHECK-LABEL: define linkonce_odr hidden ptr @"$sSo10NonTrivialVwca"
// CHECK-NOT: call
// CHECK: call void @{{_ZN10NonTrivialD(1|2)Ev|"\?\?1NonTrivial@@QEAA@XZ"}}(ptr %{{.*}})
// CHECK: call {{void|ptr}} @{{_ZN10NonTrivialC(1|2)ERKS_|"\?\?0NonTrivial@@QEAA@AEBU0@@Z"}}(ptr %{{.*}}, ptr %{{.*}})
// CHECK-NOT: call
// CHECK: ret ptr

// $sSo10NonTrivialVwtk ---> initializeWithTake value witness for __C.NonTrivial
// CHECK-LABEL: define linkonce_odr hidden ptr @"$sSo10NonTrivialVwtk"
// CHECK-NOT: call
// CHECK: call {{void|ptr}} @{{_ZN10NonTrivialC(1|2)ERKS_|"\?\?0NonTrivial@@QEAA@AEBU0@@Z"}}(ptr %{{.*}}, ptr %{{.*}})
// CHECK-NOT: call
// CHECK: call void @{{_ZN10NonTrivialD(1|2)Ev|"\?\?1NonTrivial@@QEAA@XZ"}}(ptr %{{.*}})
// CHECK-NOT: call
// CHECK: ret ptr

// $sSo10NonTrivialVwta ---> assignWithTake value witness for __C.NonTrivial
// CHECK-LABEL: define linkonce_odr hidden ptr @"$sSo10NonTrivialVwta"
// CHECK-NOT: call
// CHECK: call void @{{_ZN10NonTrivialD(1|2)Ev|"\?\?1NonTrivial@@QEAA@XZ"}}(ptr %{{.*}})
// CHECK: call {{void|ptr}} @{{_ZN10NonTrivialC(1|2)ERKS_|"\?\?0NonTrivial@@QEAA@AEBU0@@Z"}}(ptr %{{.*}}, ptr %{{.*}})
// CHECK-NOT: call
// CHECK: call void @{{_ZN10NonTrivialD(1|2)Ev|"\?\?1NonTrivial@@QEAA@XZ"}}(ptr %{{.*}})
// CHECK-NOT: call
// CHECK: ret ptr

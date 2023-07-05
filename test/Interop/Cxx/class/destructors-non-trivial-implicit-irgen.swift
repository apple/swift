// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir | %FileCheck %s

import Destructors

// CHECK-LABEL: define {{.*}}void @"$s4main35testHasNonTrivialImplicitDestructoryyF"
// CHECK:  call {{.*}}@{{_ZN31HasNonTrivialImplicitDestructorD(1|2)Ev|"\?\?1HasNonTrivialImplicitDestructor@@QEAA@XZ"}}(ptr
// CHECK: ret void

// TODO: Somehow check that _ZN31HasNonTrivialImplicitDestructorD1Ev (if present) calls _ZN25HasUserProvidedDestructorD2Ev.

public func testHasNonTrivialImplicitDestructor() {
  _ = HasNonTrivialImplicitDestructor()
}

// Check that we call the base destructor.
// CHECK-LABEL: define {{.*}}@{{_ZN31HasNonTrivialImplicitDestructorD2Ev|"\?\?1HasNonTrivialImplicitDestructor@@QEAA@XZ"}}(ptr
// CHECK: call {{.*}}@{{_ZN25HasUserProvidedDestructorD(1|2)Ev|"\?\?1HasUserProvidedDestructor@@QEAA@XZ"}}
// CHECK: ret

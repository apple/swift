// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir | %FileCheck %s

// This tests output needs to be updated for arm64.
// XFAIL: CPU=arm64e

import CustomDestructor

protocol InitWithDummy {
  init(dummy: DummyStruct)
}

extension HasUserProvidedDestructorAndDummy : InitWithDummy { }

// Make sure the destructor is added as a witness.
// CHECK: @"$sSo33HasUserProvidedDestructorAndDummyVWV" = linkonce_odr hidden constant %swift.vwtable
// CHECK-SAME: ptr @"$sSo33HasUserProvidedDestructorAndDummyVwxx"

// CHECK-LABEL: define {{.*}}void @"$s4main37testHasUserProvidedDestructorAndDummyyyF"
// CHECK: [[OBJ:%.*]] = alloca %TSo33HasUserProvidedDestructorAndDummyV
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(ptr [[OBJ]])
// CHECK: ret void

// Make sure we not only declare but define the destructor.
// CHECK-LABEL: define {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}
// CHECK: ret
public func testHasUserProvidedDestructorAndDummy() {
  _ = HasUserProvidedDestructorAndDummy(dummy: DummyStruct())
}

// CHECK-LABEL: define {{.*}}void @"$s4main26testHasDefaultedDestructoryyF"
// CHECK: call {{.*}}@{{_ZN22HasDefaultedDestructorC(1|2)Ev|"\?\?0HasDefaultedDestructor@@QEAA@XZ"}}(ptr
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@{{_ZN22HasDefaultedDestructorC(1|2)Ev|"\?\?0HasDefaultedDestructor@@QEAA@XZ"}}(ptr
// CHECK: ret
public func testHasDefaultedDestructor() {
  _ = HasDefaultedDestructor()
}

// Make sure the destroy value witness calls the destructor.
// CHECK-LABEL: define {{.*}}void @"$sSo33HasUserProvidedDestructorAndDummyVwxx"
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(ptr
// CHECK: ret

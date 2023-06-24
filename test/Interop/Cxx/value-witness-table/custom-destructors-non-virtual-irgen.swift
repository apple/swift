// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir

// This tests output needs to be updated for arm64.
// XFAIL: CPU=arm64e

import CustomDestructor

protocol InitWithDummy {
  init(dummy: DummyStruct)
}

extension HasUserProvidedDestructorAndDummy : InitWithDummy { }

// Make sure the destructor is added as a witness.
// CHECK: @"$sSo33HasUserProvidedDestructorAndDummyVWV" = linkonce_odr hidden constant %swift.vwtable
// CHECK-SAME: i8* bitcast (void (%swift.opaque*, %swift.type*)* @"$sSo33HasUserProvidedDestructorAndDummyVwxx" to i8*)

// CHECK-LABEL: define {{.*}}void @"$s4main37testHasUserProvidedDestructorAndDummyyyF"
// CHECK: [[OBJ:%.*]] = alloca %TSo33HasUserProvidedDestructorAndDummyV
// CHECK: [[CXX_OBJ:%.*]] = bitcast %TSo33HasUserProvidedDestructorAndDummyV* [[OBJ]] to %struct.HasUserProvidedDestructorAndDummy*
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(%struct.HasUserProvidedDestructorAndDummy* [[CXX_OBJ]])
// CHECK: ret void

// Make sure we not only declare but define the destructor.
// CHECK-LABEL: define {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}
// CHECK: ret
public func testHasUserProvidedDestructorAndDummy() {
  _ = HasUserProvidedDestructorAndDummy(dummy: DummyStruct())
}

// CHECK-LABEL: define {{.*}}void @"$s4main26testHasDefaultedDestructoryyF"
// CHECK: call {{.*}}@{{_ZN22HasDefaultedDestructorC(1|2)Ev|"\?\?0HasDefaultedDestructor@@QEAA@XZ"}}(%struct.HasDefaultedDestructor*
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@{{_ZN22HasDefaultedDestructorC(1|2)Ev|"\?\?0HasDefaultedDestructor@@QEAA@XZ"}}(%struct.HasDefaultedDestructor*
// CHECK: ret
public func testHasDefaultedDestructor() {
  _ = HasDefaultedDestructor()
}

// Make sure the destroy value witness calls the destructor.
// CHECK-LABEL: define {{.*}}void @"$sSo33HasUserProvidedDestructorAndDummyVwxx"
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(%struct.HasUserProvidedDestructorAndDummy*
// CHECK: ret

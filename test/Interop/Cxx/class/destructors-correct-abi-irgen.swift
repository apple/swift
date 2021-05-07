// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import Destructors

// CHECK-LABEL: define {{.*}}void @"$s4main4testyyF"
// CHECK: [[H:%.*]] = alloca %TSo33HasUserProvidedDestructorAndDummyV
// CHECK: [[CXX_THIS:%.*]] = bitcast %TSo33HasUserProvidedDestructorAndDummyV* [[H]] to %struct.HasUserProvidedDestructorAndDummy*
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(%struct.HasUserProvidedDestructorAndDummy* [[CXX_THIS]])
// CHECK: ret void
public func test() {
  let d = DummyStruct()
  let h = HasUserProvidedDestructorAndDummy(dummy: d)
}

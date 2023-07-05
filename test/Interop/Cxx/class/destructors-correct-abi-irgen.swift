// RUN: %swift -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s | %FileCheck %s

import Destructors

// CHECK-LABEL: define {{.*}}void @"$s4main4testyyF"
// CHECK: [[H:%.*]] = alloca %TSo33HasUserProvidedDestructorAndDummyV
// CHECK: call {{.*}}@{{_ZN33HasUserProvidedDestructorAndDummyD(1|2)Ev|"\?\?1HasUserProvidedDestructorAndDummy@@QEAA@XZ"}}(ptr [[H]])
// CHECK: ret void
public func test() {
  let d = DummyStruct()
  let h = HasUserProvidedDestructorAndDummy(dummy: d)
}

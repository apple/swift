// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop

import ConstructorCallsMethod

public func getIncrementorValue() -> CInt {
  return callConstructor(41)
}

// CHECK: define {{.*}}i32 @{{_ZN11Incrementor9incrementEi|"\?increment@Incrementor@@QEAAHH@Z"}}(%struct.Incrementor* {{.*}}, i32 {{.*}})

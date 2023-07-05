// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s

import ConstructorCallsMethod

public func getIncrementorValue() -> CInt {
  return callConstructor(41)
}

// CHECK: define {{.*}}i32 @{{_ZN11Incrementor9incrementEi|"\?increment@Incrementor@@QEAAHH@Z"}}(ptr {{.*}}, i32 {{.*}})

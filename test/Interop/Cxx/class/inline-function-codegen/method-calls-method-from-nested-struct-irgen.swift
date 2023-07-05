// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s

import MethodCallsMethodFromNestedStruct

public func getValueFromMethod() -> CInt {
  return callMethod(41)
}

// CHECK: define {{.*}}i32 @{{_ZN13IncrementUser11Incrementor9incrementEi|"\?increment@Incrementor@IncrementUser@@QEAAHH@Z"}}(ptr {{.*}}, i32 {{.*}})

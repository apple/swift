// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop

import MethodCallsMethodFromNestedStruct

public func getValueFromMethod() -> CInt {
  return callMethod(41)
}

// CHECK: define {{.*}}i32 @{{_ZN13IncrementUser11Incrementor9incrementEi|"\?increment@Incrementor@IncrementUser@@QEAAHH@Z"}}(%"struct.IncrementUser::Incrementor"* {{.*}}, i32 {{.*}})

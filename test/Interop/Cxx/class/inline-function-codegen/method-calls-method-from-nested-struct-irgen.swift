// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import MethodCallsMethodFromNestedStruct

public func getValueFromMethod() -> CInt {
  return callMethod(41)
}

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_ZN13IncrementUser11Incrementor9incrementEi|"\?increment@Incrementor@IncrementUser@@QEAAHH@Z"}}(%"struct.IncrementUser::Incrementor"* nonnull dereferenceable(1) %this, i32 %t)

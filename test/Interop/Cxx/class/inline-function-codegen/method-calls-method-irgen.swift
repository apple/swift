// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import MethodCallsMethod

public func getValueFromMethod() -> CInt {
  return callMethod(41)
}

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_ZN11Incrementor9incrementEi|"\?increment@Incrementor@@QEAAHH@Z"}}(%struct.Incrementor* nonnull dereferenceable(1) %this, i32 %t)

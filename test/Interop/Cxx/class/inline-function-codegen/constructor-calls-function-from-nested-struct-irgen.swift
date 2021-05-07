// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import ConstructorCallsFunctionFromNestedStruct

public func getIncrementorValue() -> CInt {
  return callConstructor(41)
}

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_Z9incrementi|"\?increment@@YAHH@Z"}}(i32 %t)

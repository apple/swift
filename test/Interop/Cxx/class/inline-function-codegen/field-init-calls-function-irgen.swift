// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import FieldInitCallsFunction

public func getInitializedField() -> CInt {
  return initializeField()
}

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_Z9incrementi|"\?increment@@YAHH@Z"}}(i32 %t)

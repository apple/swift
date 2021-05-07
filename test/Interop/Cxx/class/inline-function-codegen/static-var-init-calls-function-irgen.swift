// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir -validate-tbd-against-ir=none | %FileCheck %s)

// TODO: See why -validate-tbd-against-ir=none is needed here (SR-14069)

import StaticVarInitCallsFunction

public func getInitializedStaticVar() -> CInt {
  return initializeStaticVar()
}

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_Z9incrementi|"\?increment@@YAHH@Z"}}(i32 %t)

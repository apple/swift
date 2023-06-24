// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -module-name=test -disable-availability-checking  | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -module-name=test -disable-availability-checking

import MemberLayout

// CHECK: %TSo7IntBaseV = type <{ [4 x i8], %Ts5Int32V }>

// CHECK-LABEL: define {{.*}}swiftcc i32 @"$s4testAA1ys5Int32VSo7IntBaseV_tF"(%TSo7IntBaseV* %0)
// CHECK: getelementptr inbounds %TSo7IntBaseV, %TSo7IntBaseV* %0, i32 0, i32 1
public func test(y: IntBase) -> CInt {
  return y.i
}

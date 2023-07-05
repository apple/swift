// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -module-name=test -disable-availability-checking | %FileCheck %s

import MemberLayout

// Make sure the "refs" and "values" look *exactly* the same.

// CHECK: %TSo10IntCharRefV = type <{ %Ts5Int32V, %Ts4Int8V }>
// CHECK: %TSo12IntCharValueV = type <{ %Ts5Int32V, %Ts4Int8V }>

// CHECK: %TSo21UnimportableMemberRefV = type <{ %Ts5Int32V, %Ts5Int32V, [8 x i8], %Ts5Int32V }>
// CHECK: %TSo23UnimportableMemberValueV = type <{ %Ts5Int32V, %Ts5Int32V, [8 x i8], %Ts5Int32V }>

// CHECK-LABEL: define {{.*}}swiftcc i8 @"$s4testAA1ys4Int8VSo10IntCharRefV_tF"(ptr %0)
// CHECK: getelementptr inbounds %TSo10IntCharRefV, ptr %0, i32 0, i32 1
public func test(y: IntCharRef) -> CChar {
  return y.b
}

// CHECK-LABEL: define {{.*}}swiftcc i8 @"$s4testAA1ys4Int8VSo12IntCharValueV_tF"(ptr %0)
// CHECK: getelementptr inbounds %TSo12IntCharValueV, ptr %0, i32 0, i32 1
public func test(y: IntCharValue) -> CChar {
  return y.b
}

// CHECK-LABEL: define {{.*}}swiftcc i32 @"$s4testAA1ys5Int32VSo21UnimportableMemberRefV_tF"(ptr %0)
// CHECK: getelementptr inbounds %TSo21UnimportableMemberRefV, ptr %0, i32 0, i32 3
public func test(y: UnimportableMemberRef) -> CInt {
  return y.y
}

// CHECK-LABEL: define {{.*}}swiftcc i32 @"$s4testAA1ys5Int32VSo23UnimportableMemberValueV_tF"(ptr %0)
// CHECK: getelementptr inbounds %TSo23UnimportableMemberValueV, ptr %0, i32 0, i32 3
public func test(y: UnimportableMemberValue) -> CInt {
  return y.y
}


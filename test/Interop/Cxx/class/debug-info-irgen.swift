// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir -g | %FileCheck %s)

// Validate that we don't crash when trying to deserialize C++ type debug info.
// Note, however, that the actual debug info is not generated, see SR-13223.

import DebugInfo

public func create(_ x: Int32) -> IntWrapper { IntWrapper(value: x) }

public func getInt() -> Int32 { 0 }

// CHECK-LABEL: define {{.*}}void @"$s4main4testyyF"
// CHECK: [[I:%.*]] = call swiftcc i32 @"$s4main6getInts5Int32VyF"()
// CHECK: call swiftcc i32 @"$s4main6createySo10IntWrapperVs5Int32VF"(i32 [[I]])
// CHECK: ret void
public func test() {
  let f = create(getInt())
}


// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Onone | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -O | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Osize | %FileCheck %s

// REQUIRES: volatile

import _Volatile

public func test_volatilepointer() {
  let p = VolatileMappedRegister<UInt8>(unsafeBitPattern: 0)
  p.store(42)
}

// CHECK: define {{.*}}void @"$s4main20test_volatilepointeryyF"()
// CHECK:   store atomic volatile i8 42, ptr null monotonic, align {{.*}}
// CHECK:   ret void
// CHECK: }

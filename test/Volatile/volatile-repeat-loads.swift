// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Onone | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -O | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Osize | %FileCheck %s

// REQUIRES: volatile

import _Volatile

public func test_volatilepointer() -> UInt8 {
  let p = VolatileMappedRegister<UInt8>(unsafeBitPattern: 0xf000baa9)
  p.store(42)
  let a = p.load()
  let b = p.load()
  let c = p.load()
  return c
}

// CHECK: define {{.*}}i8 @"$s4main20test_volatilepointers5UInt8VyF"()
// CHECK:   store atomic volatile i8 42, ptr {{.*}} monotonic, align 1
// CHECK:   load atomic volatile i8, ptr {{.*}} monotonic, align 1
// CHECK:   load atomic volatile i8, ptr {{.*}} monotonic, align 1
// CHECK:   [[RET:%.*]] = load atomic volatile i8, ptr {{.*}} monotonic, align 1
// CHECK:   ret i8 [[RET]]
// CHECK: }

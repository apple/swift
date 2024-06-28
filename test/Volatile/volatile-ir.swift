// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Onone | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -O | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile -Osize | %FileCheck %s

// REQUIRES: volatile

import _Volatile

public func test_uint8() -> UInt8 {
  let p = VolatileMappedRegister<UInt8>(unsafeBitPattern: 0xf000baa9)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i8 @"$s4main10test_uint8s5UInt8VyF"()
// CHECK:   store atomic volatile i8 42, ptr {{.*}} monotonic, align 1
// CHECK:   [[RET:%.*]] = load atomic volatile i8, ptr {{.*}} monotonic, align 1
// CHECK:   ret i8 [[RET]]
// CHECK: }

public func test_uint16() -> UInt16 {
  let p = VolatileMappedRegister<UInt16>(unsafeBitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i16 @"$s4main11test_uint16s6UInt16VyF"()
// CHECK:   store atomic volatile i16 42, ptr {{.*}} monotonic, align 2
// CHECK:   [[RET:%.*]] = load atomic volatile i16, ptr {{.*}} monotonic, align 2
// CHECK:   ret i16 [[RET]]
// CHECK: }

public func test_uint32() -> UInt32 {
  let p = VolatileMappedRegister<UInt32>(unsafeBitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i32 @"$s4main11test_uint32s6UInt32VyF"()
// CHECK:   store atomic volatile i32 42, ptr {{.*}} monotonic, align 4
// CHECK:   [[RET:%.*]] = load atomic volatile i32, ptr {{.*}} monotonic, align 4
// CHECK:   ret i32 [[RET]]
// CHECK: }

public func test_uint64() -> UInt64 {
  let p = VolatileMappedRegister<UInt64>(unsafeBitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i64 @"$s4main11test_uint64s6UInt64VyF"()
// CHECK:   store atomic volatile i64 42, ptr {{.*}} monotonic, align 8
// CHECK:   [[RET:%.*]] = load atomic volatile i64, ptr {{.*}} monotonic, align 8
// CHECK:   ret i64 [[RET]]
// CHECK: }

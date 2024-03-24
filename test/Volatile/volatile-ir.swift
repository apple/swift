// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -Onone | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -O | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -Osize | %FileCheck %s

import _Volatile

public func test_uint8() -> UInt8 {
  let p = UnsafeVolatilePointer<UInt8>(bitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i8 @"$s4main10test_uint8s5UInt8VyF"()
// CHECK:   store atomic volatile i8 42, ptr %3 monotonic, align 1
// CHECK:   [[RET:%.*]] = load atomic volatile i8, ptr %4 monotonic, align 1
// CHECK:   ret i8 [[RET]]
// CHECK: }

public func test_uint16() -> UInt16 {
  let p = UnsafeVolatilePointer<UInt16>(bitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i16 @"$s4main11test_uint16s6UInt16VyF"()
// CHECK:   store atomic volatile i16 42, ptr %3 monotonic, align 2
// CHECK:   [[RET:%.*]] = load atomic volatile i16, ptr %4 monotonic, align 2
// CHECK:   ret i16 [[RET]]
// CHECK: }

public func test_uint32() -> UInt32 {
  let p = UnsafeVolatilePointer<UInt32>(bitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i32 @"$s4main11test_uint32s6UInt32VyF"()
// CHECK:   store atomic volatile i32 42, ptr %3 monotonic, align 4
// CHECK:   [[RET:%.*]] = load atomic volatile i32, ptr %4 monotonic, align 4
// CHECK:   ret i32 [[RET]]
// CHECK: }

public func test_uint64() -> UInt64 {
  let p = UnsafeVolatilePointer<UInt64>(bitPattern: 0xf000baaa)
  p.store(42)
  return p.load()
}

// CHECK: define {{.*}}i64 @"$s4main11test_uint64s6UInt64VyF"()
// CHECK:   store atomic volatile i64 42, ptr %3 monotonic, align 8
// CHECK:   [[RET:%.*]] = load atomic volatile i64, ptr %4 monotonic, align 8
// CHECK:   ret i64 [[RET]]
// CHECK: }

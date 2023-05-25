// RUN: %target-swift-frontend -emit-sil -O %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-sil -Osize %s | %FileCheck %s

// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler

// This is an end-to-end test if the count and/or capacity from empty
// array/set/dictionary singletons can be propagated.

// CHECK-LABEL: sil @{{.*}}testArray
// CHECK-NOT: global_addr
// CHECK: [[Z:%[0-9]+]] = integer_literal $Builtin.Int{{[0-9]*}}, 0 
// CHECK: [[I:%[0-9]+]] = struct $Int ([[Z]] : $Builtin.Int{{[0-9]*}})
// CHECK: return [[I]]
public func testArray() -> Int {
  let d = Array<Int>()
  return d.count + d.capacity
}

// CHECK-LABEL: sil @{{.*}}testDictionary
// CHECK-NOT: global_addr
// CHECK: [[Z:%[0-9]+]] = integer_literal $Builtin.Int{{[0-9]*}}, 0 
// CHECK: [[I:%[0-9]+]] = struct $Int ([[Z]] : $Builtin.Int{{[0-9]*}})
// CHECK: return [[I]]
public func testDictionary() -> Int {
  let d = Dictionary<Int, Int>()
  return d.count + d.capacity
}

// CHECK-LABEL: sil @{{.*}}testSet
// CHECK-NOT: global_addr
// CHECK: [[Z:%[0-9]+]] = integer_literal $Builtin.Int{{[0-9]*}}, 0 
// CHECK: [[I:%[0-9]+]] = struct $Int ([[Z]] : $Builtin.Int{{[0-9]*}})
// CHECK: return [[I]]
public func testSet() -> Int {
  let d = Set<Int>()
  return d.count + d.capacity
}


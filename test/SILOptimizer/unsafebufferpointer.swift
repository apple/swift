// RUN: %target-swift-frontend -parse-as-library -Osize -emit-ir  %s | %FileCheck %s
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler

// This is an end-to-end test to ensure that the optimizer generates
// optimal code for UnsafeBufferPointer.

// CHECK-LABEL: define {{.*}}testIteration

// Check if the code contains no traps at all.
// CHECK-NOT: unreachable
public func testIteration(_ p: UnsafeBufferPointer<Int>) -> Int {
  var s = 0

// Check for an optimal loop kernel
// CHECK:       phi
// CHECK-NEXT:  phi
// CHECK-NEXT:  getelementptr
// CHECK-NEXT:  load
// CHECK-NEXT:  add
// CHECK-NEXT:  icmp
// CHECK-NEXT:  br
  for x in p {
    s = s &+ x
  }
// CHECK-NOT: unreachable
// CHECK:       phi
// CHECK-NEXT:  ret
// CHECK-NOT: unreachable
  return s
}

// CHECK-LABEL: define {{.*}}testIsEmpty
// CHECK:      entry:
// CHECK-NEXT:   icmp
// CHECK-NEXT:   ret
public func testIsEmpty(_ x: UnsafeBufferPointer<UInt>) -> Bool {
  return x.isEmpty
}

// CHECK-LABEL: define {{.*}}testCount
// CHECK:      entry:
// CHECK-NEXT:   ret
public func testCount(_ x: UnsafeBufferPointer<UInt>) -> Int {
  return x.count
}

// Within the loop, there should be no extra checks.
// CHECK-LABEL: define {{.*}} float {{.*}}testSubscript
// The only unconditional branch is into the loop.
// CHECK: br label %[[LOOP:[0-9]+]]
//
// CHECK: [[LOOP]]:
// CHECK: phi float [ 0.000000e+00
// CHECK: load float, ptr
// CHECK: fadd float
// CHECK: [[CMP:%[0-9]+]] = icmp eq
// CHECK: br i1 [[CMP]], label %.loopexit, label %[[LOOP]]
//
// CHECK: .loopexit: ; preds = %[[LOOP]]
// CHECK: ret float
public func testSubscript(_ ubp: UnsafeBufferPointer<Float>) -> Float {
  var sum: Float = 0
  for i in 0 ..< ubp.count {
    sum += ubp[i]
  }
  return sum
}

// Within the loop, there should be no extra checks.
// CHECK-LABEL: define {{.*}} i64 {{.*}}testSubscript
// The only unconditional branch is into the loop.
// CHECK: br label %[[LOOP:[0-9]+]]
//
// CHECK: [[LOOP]]:
// CHECK: phi i64 [ 0
// CHECK: load i8, ptr
// CHECK: zext i8 %{{.*}} to i64
// CHECK: add i64
// CHECK: [[CMP:%[0-9]+]] = icmp eq
// CHECK: br i1 [[CMP]], label %[[RET:.*]], label %[[LOOP]]
//
// CHECK: [[RET]]: ; preds = %[[LOOP]], %{{.*}}
// CHECK: ret i64
public func testSubscript(_ ubp: UnsafeRawBufferPointer) -> Int64 {
  var sum: Int64 = 0
  for i in 0 ..< ubp.count {
    sum &+= Int64(ubp[i])
  }
  return sum
}

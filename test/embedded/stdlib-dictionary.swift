// RUN: %target-swift-frontend -target armv7-apple-none-macho -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib

public func test() {
  var d: [Int:Int] = [1: 2, 3: 4, 5: 6]
  d[8] = 9
  d.keys.sorted()
  d.values.allSatisfy { $0 > 0 }
  d.keys.contains { $0 > 0 }
  d.values.map { $0 * 2 }
}

test()

// CHECK: define {{.*}}i32 @main(i32 %0, ptr %1)

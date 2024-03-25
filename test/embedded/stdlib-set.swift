// RUN: %target-swift-frontend -target armv7-apple-none-macho -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: CODEGENERATOR=ARM

public func test() {
  var s: Set<Int> = [1, 2, 3]
  s.insert(42)
  s.sorted()
  s.allSatisfy { $0 > 0 }
  s.contains { $0 > 0 }
  s.map { $0 * 2 }
  s.filter { $0 > 0 }
  s.firstIndex(of: 42)
  s.min()
  s.max()
  s.reduce(0, +)
  s.shuffled()
  s.randomElement()
}

test()

// CHECK: define {{.*}}i32 @main(i32 %0, ptr %1)

// RUN: %target-swift-frontend -target armv7-apple-none-macho -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

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

// RUN: %target-swift-frontend -target armv7-apple-none-macho -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

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

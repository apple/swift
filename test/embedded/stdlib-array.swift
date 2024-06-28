// RUN: %target-swift-frontend -target armv7-apple-none-macho -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

public func test() {
  var array: [Int] = [1, 2, 3]
  array.append(42)
  array.append(contentsOf: [4, 5, 6])
  array = array.sorted()
  array.sort()
  array.allSatisfy { $0 > 0 }
  array.contains { $0 > 0 }
  array.map { $0 * 2 }
  array.filter { $0 > 0 }
  array.dropFirst().dropLast().drop(while: { $0 < 0 })
  array.firstIndex(of: 42)
  array.min()
  array.max()
  array.partition(by: { $0 > 0 })
  array.reduce(0, +)
  array.shuffle()
  array = array.shuffled()
  array.randomElement()
}

test()

// CHECK: define {{.*}}i32 @main(i32 %0, ptr %1)

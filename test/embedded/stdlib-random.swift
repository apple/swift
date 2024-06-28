// RUN: %target-swift-frontend -target armv7-apple-none-macho -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

public func test() {
  Bool.random()
  Int.random(in: 0 ..< 100)
  Double.random(in: 0.0 ..< 1.0)
  [1, 2, 3].shuffled()
  [1, 2, 3].randomElement()
}

test()

// CHECK: define {{.*}}i32 @main(i32 %0, ptr %1)

// Regression test for a "Ouroboros Bug": The ARC optimizer doesn't like the
// presense of a direct call to swift_retain and swift_release in any Swift
// code, but in the embedded Swift's runtime that's somewhat reasonable thing
// to do (but is to be avoided because of this).

// RUN: %target-swift-frontend -target armv7-apple-none-macho -assert-config Debug -Osize -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -assert-config Debug -Osize -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

public func test() {}
test()

// CHECK: define {{.*}}i32 @main

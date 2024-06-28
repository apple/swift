// RUN: %target-swift-emit-ir %s -enable-experimental-feature Embedded -no-allocations

// RUN: %target-swift-emit-ir -target armv7-apple-none-macho -no-allocations -Xcc -D__MACH__ %s -enable-experimental-feature Embedded
// RUN: %target-swift-emit-ir -target arm64-apple-none-macho -no-allocations -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ %s -enable-experimental-feature Embedded

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

print("Hello Embedded Swift!")
print(42)
print(false)

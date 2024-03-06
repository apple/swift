// RUN: %empty-directory(%t)
// RUN: %target-run-simple-swift(-enable-experimental-feature NoncopyableGenerics) | %FileCheck %s
// REQUIRES: executable_test, noncopyable_generics

struct A: ~Copyable {
  let value: Int

  init(_ value: Int) { self.value = value }
}

let expectedSize = MemoryLayout<Int>.size
let expectedStride = MemoryLayout<Int>.stride
let expectedAlignment = MemoryLayout<Int>.alignment


let actualSize1 = MemoryLayout<A>.size
// CHECK: size: true
print("size: \(actualSize1 == expectedSize)")

let actualStride1 = MemoryLayout<A>.stride
// CHECK: stride: true
print("stride: \(actualStride1 == expectedStride)")

let actualAlignment1 = MemoryLayout<A>.alignment
// CHECK: alignment: true
print("alignment: \(actualAlignment1 == expectedAlignment)")

let a = A(42)

let actualSize2 = MemoryLayout.size(ofValue: a)
// CHECK: size(ofValue:): true
print("size(ofValue:): \(actualSize2 == expectedSize)")

let actualStride2 = MemoryLayout.stride(ofValue: a)
// CHECK: stride(ofValue:): true
print("stride(ofValue:): \(actualStride2 == expectedStride)")

let actualAlignment2 = MemoryLayout.alignment(ofValue: a)
// CHECK: alignment(ofValue:): true
print("alignment(ofValue:): \(actualAlignment2 == expectedAlignment)")

// RUN: %empty-directory(%t)
// RUN: %target-run-simple-swift(-enable-experimental-feature NoncopyableGenerics -enable-experimental-feature NonescapableTypes -Xfrontend -enable-experimental-lifetime-dependence-inference -Xfrontend -disable-round-trip-debug-types -Xfrontend -enable-experimental-associated-type-inference -Xfrontend -disable-experimental-parser-round-trip) | %FileCheck %s
// REQUIRES: executable_test, noncopyable_generics

struct A: ~Copyable {
  var value: Int

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

var a = A(42)

let actualSize2 = MemoryLayout.size(ofValue: a)
// CHECK: size(ofValue:): true
print("size(ofValue:): \(actualSize2 == expectedSize)")

let actualStride2 = MemoryLayout.stride(ofValue: a)
// CHECK: stride(ofValue:): true
print("stride(ofValue:): \(actualStride2 == expectedStride)")

let actualAlignment2 = MemoryLayout.alignment(ofValue: a)
// CHECK: alignment(ofValue:): true
print("alignment(ofValue:): \(actualAlignment2 == expectedAlignment)")


struct B: ~Escapable {
  let value: Int

  init(_ borrowed: borrowing A) -> _borrow(borrowed) Self {
    self.value = borrowed.value
    return self
  }
}

let actualSize3 = MemoryLayout<B>.size
// CHECK: size: true
print("size: \(actualSize3 == expectedSize)")

let actualStride3 = MemoryLayout<B>.stride
// CHECK: stride: true
print("stride: \(actualStride3 == expectedStride)")

let actualAlignment3 = MemoryLayout<B>.alignment
// CHECK: alignment: true
print("alignment: \(actualAlignment3 == expectedAlignment)")

extension A {
  var b: B {
    borrowing _read {
      yield B(self)
    }
  }
}

let b = a.b

let actualSize4 = MemoryLayout.size(ofValue: b)
// CHECK: size(ofValue:): true
print("size(ofValue:): \(actualSize4 == expectedSize)")

let actualStride4 = MemoryLayout.stride(ofValue: b)
// CHECK: stride(ofValue:): true
print("stride(ofValue:): \(actualStride4 == expectedStride)")

let actualAlignment4 = MemoryLayout.alignment(ofValue: b)
// CHECK: alignment(ofValue:): true
print("alignment(ofValue:): \(actualAlignment4 == expectedAlignment)")

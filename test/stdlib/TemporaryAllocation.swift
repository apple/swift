// RUN: %target-run-simple-swiftgyb
// REQUIRES: executable_test

import StdlibUnittest
import SwiftShims

var TemporaryAllocationTestSuite = TestSuite("TemporaryAllocation")

func isStackAllocated(_ pointer: UnsafeRawPointer) -> Bool? {
    var stackBegin: UInt = 0
    var stackEnd: UInt = 0
    if _swift_stdlib_getCurrentStackBounds(&stackBegin, &stackEnd) {
        var pointerValue = UInt(bitPattern: pointer)
        return pointerValue >= stackBegin && pointerValue < stackEnd
    }
    return nil
}

func expectStackAllocated(_ pointer: UnsafeRawPointer) {
    if let stackAllocated = isStackAllocated(pointer) {
        expectTrue(stackAllocated)
    } else {
        // Could not read stack bounds. Skip.
    }
}

func expectNotStackAllocated(_ pointer: UnsafeRawPointer) {
    if let stackAllocated = isStackAllocated(pointer) {
        expectFalse(stackAllocated)
    } else {
        // Could not read stack bounds. Skip.
    }
}

// MARK: Untyped buffers

TemporaryAllocationTestSuite.test("untypedAllocationOnStack") {
  withUnsafeTemporaryAllocation(byteCount: 8, alignment: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("untypedAllocationOnHeap") {
  // EXPECTATION: a very large allocated buffer is heap-allocated. (Note if
  // swift_stdlib_isStackAllocationSafe() gets fleshed out, this test may need
  // to be changed.)
  withUnsafeTemporaryAllocation(byteCount: 100_000, alignment: 1) { buffer in
      expectNotStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("unprotectedUntypedAllocationOnStack") {
  _withUnprotectedUnsafeTemporaryAllocation(byteCount: 8, alignment: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("unprotectedUntypedAllocationOnHeap") {
  // EXPECTATION: a very large allocated buffer is heap-allocated. (Note if
  // swift_stdlib_isStackAllocationSafe() gets fleshed out, this test may need
  // to be changed.)
  _withUnprotectedUnsafeTemporaryAllocation(byteCount: 100_000, alignment: 1) { buffer in
      expectNotStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("untypedEmptyAllocationIsStackAllocated") {
  withUnsafeTemporaryAllocation(byteCount: 0, alignment: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

#if !os(WASI)
TemporaryAllocationTestSuite.test("crashOnNegativeByteCount") {
  expectCrash {
    let byteCount = Int.random(in: -2 ..< -1)
    withUnsafeTemporaryAllocation(byteCount: byteCount, alignment: 1) { _ in }
  }
}

TemporaryAllocationTestSuite.test("crashOnNegativeAlignment") {
  expectCrash {
    let alignment = Int.random(in: -2 ..< -1)
    withUnsafeTemporaryAllocation(byteCount: 16, alignment: alignment) { _ in }
  }
}
#endif

TemporaryAllocationTestSuite.test("untypedAllocationIsAligned") {
  withUnsafeTemporaryAllocation(byteCount: 1, alignment: 8) { buffer in
    let pointerBits = Int(bitPattern: buffer.baseAddress!)
    let alignmentMask = 0b111
    expectEqual(pointerBits & alignmentMask, 0)
  }
}

// MARK: Typed buffers

TemporaryAllocationTestSuite.test("typedAllocationOnStack") {
  withUnsafeTemporaryAllocation(of: Int.self, capacity: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("typedAllocationOnHeap") {
  // EXPECTATION: a very large allocated buffer is heap-allocated. (Note if
  // swift_stdlib_isStackAllocationSafe() gets fleshed out, this test may need
  // to be changed.)
  withUnsafeTemporaryAllocation(of: Int.self, capacity: 100_000) { buffer in
      expectNotStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("unprotectedTypedAllocationOnStack") {
  _withUnprotectedUnsafeTemporaryAllocation(of: Int.self, capacity: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("unprotectedTypedAllocationOnHeap") {
  // EXPECTATION: a very large allocated buffer is heap-allocated. (Note if
  // swift_stdlib_isStackAllocationSafe() gets fleshed out, this test may need
  // to be changed.)
  _withUnprotectedUnsafeTemporaryAllocation(of: Int.self, capacity: 100_000) { buffer in
      expectNotStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("typedEmptyAllocationIsStackAllocated") {
  withUnsafeTemporaryAllocation(of: Int.self, capacity: 0) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

TemporaryAllocationTestSuite.test("voidAllocationIsStackAllocated") {
  withUnsafeTemporaryAllocation(of: Void.self, capacity: 1) { buffer in
      expectStackAllocated(buffer.baseAddress!)
  }
}

#if !os(WASI)
TemporaryAllocationTestSuite.test("crashOnNegativeValueCount") {
  expectCrash {
    let capacity = Int.random(in: -2 ..< -1)
    withUnsafeTemporaryAllocation(of: Int.self, capacity: capacity) { _ in }
  }
}
#endif

TemporaryAllocationTestSuite.test("typedAllocationIsAligned") {
  withUnsafeTemporaryAllocation(of: Int.self, capacity: 1) { buffer in
    let pointerBits = Int(bitPattern: buffer.baseAddress!)
    let alignmentMask = MemoryLayout<Int>.alignment - 1
    expectEqual(pointerBits & alignmentMask, 0)
  }
}

runAllTests()

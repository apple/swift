// RUN: %target-run-simple-swiftgyb
// REQUIRES: executable_test
//
// Test corner cases specific to UnsafeRawBufferPointer.
// General Collection behavior tests are in
// validation-test/stdlib/UnsafeBufferPointer.swift.

// FIXME: The optimized-build behavior of UnsafeBufferPointer bounds/overflow
// checking cannot be tested. The standard library always compiles with debug
// checking enabled, so the behavior of the optimized test depends on whether
// the inlining heuristics decide to inline these methods. To fix this, we need
// a way to force @inlinable UnsafeBufferPointer methods to be emitted inside
// the client code, and thereby subject the stdlib implementation to the test
// case's compile options.
//
// REQUIRES: swift_test_mode_optimize_none

import StdlibUnittest

var UnsafeRawBufferPointerTestSuite = TestSuite("UnsafeRawBufferPointer")

// View an in-memory value's bytes.
// Use copyBytes to overwrite the value's bytes.
UnsafeRawBufferPointerTestSuite.test("initFromValue") {
  var value1: Int32 = -1
  var value2: Int32 = 0
  // Immutable view of value1's bytes.
  withUnsafeBytes(of: &value1) { bytes1 in
    expectEqual(bytes1.count, 4)
    for b in bytes1 {
      expectEqual(b, 0xFF)
    }
    // Mutable view of value2's bytes.
    withUnsafeMutableBytes(of: &value2) { bytes2 in
      expectEqual(bytes1.count, bytes2.count)
      bytes2.copyMemory(from: bytes1)
    }
  }
  expectEqual(value2, value1)
}

UnsafeRawBufferPointerTestSuite.test("initFromNilBuffer") {
  let urbp1 =
    UnsafeRawBufferPointer(UnsafeBufferPointer<Int>(start: nil, count: 0))
  expectEqual(urbp1.baseAddress, nil)

  let urbp2 =
    UnsafeRawBufferPointer(UnsafeMutableBufferPointer<Int>(start: nil, count: 0))
  expectEqual(urbp2.baseAddress, nil)

  let umrbp =
    UnsafeMutableRawBufferPointer(
      UnsafeMutableBufferPointer<Int>(start: nil, count: 0))
  expectEqual(umrbp.baseAddress, nil)
}

UnsafeRawBufferPointerTestSuite.test("initFromNilSlice") {
  let urbp1 =
    UnsafeRawBufferPointer(
      rebasing: UnsafeRawBufferPointer(start: nil, count: 0)[...])
  expectEqual(urbp1.baseAddress, nil)

  let urbp2 =
    UnsafeRawBufferPointer(
      rebasing: UnsafeMutableRawBufferPointer(start: nil, count: 0)[...])
  expectEqual(urbp2.baseAddress, nil)

  let umrbp =
    UnsafeMutableRawBufferPointer(
      rebasing: UnsafeMutableRawBufferPointer(start: nil, count: 0)[...])
  expectEqual(umrbp.baseAddress, nil)
}

// Test mutability and subscript getter/setters.
UnsafeRawBufferPointerTestSuite.test("nonmutating_subscript_setter") {
  var value1: Int32 = -1
  var value2: Int32 = 0

  withUnsafeMutableBytes(of: &value1) { bytes1 in
    withUnsafeMutableBytes(of: &value2) { bytes2 in
      bytes2[0..<bytes2.count] = bytes1[0..<bytes1.count]
    }
  }
  expectEqual(value2, value1)

  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 4, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }
  buffer.copyBytes(from: [0, 1, 2, 3] as [UInt8])
  let leftBytes = buffer[0..<2]

  // Subscript assign.
  var rightBytes = buffer[2..<4]
  buffer[2..<4] = leftBytes
  expectEqualSequence(leftBytes, rightBytes)
  
  // Subscript assign into a `var` mutable slice.
  buffer.copyBytes(from: [0, 1, 2, 3] as [UInt8])
  rightBytes[2..<4] = leftBytes
  expectEqualSequence(leftBytes, rightBytes)
}

// View an array's elements as bytes.
// Use copyBytes to overwrite the array element's bytes.
UnsafeRawBufferPointerTestSuite.test("initFromArray") {
  let array1: [Int32] = [0, 1, 2, 3]
  var array2 = [Int32](repeating: 0, count: 4)
  // Immutable view of array1's bytes.
  array1.withUnsafeBytes { bytes1 in
    expectEqual(bytes1.count, 16)
    for (i, b) in bytes1.enumerated() {
      var num = i
#if _endian(big)
      num = num + 1
#endif
      if num % 4 == 0 {
        expectEqual(Int(b), i / 4)
      }
      else {
        expectEqual(b, 0)
      }
    }
    // Mutable view of array2's bytes.
    array2.withUnsafeMutableBytes { bytes2 in
      expectEqual(bytes1.count, bytes2.count)
      bytes2.copyMemory(from: bytes1)
    }
  }
  expectEqual(array2, array1)
}

#if !os(WASI)
UnsafeRawBufferPointerTestSuite.test("initializeMemory(as:from:).underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 30, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }
  let source = stride(from: 5 as Int64, to: 0, by: -1)
  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  var (it, bound) = buffer.initializeMemory(as: Int64.self, from: source)
  let idx = bound.endIndex * MemoryLayout<Int64>.stride
  expectEqual(it.next()!, 2)
  expectEqual(idx, 24)
  let expected: [Int64] = [5,4,3]
  expected.withUnsafeBytes { expectEqualSequence($0,buffer[0..<idx]) }
  expectEqualSequence([5, 4, 3],bound)
}

UnsafeRawBufferPointerTestSuite.test("initializeMemory(as:from:).overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 30, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }
  let source: [Int64] = [5, 4, 3, 2, 1]
  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  var (it,bound) = buffer.initializeMemory(as: Int64.self, from: source)
  let idx = bound.endIndex * MemoryLayout<Int64>.stride
  expectEqual(it.next()!, 2)
  expectEqual(idx, 24)
  let expected: [Int64] = [5,4,3]
  expected.withUnsafeBytes { expectEqualSequence($0,buffer[0..<idx]) }
  expectEqualSequence([5, 4, 3],bound)
}
#endif

UnsafeRawBufferPointerTestSuite.test("initializeMemory(as:from:).exact") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 24, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }
  let source: [Int64] = [5, 4, 3]
  var (it,bound) = buffer.initializeMemory(as: Int64.self, from: source)
  let idx = bound.endIndex * MemoryLayout<Int64>.stride
  expectNil(it.next())
  expectEqual(idx, buffer.endIndex)
  source.withUnsafeBytes { expectEqualSequence($0,buffer) }
  expectEqualSequence([5, 4, 3],bound)
}

#if !os(WASI)
UnsafeRawBufferPointerTestSuite.test("initializeMemory(as:from:).invalidNilPtr") {
  let buffer = UnsafeMutableRawBufferPointer(start: nil, count: 0)
  let source: [Int64] = [5, 4, 3, 2, 1]
  expectCrashLater()
  _ = buffer.initializeMemory(as: Int64.self, from: source)
}
#endif

UnsafeRawBufferPointerTestSuite.test("initializeMemory(as:from:).validNilPtr") {
  let buffer = UnsafeMutableRawBufferPointer(start: nil, count: 0)
  let source: [Int64] = []
  var (it, bound) = buffer.initializeMemory(as: Int64.self, from: source)
  let idx = bound.endIndex * MemoryLayout<Int64>.stride
  expectNil(it.next())
  expectEqual(idx, source.endIndex)
}


// Directly test the byte Sequence produced by withUnsafeBytes.
UnsafeRawBufferPointerTestSuite.test("withUnsafeBytes.Sequence") {
  let array1: [Int32] = [0, 1, 2, 3]
  array1.withUnsafeBytes { bytes1 in
    // Initialize an array from a sequence of bytes.
    let byteArray = [UInt8](bytes1)
    for (b1, b2) in zip(byteArray, bytes1) {
      expectEqual(b1, b2)
    }
  }
}

// Test the empty buffer.
UnsafeRawBufferPointerTestSuite.test("empty") {
  let emptyBytes = UnsafeRawBufferPointer(start: nil, count: 0)
  for _ in emptyBytes {
    expectUnreachable()
  }
  let emptyMutableBytes = UnsafeMutableRawBufferPointer.allocate(byteCount: 0, alignment: MemoryLayout<UInt>.alignment)
  for _ in emptyMutableBytes {
    expectUnreachable()
  }
  emptyMutableBytes.deallocate()
}

// Store a sequence of integers to raw memory, and reload them as structs.
// Store structs to raw memory, and reload them as integers.
UnsafeRawBufferPointerTestSuite.test("reinterpret") {
  struct Pair {
    var x: Int32
    var y: Int32
  }
  let numPairs = 2
  let bytes = UnsafeMutableRawBufferPointer.allocate(
    byteCount: MemoryLayout<Pair>.stride * numPairs, alignment: MemoryLayout<UInt>.alignment)
  defer { bytes.deallocate() }  

  for i in 0..<(numPairs * 2) {
    bytes.storeBytes(of: Int32(i), toByteOffset: i * MemoryLayout<Int32>.stride,
      as: Int32.self)
  }
  let pair1 = bytes.load(as: Pair.self)
  let pair2 = bytes.load(fromByteOffset: MemoryLayout<Pair>.stride,
    as: Pair.self)
  expectEqual(0, pair1.x)
  expectEqual(1, pair1.y)
  expectEqual(2, pair2.x)
  expectEqual(3, pair2.y)

  bytes.storeBytes(of: Pair(x: -1, y: 0), as: Pair.self)
  for i in 0..<MemoryLayout<Int32>.stride {
    expectEqual(0xFF, bytes[i])
  }
  let bytes2 = UnsafeMutableRawBufferPointer(
    rebasing: bytes[MemoryLayout<Int32>.stride..<bytes.count])
  for i in 0..<MemoryLayout<Int32>.stride {
    expectEqual(0, bytes2[i])
  }
}

// Store, load, subscript, and slice at all valid byte offsets.
UnsafeRawBufferPointerTestSuite.test("inBounds") {
  let numInts = 4
  let bytes = UnsafeMutableRawBufferPointer.allocate(
    byteCount: MemoryLayout<Int>.stride * numInts, alignment: MemoryLayout<UInt>.alignment)
  defer { bytes.deallocate() }

  for i in 0..<numInts {
    bytes.storeBytes(
      of: i, toByteOffset: i * MemoryLayout<Int>.stride, as: Int.self)
  }
  for i in 0..<numInts {
    let x = bytes.load(
      fromByteOffset: i * MemoryLayout<Int>.stride, as: Int.self)
    expectEqual(x, i)
  }
  for i in 0..<numInts {
    var x = i
    withUnsafeBytes(of: &x) {
      for (offset, byte) in $0.enumerated() {
        expectEqual(bytes[i * MemoryLayout<Int>.stride + offset], byte)
      }
    }
  }
  let median = (numInts/2 * MemoryLayout<Int>.stride)
  var firstHalf = bytes[0..<median]
  let secondHalf = bytes[median..<bytes.count]
  firstHalf[0..<firstHalf.count] = secondHalf
  expectEqualSequence(firstHalf, secondHalf)
}

#if !os(WASI)
UnsafeRawBufferPointerTestSuite.test("subscript.get.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 2, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeRawBufferPointer(rebasing: buffer[1..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[-1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.get.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 2, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeRawBufferPointer(rebasing: buffer[0..<1])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.set.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 2, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  var bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[1..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1] = 0
}

UnsafeRawBufferPointerTestSuite.test("subscript.set.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 2, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  var bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[0..<1])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[1] = 0
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.get.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeRawBufferPointer(rebasing: buffer[1..<3])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[-1..<1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.get.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeRawBufferPointer(rebasing: buffer[0..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[1..<3]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.set.underflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  var bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[1..<3])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1..<1] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.set.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  var bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[0..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }

  // Performs a valid byte-wise copy but triggers a debug bounds check.
  bytes[1..<3] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.narrow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<3] = buffer[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.wide") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<2] = buffer[0..<3]
}

UnsafeRawBufferPointerTestSuite.test("copyMemory.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug range size check.
  UnsafeMutableRawBufferPointer(rebasing: bytes).copyMemory(
      from: UnsafeRawBufferPointer(buffer))
}

UnsafeRawBufferPointerTestSuite.test("copyBytes.sequence.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }
  
  let bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug range size check.
  UnsafeMutableRawBufferPointer(rebasing: bytes).copyBytes(
    from: [0, 1, 2] as [UInt8])
}

UnsafeRawBufferPointerTestSuite.test("load.before")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeBytes(of: &x) {
    _ = $0.load(fromByteOffset: -1, as: UInt8.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("load.after")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeBytes(of: &x) {
    _ = $0.load(as: UInt64.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("store.before")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeMutableBytes(of: &x) {
    $0.storeBytes(of: UInt8(0), toByteOffset: -1, as: UInt8.self)
  }
}
UnsafeRawBufferPointerTestSuite.test("store.after")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeMutableBytes(of: &x) {
    $0.storeBytes(of: UInt64(0), as: UInt64.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("copy.bytes.overflow")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int64 = 0
  var y: Int32 = 0
  withUnsafeBytes(of: &x) { srcBytes in
    withUnsafeMutableBytes(of: &y) { destBytes in
      destBytes.copyMemory(
        from: srcBytes)
    }
  }
}

UnsafeRawBufferPointerTestSuite.test("copy.sequence.overflow")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int64 = 0
  var y: Int32 = 0
  withUnsafeBytes(of: &x) { srcBytes in
    withUnsafeMutableBytes(of: &y) { destBytes in
      destBytes.copyMemory(from: srcBytes)
    }
  }
}
#endif

UnsafeRawBufferPointerTestSuite.test("copy.overlap") {
  let bytes = UnsafeMutableRawBufferPointer.allocate(byteCount: 4, alignment: MemoryLayout<UInt>.alignment)
  // Right Overlap
  bytes[0] = 1
  bytes[1] = 2
  bytes[1..<3] = bytes[0..<2]
  expectEqual(1, bytes[1])
  expectEqual(2, bytes[2])
  // Left Overlap
  bytes[1] = 2
  bytes[2] = 3
  bytes[0..<2] = bytes[1..<3]
  expectEqual(2, bytes[0])
  expectEqual(3, bytes[1])
  // Disjoint
  bytes[2] = 2
  bytes[3] = 3
  bytes[0..<2] = bytes[2..<4]
  expectEqual(2, bytes[0])
  expectEqual(3, bytes[1])
  bytes[0] = 0
  bytes[1] = 1
  bytes[2..<4] = bytes[0..<2]
  expectEqual(0, bytes[2])
  expectEqual(1, bytes[3])
}

runAllTests()

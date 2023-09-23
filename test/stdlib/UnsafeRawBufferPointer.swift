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
// Trap tests aren't available on WASI.
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
// Trap tests aren't available on WASI.
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
// Trap tests aren't available on WASI.
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

  let bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[1..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1] = 0
}

UnsafeRawBufferPointerTestSuite.test("subscript.set.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 2, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[0..<1])

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
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[1..<3])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1..<1] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.set.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = UnsafeMutableRawBufferPointer(rebasing: buffer[0..<2])

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }

  // Performs a valid byte-wise copy but triggers a debug bounds check.
  bytes[1..<3] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.narrow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<3] = buffer[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.wide") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<2] = buffer[0..<3]
}
#endif

UnsafeRawBufferPointerTestSuite.test("_copyContents") {
  let a = Array<UInt8>(0..<20)
  let b = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: 10*a.count)
  defer { b.deallocate() }
  var (unwritten, written) = a.withUnsafeBytes {
    bytes in
    bytes._copyContents(initializing: b)
  }
  expectNil(unwritten.next())
  expectEqual(written, a.count)
}

#if !os(WASI)
// Trap tests aren't available on WASI.
UnsafeRawBufferPointerTestSuite.test("copyMemory.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
  defer { buffer.deallocate() }

  let bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug range size check.
  UnsafeMutableRawBufferPointer(rebasing: bytes).copyMemory(
      from: UnsafeRawBufferPointer(buffer))
}
#endif

// Use copyBytes without contiguous storage
UnsafeRawBufferPointerTestSuite.test("copyBytes.withoutContiguousStorage") {
  let ranges: [Range<UInt8>] = [0..<2, 1..<3, 2..<4, 3..<5]
  var array = [UInt8](repeating: 0, count: 2)
  for range in ranges {
    array.withUnsafeMutableBytes { byte in
        byte.copyBytes(from: range)
    }
    expectEqual(array.count, range.count)
    expectEqual(array, Array(range))
  }
}

#if !os(WASI)
// Trap tests aren't available on WASI.
UnsafeRawBufferPointerTestSuite.test("copyBytes.sequence.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 3, alignment: MemoryLayout<UInt>.alignment)
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

UnsafeRawBufferPointerTestSuite.test("load.aligned") {
  var data: [UInt8] = [0, 0, 0, 0, .max, .max, .max, .max]
  data.withUnsafeBytes {
    let x = $0.load(fromByteOffset: 4, as: UInt32.self)
    expectEqual(x, .max)
  }
  data.withUnsafeMutableBytes {
    let x = $0.load(fromByteOffset: 0, as: UInt32.self)
    expectEqual(x, 0)
  }
}

UnsafeRawBufferPointerTestSuite.test("load.invalid")
.skip(.custom({ !_isDebugAssertConfiguration() }, // require debugAssert
              reason: "This tests a debug precondition.."))
.code {
  let data: [UInt8] = [0, 0, 0, .max, .max, .max, .max, 0]
  let i = data.firstIndex(of: .max)!
  expectCrashLater()
  _ = data.withUnsafeBytes {
    $0.load(fromByteOffset: i, as: UInt32.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("load.unaligned")
.skip(.custom({ // require SwiftStdlib 5.7
  if #available(SwiftStdlib 5.7, *) { return false } else { return true }
}, reason: "Requires stdlib from Swift 5.7"))
.code {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var data: [UInt8] = [0, 0, 0, .max, .max, .max, .max, 0]
  let i = data.firstIndex(of: .max)!
  data.withUnsafeBytes {
    let x = $0.loadUnaligned(fromByteOffset: i, as: UInt32.self)
    expectEqual(x, .max)
  }
  data.withUnsafeMutableBytes {
    let x = $0.loadUnaligned(fromByteOffset: i-1, as: UInt32.self)
    expectEqual(UInt32(littleEndian: x), 0xffffff00)
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

UnsafeRawBufferPointerTestSuite.test("store.unaligned")
.skip(.custom({
  if #available(SwiftStdlib 5.7, *) { return false }
  return true
}, reason: "Requires Swift 5.7's stdlib"))
.code {
  let count = MemoryLayout<UInt>.stride * 2
  let p1 = UnsafeMutableRawBufferPointer.allocate(
    byteCount: count,
    alignment: MemoryLayout<UInt>.alignment
  )
  defer { p1.deallocate() }
  p1.copyBytes(from: repeatElement(UInt8.zero, count: count))
  let value = UInt.max
  let offset = 3
  p1.storeBytes(of: value, toByteOffset: offset, as: UInt.self)
  expectEqual(p1.load(fromByteOffset: offset-1, as: UInt8.self),
              0)
  expectEqual(p1.load(fromByteOffset: offset, as: UInt8.self),
              .max)
  let storedLength = MemoryLayout<UInt>.size
  expectEqual(p1.load(fromByteOffset: offset-1+storedLength, as: UInt8.self),
              .max)
  expectEqual(p1.load(fromByteOffset: offset+storedLength, as: UInt8.self),
              0)
}

UnsafeRawBufferPointerTestSuite.test("store.invalid")
.skip(.custom({ !_isDebugAssertConfiguration() }, // require debugAssert
              reason: "This tests a debug precondition.."))
.skip(.custom({ // require SwiftStdlib 5.7
  if #available(SwiftStdlib 5.7, *) { return false } else { return true }
}, reason: "Requires stdlib from Swift 5.7"))
.code {
  let t = "Text that is longer than fits in a small String."
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: MemoryLayout<String>.size,
    alignment: MemoryLayout<String>.alignment
  )
  defer { p1.deallocate() }
  expectCrashLater()
  p1.storeBytes(of: t, as: String.self)
  expectUnreachable()
}

UnsafeRawBufferPointerTestSuite.test("store.valid") {
  let value32 = UInt32.max
  var value64 = Int64.zero
  withUnsafeMutableBytes(of: &value64) {
    $0.storeBytes(of: value32, toByteOffset: MemoryLayout<UInt32>.stride, as: UInt32.self)
  }
  expectEqual(value64, 0xffffffff << 32)
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
  defer { bytes.deallocate() }
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

UnsafeRawBufferPointerTestSuite.test("_customIndexOfEquatableElement") {
  let fibonacci = [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233] as [UInt8]
  fibonacci.withUnsafeBytes { bytes in
    expectEqual(Optional(Optional(0)), bytes._customIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(1)), bytes._customIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(7)), bytes._customIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(13)), bytes._customIndexOfEquatableElement(233))
    expectEqual(Optional(nil), bytes._customIndexOfEquatableElement(255))
    let unalignedBy1 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 1, count: bytes.count - 1)
    expectEqual(Optional(nil), unalignedBy1._customIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(0)), unalignedBy1._customIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(6)), unalignedBy1._customIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(12)), unalignedBy1._customIndexOfEquatableElement(233))
    let unalignedBy2 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 2, count: bytes.count - 2)
    expectEqual(Optional(nil), unalignedBy2._customIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(0)), unalignedBy2._customIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(5)), unalignedBy2._customIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(11)), unalignedBy2._customIndexOfEquatableElement(233))
    let unalignedBy3 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 3, count: bytes.count - 3)
    expectEqual(Optional(nil), unalignedBy3._customIndexOfEquatableElement(0))
    expectEqual(Optional(nil), unalignedBy3._customIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(4)), unalignedBy3._customIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(10)), unalignedBy3._customIndexOfEquatableElement(233))
    let singleValueBufferUnaligned = UnsafeRawBufferPointer(start: bytes.baseAddress! + 7, count: 1)
    expectEqual(Optional(Optional(0)), singleValueBufferUnaligned._customIndexOfEquatableElement(13))
    expectEqual(Optional(nil), singleValueBufferUnaligned._customIndexOfEquatableElement(0))
    let singleValueBufferAligned = UnsafeRawBufferPointer(start: bytes.baseAddress!, count: 1)
    expectEqual(Optional(Optional(0)), singleValueBufferAligned._customIndexOfEquatableElement(0))
    expectEqual(Optional(nil), singleValueBufferAligned._customIndexOfEquatableElement(1))
    let emptyBuffer = UnsafeRawBufferPointer(start: bytes.baseAddress!, count: 0)
    expectEqual(Optional(nil), emptyBuffer._customIndexOfEquatableElement(0))
    expectEqual(Optional(nil), emptyBuffer._customIndexOfEquatableElement(1))
  }
  let emptyBuffer = UnsafeRawBufferPointer(start: nil, count: 0)
  expectEqual(Optional(nil), emptyBuffer._customIndexOfEquatableElement(0))
  expectEqual(Optional(nil), emptyBuffer._customIndexOfEquatableElement(1))
}

UnsafeRawBufferPointerTestSuite.test("_customLastIndexOfEquatableElement") {
  let fibonacci = [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233] as [UInt8]
  fibonacci.withUnsafeBytes { bytes in
    expectEqual(Optional(Optional(0)), bytes._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(2)), bytes._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(7)), bytes._customLastIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(13)), bytes._customLastIndexOfEquatableElement(233))
    expectEqual(Optional(nil), bytes._customLastIndexOfEquatableElement(255))
    let unalignedBy1 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 1, count: bytes.count - 1)
    expectEqual(Optional(nil), unalignedBy1._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(1)), unalignedBy1._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(6)), unalignedBy1._customLastIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(12)), unalignedBy1._customLastIndexOfEquatableElement(233))
    let unalignedBy2 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 2, count: bytes.count - 2)
    expectEqual(Optional(nil), unalignedBy2._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(0)), unalignedBy2._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(5)), unalignedBy2._customLastIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(11)), unalignedBy2._customLastIndexOfEquatableElement(233))
    let unalignedBy3 = UnsafeRawBufferPointer(start: bytes.baseAddress! + 3, count: bytes.count - 3)
    expectEqual(Optional(nil), unalignedBy3._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(nil), unalignedBy3._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(4)), unalignedBy3._customLastIndexOfEquatableElement(13))
    expectEqual(Optional(Optional(10)), unalignedBy3._customLastIndexOfEquatableElement(233))
    let singleValueBufferUnaligned = UnsafeRawBufferPointer(start: bytes.baseAddress! + 7, count: 1)
    expectEqual(Optional(Optional(0)), singleValueBufferUnaligned._customLastIndexOfEquatableElement(13))
    expectEqual(Optional(nil), singleValueBufferUnaligned._customLastIndexOfEquatableElement(0))
    let singleValueBufferAligned = UnsafeRawBufferPointer(start: bytes.baseAddress!, count: 1)
    expectEqual(Optional(Optional(0)), singleValueBufferAligned._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(nil), singleValueBufferAligned._customLastIndexOfEquatableElement(1))
    let exactWordAligned = UnsafeRawBufferPointer(start: bytes.baseAddress!, count: MemoryLayout<UInt>.size)
    expectEqual(Optional(Optional(0)), exactWordAligned._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(2)), exactWordAligned._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(MemoryLayout<UInt>.size - 1)),
      exactWordAligned._customLastIndexOfEquatableElement(fibonacci[MemoryLayout<UInt>.size - 1]))
    let exactWordUnaligned = UnsafeRawBufferPointer(start: bytes.baseAddress! + 1, count: MemoryLayout<UInt>.size)
    expectEqual(Optional(nil), exactWordUnaligned._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(Optional(1)), exactWordUnaligned._customLastIndexOfEquatableElement(1))
    expectEqual(Optional(Optional(MemoryLayout<UInt>.size - 1)),
      exactWordUnaligned._customLastIndexOfEquatableElement(fibonacci[MemoryLayout<UInt>.size]))
    let emptyBuffer = UnsafeRawBufferPointer(start: bytes.baseAddress!, count: 0)
    expectEqual(Optional(nil), emptyBuffer._customLastIndexOfEquatableElement(0))
    expectEqual(Optional(nil), emptyBuffer._customLastIndexOfEquatableElement(1))
  }
  let emptyBuffer = UnsafeRawBufferPointer(start: nil, count: 0)
  expectEqual(Optional(nil), emptyBuffer._customLastIndexOfEquatableElement(0))
  expectEqual(Optional(nil), emptyBuffer._customLastIndexOfEquatableElement(1))
}

runAllTests()

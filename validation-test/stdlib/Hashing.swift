// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

// Freestanding doesn't support environment variables, and this test depends on SWIFT_DETERMINISTIC_HASHING=1
// UNSUPPORTED: freestanding

import Swift
import SwiftPrivate
import StdlibUnittest


var HashingTestSuite = TestSuite("Hashing")

func checkHash(
  for value: UInt64,
  withSeed seed: UInt,
  expected: UInt64,
  file: String = #file, line: UInt = #line
) {
  var hasher = Hasher(_seed: Int(bitPattern: seed))
  hasher._combine(value)
  let hash = hasher.finalize()
  expectEqual(
    hash, Int(truncatingIfNeeded: expected),
    file: file, line: line)
}

HashingTestSuite.test("Hasher/CustomKeys") {
  // This assumes Hasher implements SipHash-1-3 and hashing is deterministic.
  expectTrue(Hasher._isDeterministic)

  checkHash(for: 0, withSeed: 0, expected: 0xbd60acb658c79e45)
  checkHash(for: 1, withSeed: 0, expected: 0x1e9f734161d62dd9)
  checkHash(for: .max, withSeed: 0, expected: 0x2f205be2fec8e38d)

  checkHash(for: 0, withSeed: 1, expected: 0x9c44b7c8df2ca74b)
  checkHash(for: 1, withSeed: 1, expected: 0xacb556b13007504a)
  checkHash(for: .max, withSeed: 1, expected: 0x404afd8eb2c4b22a)

  checkHash(for: 0, withSeed: 0xFFFFFFFF, expected: 0x47329159fe988221)
  checkHash(for: 1, withSeed: 0xFFFFFFFF, expected: 0xd7da861471fc35dc)
  checkHash(for: .max, withSeed: 0xFFFFFFFF, expected: 0xf6e3047fc114dbc0)

#if _pointerBitWidth(_64)
  checkHash(for: 0, withSeed: 0xFFFFFFFF_FFFFFFFF, expected: 0x8d0ea5ad8d6a55c7)
  checkHash(for: 1, withSeed: 0xFFFFFFFF_FFFFFFFF, expected: 0x2899f60d6b5bc847)
  checkHash(for: .max, withSeed: 0xFFFFFFFF_FFFFFFFF, expected: 0xc4d7726fff5e65a0)
#endif
}

HashingTestSuite.test("Hasher/DefaultKey") {
  let value: UInt64 = 0x0102030405060708

  let defaultHash = _hashValue(for: value)

  let rawHash = value._rawHashValue(seed: 0)
  expectEqual(rawHash, defaultHash)

  let oneShotHash = Hasher._hash(seed: 0, value)
  expectEqual(oneShotHash, defaultHash)

  var defaultHasher = Hasher()
  defaultHasher._combine(value)
  expectEqual(defaultHasher.finalize(), defaultHash)

  var customHasher = Hasher(_seed: 0)
  customHasher._combine(value)
  expectEqual(customHasher.finalize(), defaultHash)
}

HashingTestSuite.test("Hashing/TopLevelHashing/UInt64") {
  func checkTopLevelHash(
    for value: UInt64,
    seed: Int,
    file: String = #file,
    line: UInt = #line) {
    var hasher = Hasher(_seed: seed)
    hasher._combine(value)
    let expected = hasher.finalize()
    let actual = Hasher._hash(seed: seed, value)
    expectEqual(actual, expected, file: file, line: line)
  }
  checkTopLevelHash(for: 0, seed: 0)
  checkTopLevelHash(for: 1, seed: 0)
  checkTopLevelHash(for: 0, seed: 1)
  checkTopLevelHash(for: 1, seed: 1)
  checkTopLevelHash(for: 0x0102030405060708, seed: 1)
  checkTopLevelHash(for: 0x0102030405060708, seed: Int.max)
  checkTopLevelHash(for: UInt64.max, seed: 1)
  checkTopLevelHash(for: UInt64.max, seed: Int.max)
}

HashingTestSuite.test("Hashing/TopLevelHashing/UInt") {
  func checkTopLevelHash(
    for value: UInt,
    seed: Int,
    file: String = #file,
    line: UInt = #line) {
    var hasher = Hasher(_seed: seed)
    hasher._combine(value)
    let expected = hasher.finalize()
    let actual = Hasher._hash(seed: seed, value)
    expectEqual(actual, expected, file: file, line: line)
  }
  checkTopLevelHash(for: 0, seed: 0)
  checkTopLevelHash(for: 1, seed: 0)
  checkTopLevelHash(for: 0, seed: 1)
  checkTopLevelHash(for: 1, seed: 1)
  checkTopLevelHash(
    for: UInt(truncatingIfNeeded: 0x0102030405060708 as UInt64),
    seed: 1)
  checkTopLevelHash(
    for: UInt(truncatingIfNeeded: 0x0102030405060708 as UInt64),
    seed: Int(truncatingIfNeeded: 0x8877665544332211 as UInt64))
  checkTopLevelHash(for: UInt.max, seed: 1)
  checkTopLevelHash(for: UInt.max, seed: Int.max)
}

HashingTestSuite.test("Hashing/TopLevelHashing/PartialUInt64") {
  func checkTopLevelHash(
    for value: UInt64,
    count: Int,
    seed: Int,
    file: String = #file,
    line: UInt = #line) {
    var hasher = Hasher(_seed: seed)
    hasher._combine(bytes: value, count: count)
    let expected = hasher.finalize()
    let actual = Hasher._hash(seed: seed, bytes: value, count: count)
    expectEqual(
      actual,
      expected,
      "seed: \(seed), value: \(value), count: \(count)",
      file: file,
      line: line)
  }
  for seed: Int in [
    0,
    1,
    2,
    Int(truncatingIfNeeded: 0x1827364554637281 as UInt64),
    Int(truncatingIfNeeded: 0xf9e8d7c6b5a49382 as UInt64)
  ] {
    for count in 1 ..< 8 {
      checkTopLevelHash(for: 0, count: count, seed: seed)
    }
    checkTopLevelHash(for: 0x01, count: 1, seed: seed)
    checkTopLevelHash(for: 0x0102, count: 2, seed: seed)
    checkTopLevelHash(for: 0x010203, count: 3, seed: seed)
    checkTopLevelHash(for: 0x01020304, count: 4, seed: seed)
    checkTopLevelHash(for: 0x0102030405, count: 5, seed: seed)
    checkTopLevelHash(for: 0x010203040506, count: 6, seed: seed)
    checkTopLevelHash(for: 0x01020304050607, count: 7, seed: seed)
  }
}

HashingTestSuite.test("Hashing/TopLevelHashing/UnsafeRawBufferPointer") {
  func checkTopLevelHash(
    for buffer: [UInt8],
    seed: Int,
    file: String = #file,
    line: UInt = #line) {
    var hasher = Hasher(_seed: seed)
    buffer.withUnsafeBytes { buffer in
      hasher.combine(bytes: buffer)
    }
    let expected = hasher.finalize()
    let actual = buffer.withUnsafeBytes { buffer in
      Hasher._hash(seed: seed, bytes: buffer)
    }
    expectEqual(
      actual,
      expected,
      "seed: \(seed), buffer: \(buffer)",
      file: file,
      line: line)
  }
  for seed: Int in [
    0,
    1,
    2,
    Int(truncatingIfNeeded: 0x1827364554637281 as UInt64),
    Int(truncatingIfNeeded: 0xf9e8d7c6b5a49382 as UInt64)
  ] {
    var zeros: [UInt8] = []
    var integers: [UInt8] = []
    for i: UInt8 in 0 ..< 20 {
      zeros.append(0)
      checkTopLevelHash(for: zeros, seed: seed)
      integers.append(i)
      checkTopLevelHash(for: integers, seed: seed)
    }
  }
}

runAllTests()

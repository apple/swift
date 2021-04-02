// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

import StdlibUnittest

var BitsetTests = TestSuite("Bitset")

BitsetTests.test("_UnsafeBitset.wordCount(forCapacity:)") {
  expectEqual(0, _UnsafeBitset.wordCount(forCapacity: 0))
#if arch(i386) || arch(arm)
  for i in 1...32 {
    expectEqual(1, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
  for i in 33...64 {
    expectEqual(2, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
  for i in 65...96 {
    expectEqual(3, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
  for i in 97...128 {
    expectEqual(4, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
#elseif arch(x86_64) || arch(arm64) || arch(powerpc64) || arch(powerpc64le) || arch(s390x)
  for i in 1...64 {
    expectEqual(1, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
  for i in 65...128 {
    expectEqual(2, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
  for i in 193...256 {
    expectEqual(4, _UnsafeBitset.wordCount(forCapacity: i), "i=\(i)")
  }
#else
  _UnimplementedError()
#endif
}

BitsetTests.test("_UnsafeBitset.split(_:)") {
#if arch(i386) || arch(arm)
  for i in 0...31 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(0, comps.word, "i=\(i)")
    expectEqual(i, comps.bit, "i=\(i)")
  }
  for i in 32...63 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(1, comps.word, "i=\(i)")
    expectEqual(i - 32, comps.bit, "i=\(i)")
  }
  for i in 64...95 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(2, comps.word, "i=\(i)")
    expectEqual(i - 64, comps.bit, "i=\(i)")
  }
  for i in 96...127 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(3, comps.word, "i=\(i)")
    expectEqual(i - 96, comps.bit, "i=\(i)")
  }
#elseif arch(x86_64) || arch(arm64) || arch(powerpc64) || arch(powerpc64le) || arch(s390x)
  for i in 0...63 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(0, comps.word, "i=\(i)")
    expectEqual(i, comps.bit, "i=\(i)")
  }
  for i in 64...127 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(1, comps.word, "i=\(i)")
    expectEqual(i - 64, comps.bit, "i=\(i)")
  }
  for i in 128...191 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(2, comps.word, "i=\(i)")
    expectEqual(i - 128, comps.bit, "i=\(i)")
  }
  for i in 192...255 {
    let comps = _UnsafeBitset.split(i)
    expectEqual(3, comps.word, "i=\(i)")
    expectEqual(i - 192, comps.bit, "i=\(i)")
  }
#else
  _UnimplementedError()
#endif
}

BitsetTests.test("Word.capacity") {
  expectEqual(_Bitset.Word.capacity, UInt.bitWidth)
}

BitsetTests.test("Word.uncheckedContains(_:)") {
  typealias Word = _Bitset.Word
  for i in 0 ..< Word.capacity {
    expectEqual(Word(0).uncheckedContains(i), false, "i=\(i)")
    expectEqual(Word(1).uncheckedContains(i), i == 0, "i=\(i)")
    expectEqual(Word(2).uncheckedContains(i), i == 1, "i=\(i)")
    expectEqual(Word(4).uncheckedContains(i), i == 2, "i=\(i)")
    expectEqual(Word(8).uncheckedContains(i), i == 3, "i=\(i)")
    expectEqual(Word(15).uncheckedContains(i), i <= 3, "i=\(i)")
    expectEqual(Word(~1).uncheckedContains(i), i != 0, "i=\(i)")
    expectEqual(Word(~2).uncheckedContains(i), i != 1, "i=\(i)")
    expectEqual(Word(~4).uncheckedContains(i), i != 2, "i=\(i)")
    expectEqual(Word(~8).uncheckedContains(i), i != 3, "i=\(i)")
    expectEqual(Word(~15).uncheckedContains(i), i > 3, "i=\(i)")
    expectEqual(Word(UInt.max).uncheckedContains(i), true, "i=\(i)")
  }
}

BitsetTests.test("Word.uncheckedInsert(_:)") {
  typealias Word = _Bitset.Word
  var word = Word(0)
  for i in 0 ..< Word.capacity {
    expectFalse(word.uncheckedContains(i))
    expectTrue(word.uncheckedInsert(i))
    expectTrue(word.uncheckedContains(i))
    expectFalse(word.uncheckedInsert(i))
  }
  expectEqual(word.value, UInt.max)
}

BitsetTests.test("Word.uncheckedRemove(_:)") {
  typealias Word = _Bitset.Word
  var word = Word(UInt.max)
  for i in 0 ..< Word.capacity {
    expectTrue(word.uncheckedContains(i))
    expectTrue(word.uncheckedRemove(i))
    expectFalse(word.uncheckedContains(i))
    expectFalse(word.uncheckedRemove(i))
  }
  expectEqual(word.value, 0)
}

BitsetTests.test("Word.count") {
  typealias Word = _Bitset.Word
  var word = Word(0)
  for i in 0 ..< Word.capacity {
    expectEqual(word.count, i)
    _ = word.uncheckedInsert(i)
    expectEqual(word.count, i + 1)
  }
  for i in 0 ..< Word.capacity {
    expectEqual(word.count, Word.capacity - i)
    _ = word.uncheckedRemove(i)
    expectEqual(word.count, Word.capacity - i - 1)
  }
}

BitsetTests.test("Word.makeIterator()") {
  typealias Word = _Bitset.Word
  expectEqual(Array(Word(0)), [])
  expectEqual(Array(Word(1)), [0])
  expectEqual(Array(Word(2)), [1])
  expectEqual(Array(Word(3)), [0, 1])
  expectEqual(Array(Word(42)), [1, 3, 5])
  expectEqual(Array(Word(UInt.max)), Array(0..<UInt.bitWidth))
}

let capacities = [
  0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 48, 63, 64, 65, 127, 128, 129, 1024, 10000
]

let primes: Set<Int> = [
  2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
  73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
  157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233,
  239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317,
  331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419,
  421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499
]

BitsetTests.test("_Bitset:InitializedToZero")
.forEach(in: capacities) { capacity in
  let bitset = _Bitset(capacity: capacity)
  expectEqual(bitset.count, 0)
  for bit in 0 ..< capacity {
    expectFalse(bitset.uncheckedContains(bit))
  }
}

BitsetTests.test("_Bitset:Allocation")
.forEach(in: capacities) { capacity in
  let bitset = _Bitset(capacity: capacity)
  expectEqual(bitset.storage != nil, capacity > _Bitset.Word.capacity)
}

BitsetTests.test("_Bitset.uncheckedContains(_:)")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  for p in primes where p < capacity {
    expectFalse(bitset.uncheckedContains(p), "\(p)")
    _ = bitset.uncheckedInsert(p)
    expectTrue(bitset.uncheckedContains(p), "\(p)")
  }
  for i in 0 ..< capacity {
    expectEqual(
      bitset.uncheckedContains(i),
      primes.contains(i),
      "\(i)")
  }
}

BitsetTests.test("_Bitset.uncheckedInsert(_:)")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  for p in primes where p < capacity {
    let inserted = bitset.uncheckedInsert(p)
    expectTrue(inserted, "\(p)")
  }
  for p in primes where p < capacity {
    let inserted = bitset.uncheckedInsert(p)
    expectFalse(inserted, "\(p)")
  }
  for i in 0 ..< capacity {
    expectEqual(
      bitset.uncheckedContains(i),
      primes.contains(i),
      "\(i)")
  }
}

BitsetTests.test("_Bitset.uncheckedInsert(_:):COW")
.forEach(in: capacities) { capacity in
  for bit in primes where bit < capacity {
    var bitset = _Bitset(capacity: capacity)
    let copy = bitset

    bitset.uncheckedInsert(bit)
    expectTrue(bitset.uncheckedContains(bit), "\(bit)")
    expectFalse(copy.uncheckedContains(bit), "\(bit)")
  }
}

BitsetTests.test("_Bitset.uncheckedRemove(_:)")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  for i in 0 ..< capacity {
    bitset.uncheckedInsert(i)
  }
  for bit in primes where bit < capacity {
    expectTrue(bitset.uncheckedRemove(bit), "\(bit)")
  }
  for bit in primes where bit < capacity {
    expectFalse(bitset.uncheckedRemove(bit), "\(bit)")
  }
  for i in 0 ..< capacity {
    expectEqual(
      bitset.uncheckedContains(i),
      !primes.contains(i),
      "\(i)")
  }
}

BitsetTests.test("_Bitset.uncheckedRemove(_:):COW")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  for bit in primes where bit < capacity {
    bitset.uncheckedInsert(bit)
  }
  for bit in primes where bit < capacity {
    var copy = bitset
    expectTrue(copy.uncheckedRemove(bit), "\(bit)")
    expectFalse(copy.uncheckedContains(bit), "\(bit)")
    expectTrue(bitset.uncheckedContains(bit), "\(bit)")
  }
}

BitsetTests.test("_Bitset.count")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  var c = 0
  for bit in primes where bit < capacity {
    bitset.uncheckedInsert(bit)
    c += 1
  }
  expectEqual(c, bitset.count)
}

BitsetTests.test("_Bitset.makeIterator()")
.forEach(in: capacities) { capacity in
  var bitset = _Bitset(capacity: capacity)
  let bits = primes.filter { $0 < capacity }
  for b in bits {
    bitset.uncheckedInsert(b)
  }
  expectEqual(Array(bitset), bits.sorted())
}

runAllTests()

// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

import StdlibUnittest

var BitsetTests = TestSuite("Bitset")

BitsetTests.test("_UnsafeBitset.wordCount(forCapacity:)") {
  expectEqual(0, _UnsafeBitset.wordCount(forCapacity: 0))
#if _pointerBitWidth(_32)
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
#elseif _pointerBitWidth(_64)
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
#if _pointerBitWidth(_32)
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
#elseif _pointerBitWidth(_64)
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
  expectEqual(_UnsafeBitset.Word.capacity, UInt.bitWidth)
}

BitsetTests.test("Word.uncheckedContains(_:)") {
  typealias Word = _UnsafeBitset.Word
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
  typealias Word = _UnsafeBitset.Word
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
  typealias Word = _UnsafeBitset.Word
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
  typealias Word = _UnsafeBitset.Word
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
  typealias Word = _UnsafeBitset.Word
  expectEqual(Array(Word(0)), [])
  expectEqual(Array(Word(1)), [0])
  expectEqual(Array(Word(2)), [1])
  expectEqual(Array(Word(3)), [0, 1])
  expectEqual(Array(Word(42)), [1, 3, 5])
  expectEqual(Array(Word(UInt.max)), Array(0..<UInt.bitWidth))
}

runAllTests()

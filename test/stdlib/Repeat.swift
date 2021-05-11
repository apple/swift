// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest

let RepeatTests = TestSuite("Repeated")
RepeatTests.test("repeatElement") {
  let sequence = repeatElement(1, count: 5)
  expectEqual(sequence.count, 5)
  expectEqualSequence(sequence, [1, 1, 1, 1, 1])
  expectEqual(sequence.startIndex, 0)
  expectEqual(sequence.endIndex, 5)
  expectEqual(sequence[0], 1)
}

RepeatTests.test("associated-types") {
  typealias Subject = Repeated<String>
  expectRandomAccessCollectionAssociatedTypes(
      collectionType: Subject.self,
      iteratorType: IndexingIterator<Subject>.self,
      subSequenceType: Slice<Subject>.self,
      indexType: Int.self,
      indicesType: CountableRange<Int>.self)
}

#if !os(WASI)
RepeatTests.test("out-of-bounds") {
  let sequence = repeatElement(0, count: 1)
  expectCrashLater()
  _ = sequence[sequence.count]
}
#endif

runAllTests()


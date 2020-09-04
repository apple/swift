//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import StdlibUnittest

// These tests are shared between partition(by:) and sort().
public struct PartitionExhaustiveTest {
  public let sequence: [Int]
  public let loc: SourceLoc

  public init(
    _ sequence: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.sequence = sequence
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

enum SillyError : Error { case JazzHands }

public let partitionExhaustiveTests = [
  PartitionExhaustiveTest([]),
  PartitionExhaustiveTest([ 10 ]),
  PartitionExhaustiveTest([ 10, 10 ]),
  PartitionExhaustiveTest([ 10, 20 ]),
  PartitionExhaustiveTest([ 10, 10, 10 ]),
  PartitionExhaustiveTest([ 10, 10, 20 ]),
  PartitionExhaustiveTest([ 10, 20, 20 ]),
  PartitionExhaustiveTest([ 10, 20, 30 ]),
  PartitionExhaustiveTest([ 10, 10, 10, 10 ]),
  PartitionExhaustiveTest([ 10, 10, 10, 20 ]),
  PartitionExhaustiveTest([ 10, 10, 20, 20 ]),
  PartitionExhaustiveTest([ 10, 20, 30, 40 ]),
  PartitionExhaustiveTest([ 10, 10, 10, 10, 10 ]),
  PartitionExhaustiveTest([ 10, 10, 10, 20, 20 ]),
  PartitionExhaustiveTest([ 10, 10, 10, 20, 30 ]),
  PartitionExhaustiveTest([ 10, 10, 20, 20, 30 ]),
  PartitionExhaustiveTest([ 10, 10, 20, 30, 40 ]),
  PartitionExhaustiveTest([ 10, 20, 30, 40, 50 ]),
  PartitionExhaustiveTest([ 10, 20, 30, 40, 50, 60 ]),
]

//Random collection of 30 elements
 public let largeElementSortTests = [
   [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30],
   [30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18,
     17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1],
   [30, 29, 28, 27, 26, 25,  20, 19, 18, 5, 4, 3, 2, 1,
     15, 14, 13, 12, 11, 10, 9, 24, 23, 22, 21, 8, 17, 16, 7, 6],
   [30, 29, 25, 20, 19, 18, 5, 4, 3, 2, 1, 28, 27, 26,
     15, 14, 13, 12,  24, 23, 22, 21, 8, 17, 16, 7, 6,  11, 10, 9],
   [3, 2, 1, 20, 19, 18, 5, 4, 28, 27, 26, 11, 10, 9,
     15, 14, 13, 12,  24, 23, 22, 21, 8, 17, 16, 7, 6, 30, 29, 25],
 ]

public func withInvalidOrderings(_ body: (@escaping (Int, Int) -> Bool) -> Void) {
  // Test some ordering predicates that don't create strict weak orderings
  body { (_,_) in true }
  body { (_,_) in false }
  var i = 0
  body { (_,_) in defer {i += 1}; return i % 2 == 0 }
  body { (_,_) in defer {i += 1}; return i % 3 == 0 }
  body { (_,_) in defer {i += 1}; return i % 5 == 0 }
}

internal func _mapInPlace<C : MutableCollection>(
  _ elements: inout C,
  _ transform: (C.Element) -> C.Element
) {
  for i in elements.indices {
    elements[i] = transform(elements[i])
  }
}

internal func makeBufferAccessLoggingMutableCollection<
  C
>(wrapping c: C) -> BufferAccessLoggingMutableBidirectionalCollection<C> {
  return BufferAccessLoggingMutableBidirectionalCollection(wrapping: c)
}

internal func makeBufferAccessLoggingMutableCollection<
  C
>(wrapping c: C) -> BufferAccessLoggingMutableRandomAccessCollection<C> {
  return BufferAccessLoggingMutableRandomAccessCollection(wrapping: c)
}

extension TestSuite {
  public func addMutableCollectionTests<
    C : MutableCollection,
    CollectionWithEquatableElement : MutableCollection,
    CollectionWithComparableElement : MutableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    makeCollectionOfComparable: @escaping ([CollectionWithComparableElement.Element]) -> CollectionWithComparableElement,
    wrapValueIntoComparable: @escaping (MinimalComparableValue) -> CollectionWithComparableElement.Element,
    extractValueFromComparable: @escaping ((CollectionWithComparableElement.Element) -> MinimalComparableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1,
    outOfBoundsSubscriptOffset: Int = 1,
    withUnsafeMutableBufferPointerIsSupported: Bool,
    isFixedLengthCollection: Bool,
    collectionIsBidirectional: Bool = false
  ) where
    CollectionWithEquatableElement.Element : Equatable,
    CollectionWithComparableElement.Element : Comparable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
      outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset,
      collectionIsBidirectional: collectionIsBidirectional
    )

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    func makeWrappedCollectionWithComparableElement(
      _ elements: [MinimalComparableValue]
    ) -> CollectionWithComparableElement {
      return makeCollectionOfComparable(elements.map(wrapValueIntoComparable))
    }

    testNamePrefix += String(describing: C.self)

//===----------------------------------------------------------------------===//
// subscript(_: Index)
//===----------------------------------------------------------------------===//

if resiliencyChecks.subscriptOnOutOfBoundsIndicesBehavior != .none {
  self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Right/NonEmpty/Set") {
    var c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
    var index = c.endIndex
    expectCrashLater()
    index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
    c[index] = wrapValue(OpaqueValue(9999))
  }

  self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Right/Empty/Set") {
    var c = makeWrappedCollection([])
    var index = c.endIndex
    expectCrashLater()
    index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
    c[index] = wrapValue(OpaqueValue(9999))
  }

  let tests = cartesianProduct(
    subscriptRangeTests,
    _SubSequenceSubscriptOnIndexMode.all)

  self.test("\(testNamePrefix).SubSequence.subscript(_: Index)/Set/OutOfBounds")
    .forEach(in: tests) {
    (test, mode) in
    let elements = test.collection
    let sliceFromLeft = test.bounds.lowerBound
    let sliceFromRight = elements.count - test.bounds.upperBound
    print("\(elements)/sliceFromLeft=\(sliceFromLeft)/sliceFromRight=\(sliceFromRight)")
    let base = makeWrappedCollection(elements)
    let sliceStartIndex =
      base.index(base.startIndex, offsetBy: sliceFromLeft)
    let sliceEndIndex = base.index(
      base.startIndex,
      offsetBy: elements.count - sliceFromRight)
    var slice = base[sliceStartIndex..<sliceEndIndex]
    expectType(C.SubSequence.self, &slice)

    var index: C.Index = base.startIndex
    switch mode {
    case .inRange:
      let sliceNumericIndices =
        sliceFromLeft..<(elements.count - sliceFromRight)
      for (i, index) in base.indices.enumerated() {
        if sliceNumericIndices.contains(i) {
          slice[index] = wrapValue(OpaqueValue(elements[i].value + 90000))
        }
      }
      for (i, index) in base.indices.enumerated() {
        if sliceNumericIndices.contains(i) {
          expectEqual(
            elements[i].value + 90000,
            extractValue(slice[index]).value)
          expectEqual(
            extractValue(base[index]).value + 90000,
            extractValue(slice[index]).value)
        }
      }
      return
    case .outOfRangeToTheLeft:
      if sliceFromLeft == 0 { return }
      index = base.index(
        base.startIndex,
        offsetBy: sliceFromLeft - 1)
    case .outOfRangeToTheRight:
      if sliceFromRight == 0 { return }
      index = base.index(
        base.startIndex,
        offsetBy: elements.count - sliceFromRight)
    case .baseEndIndex:
      index = base.endIndex
    case .sliceEndIndex:
      index = sliceEndIndex
    }

    expectCrashLater()
    slice[index] = wrapValue(OpaqueValue(9999))
  }
}

//===----------------------------------------------------------------------===//
// subscript(_: Range)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).subscript(_: Range)/Set/semantics") {
  for test in subscriptRangeTests {
    var expectedCollection = test.collection
    _mapInPlace(&expectedCollection[test.bounds(in: expectedCollection)]) {
      OpaqueValue($0.value + 90000)
    }

    do {
      // Call setter with new elements coming from a different collection.
      var c = makeWrappedCollection(test.collection)

      let newElements = makeWrappedCollection(
        test.expected.map { OpaqueValue($0.value + 90000) })
      c[test.bounds(in: c)] = newElements[...]

      // FIXME: improve checkForwardCollection to check the SubSequence type.
      /*
      // TODO: swift-3-indexing-model: uncomment the following.
      checkForwardCollection(
        expectedCollection.map(wrapValue),
        c,
        resiliencyChecks: .none) {
        extractValue($0).value == extractValue($1).value
      }
      */
    }

    do {
      // Call setter implicitly through an inout mutation.
      let c = makeWrappedCollection(test.collection)

      var s = c[test.bounds(in: c)]
      _mapInPlace(&s) {
        wrapValue(OpaqueValue(extractValue($0).value + 90000))
      }

      // FIXME: improve checkForwardCollection to check the SubSequence type.
      /*
      // TODO: swift-3-indexing-model: uncomment the following.
      checkForwardCollection(
        expectedCollection.map(wrapValue),
        c,
        resiliencyChecks: .none) {
        extractValue($0).value == extractValue($1).value
      }
      */
    }
  }
}

if isFixedLengthCollection {
  self.test("\(testNamePrefix).subscript(_: Range)/Set/LargerRange") {
    var c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
    let newElements = makeWrappedCollection([ 91010, 92020, 93030, 94040 ].map(OpaqueValue.init))

    expectCrashLater()
    c[...] = newElements[...]
  }

  self.test("\(testNamePrefix).subscript(_: Range)/Set/SmallerRange") {
    var c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
    let newElements = makeWrappedCollection([ 91010, 92020 ].map(OpaqueValue.init))

    expectCrashLater()
    c[...] = newElements[...]
  }
} else {
  self.test("\(testNamePrefix).subscript(_: Range)/Set/DifferentlySizedRange") {
    for test in replaceRangeTests {
      var c = makeWrappedCollection(test.collection)
      let newElements = makeWrappedCollection(test.newElements)
      let rangeToReplace = test.rangeSelection.range(in: c)

      c[rangeToReplace] = newElements[...]

      expectEqualSequence(
        test.expected,
        c.map(extractValue).map { $0.value },
        stackTrace: SourceLocStack().with(test.loc))
    }
  }
}

if resiliencyChecks.subscriptRangeOnOutOfBoundsRangesBehavior != .none {
  let tests = cartesianProduct(
    subscriptRangeTests,
    _SubSequenceSubscriptOnRangeMode.all)

  self.test("\(testNamePrefix).SubSequence.subscript(_: Range)/Set/OutOfBounds")
    .forEach(in: tests) {
    (test, mode) in
    let elements = test.collection
    let sliceFromLeft = test.bounds.lowerBound
    let sliceFromRight = elements.count - test.bounds.upperBound
    print("\(elements)/sliceFromLeft=\(sliceFromLeft)/sliceFromRight=\(sliceFromRight)")
    let base = makeWrappedCollection(elements)
    let sliceStartIndex =
      base.index(base.startIndex, offsetBy: sliceFromLeft)
    let sliceEndIndex = base.index(
      base.startIndex,
      offsetBy: elements.count - sliceFromRight)
    var slice = base[sliceStartIndex..<sliceEndIndex]
    expectType(C.SubSequence.self, &slice)

    var bounds: Range<C.Index> = base.startIndex..<base.startIndex
    switch mode {
    case .inRange:
      let sliceNumericIndices =
        sliceFromLeft..<(elements.count - sliceFromRight + 1)
      for (i, subSliceStartIndex) in base.indices.enumerated() {
        for (j, subSliceEndIndex) in base.indices.enumerated() {
          if i <= j &&
            sliceNumericIndices.contains(i) &&
            sliceNumericIndices.contains(j) {
            let newValues = makeWrappedCollection(
              elements[i..<j].map { OpaqueValue($0.value + 90000) }
            )
            slice[subSliceStartIndex..<subSliceEndIndex] = newValues[...]
            let subSlice = slice[subSliceStartIndex..<subSliceEndIndex]
            for (k, index) in subSlice.indices.enumerated() {
              expectEqual(
                elements[i + k].value + 90000,
                extractValue(subSlice[index]).value)
              expectEqual(
                extractValue(base[index]).value + 90000,
                extractValue(subSlice[index]).value)
              expectEqual(
                extractValue(slice[index]).value,
                extractValue(subSlice[index]).value)
            }
            let oldValues = makeWrappedCollection(Array(elements[i..<j]))
            slice[subSliceStartIndex..<subSliceEndIndex] = oldValues[...]
          }
        }
      }
      return
    case .outOfRangeToTheLeftEmpty:
      if sliceFromLeft == 0 { return }
      let index = base.index(
        base.startIndex,
        offsetBy: sliceFromLeft - 1)
      bounds = index..<index
      break
    case .outOfRangeToTheLeftNonEmpty:
      if sliceFromLeft == 0 { return }
      let index = base.index(
        base.startIndex,
        offsetBy: sliceFromLeft - 1)
      bounds = index..<sliceStartIndex
      break
    case .outOfRangeToTheRightEmpty:
      if sliceFromRight == 0 { return }
      let index = base.index(
        base.startIndex,
        offsetBy: elements.count - sliceFromRight + 1)
      bounds = index..<index
      break
    case .outOfRangeToTheRightNonEmpty:
      if sliceFromRight == 0 { return }
      let index = base.index(
        base.startIndex,
        offsetBy: elements.count - sliceFromRight + 1)
      bounds = sliceEndIndex..<index
      break
    case .outOfRangeBothSides:
      if sliceFromLeft == 0 { return }
      if sliceFromRight == 0 { return }
      bounds =
        base.index(
          base.startIndex,
          offsetBy: sliceFromLeft - 1)
        ..<
        base.index(
          base.startIndex,
          offsetBy: elements.count - sliceFromRight + 1)
      break
    case .baseEndIndex:
      if sliceFromRight == 0 { return }
      bounds = sliceEndIndex..<base.endIndex
      break
    }

    let count: Int =
      base.distance(from: bounds.lowerBound, to: bounds.upperBound)
    let newValues = makeWrappedCollection(Array(elements[0..<count]))
    let newSlice = newValues[...]

    expectCrashLater()
    slice[bounds] = newSlice
  }
}

//===----------------------------------------------------------------------===//
// withContiguousMutableStorageIfAvailable()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).withContiguousMutableStorageIfAvailable()/semantics") {
  for test in subscriptRangeTests {
    var c = makeWrappedCollection(test.collection)
    var result = c.withContiguousMutableStorageIfAvailable {
      (bufferPointer) -> OpaqueValue<Array<OpaqueValue<Int>>> in
      let value = OpaqueValue(bufferPointer.map(extractValue))
      return value
    }
    expectType(Optional<OpaqueValue<Array<OpaqueValue<Int>>>>.self, &result)
    if withUnsafeMutableBufferPointerIsSupported {
      expectEqualSequence(test.collection, result!.value) { $0.value == $1.value }
    } else {
      expectNil(result)
    }
  }
}

//===----------------------------------------------------------------------===//
// sort()
//===----------------------------------------------------------------------===//

func checkSortedPredicateThrow(
  sequence: [Int],
  lessImpl: ((Int, Int) -> Bool),
  throwIndex: Int
) {
  let extract = extractValue
  let throwElement = sequence[throwIndex]
  var thrown = false
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }
  var result: [C.Element] = []
  let c = makeWrappedCollection(elements)
  let closureLifetimeTracker = LifetimeTracked(0)
  do {
    result = try c.sorted {
      (lhs, rhs) throws -> Bool in
      _blackHole(closureLifetimeTracker)
      if throwElement == extractValue(rhs).value {
        thrown = true
        throw SillyError.JazzHands
      }
      return lessImpl(extractValue(lhs).value, extractValue(rhs).value)
    }
  } catch {}

  // Check that the original collection is unchanged.
  expectEqualSequence(
    elements.map { $0.value },
    c.map { extract($0).value })

  // If `sorted` throws then result will be empty else
  // returned result must be sorted.
  if thrown {
    expectEqual(0, result.count)
  } else {
    // Check that the elements are sorted.
    let extractedResult = result.map(extract)
    for i in extractedResult.indices {
      if i != extractedResult.index(before: extractedResult.endIndex) {
        let first = extractedResult[i].value
        let second = extractedResult[extractedResult.index(after: i)].value
        let result = lessImpl(second, first)
        expectFalse(result)
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/DispatchesThrough_withUnsafeMutableBufferPointerIfSupported/WhereElementIsComparable") {
  let sequence = [ 5, 4, 3, 2, 1 ]
  let elements: [MinimalComparableValue] =
    zip(sequence, 0..<sequence.count).map {
      MinimalComparableValue($0, identity: $1)
    }
  let c = makeWrappedCollectionWithComparableElement(elements)

  let lc = LoggingMutableCollection(wrapping: c)

  let result = lc.sorted()
  let extractedResult = result.map(extractValueFromComparable)

  // This sort operation is not in-place.
  // The collection is copied into an array before sorting.
  expectEqual(
    0, lc.log._withUnsafeMutableBufferPointerIfSupported[type(of: lc)])
  expectEqual(
    0,
    lc.log._withUnsafeMutableBufferPointerIfSupportedNonNilReturns[type(of: lc)])

  expectEqualSequence([ 1, 2, 3, 4, 5 ], extractedResult.map { $0.value })
}

func checkSort_WhereElementIsComparable(
  sequence: [Int],
  equalImpl: @escaping ((Int, Int) -> Bool),
  lessImpl: @escaping ((Int, Int) -> Bool),
  verifyOrder: Bool
) {
  MinimalComparableValue.equalImpl.value = equalImpl
  MinimalComparableValue.lessImpl.value = lessImpl

  let extract = extractValueFromComparable
  let elements: [MinimalComparableValue] =
    zip(sequence, 0..<sequence.count).map {
      MinimalComparableValue($0, identity: $1)
    }
  let c = makeWrappedCollectionWithComparableElement(elements)
  let result = c.sorted()

  // Check that the original collection is unchanged.
  expectEqualSequence(
    elements.map { $0.value },
    c.map { extract($0).value })

  let extractedResult = result.map(extract)

  // Check that we didn't lose any elements.
  expectEqualsUnordered(
    0..<sequence.count,
    extractedResult.map { $0.identity })

  // Check that the elements are sorted.
  if verifyOrder {
    for i in extractedResult.indices {
      if i != extractedResult.index(before: extractedResult.endIndex) {
        let first = extractedResult[i].value
        let second = extractedResult[extractedResult.index(after: i)].value
        expectFalse(lessImpl(second, first))
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/WhereElementIsComparable") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      checkSort_WhereElementIsComparable(
        sequence: sequence,
        equalImpl: { $0 == $1 },
        lessImpl: { $0 < $1 },
        verifyOrder: true)
    }
  }
}

self.test("\(testNamePrefix).sorted/WhereElementIsComparable/InvalidOrderings") {
  withInvalidOrderings { (comparisonPredicate: @escaping (Int, Int) -> Bool) in
    for i in 0..<7 {
      forAllPermutations(i) { (sequence) in
        checkSort_WhereElementIsComparable(
          sequence: sequence,
          equalImpl: {
            !comparisonPredicate($0, $1) &&
            !comparisonPredicate($1, $0)
          },
          lessImpl: comparisonPredicate,
          verifyOrder: false)
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/ThrowingPredicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      for i in 0..<sequence.count {
      checkSortedPredicateThrow(
       sequence: sequence,
       lessImpl: { $0 < $1 },
       throwIndex: i)
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/ThrowingPredicateWithLargeNumberElements") {
  for sequence in largeElementSortTests {
      for i in 0..<sequence.count {
      checkSortedPredicateThrow(
       sequence: sequence,
       lessImpl: { $0 < $1 },
       throwIndex: i)
    }
  }
}


self.test("\(testNamePrefix).sorted/DispatchesThrough_withUnsafeMutableBufferPointerIfSupported/Predicate") {
  let sequence = [ 5, 4, 3, 2, 1 ]
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }
  let c = makeWrappedCollection(elements)

  let lc = LoggingMutableCollection(wrapping: c)

  let result = lc.sorted { extractValue($0).value < extractValue($1).value }
  let extractedResult = result.map(extractValue)

  // This sort operation is not in-place.
  // The collection is copied into an array before sorting.
  expectEqual(
    0, lc.log._withUnsafeMutableBufferPointerIfSupported[type(of: lc)])
  expectEqual(
    0,
    lc.log._withUnsafeMutableBufferPointerIfSupportedNonNilReturns[type(of: lc)])

  expectEqualSequence([ 1, 2, 3, 4, 5 ], extractedResult.map { $0.value })
}

func checkSort_Predicate(
  sequence: [Int],
  equalImpl: @escaping ((Int, Int) -> Bool),
  lessImpl: @escaping ((Int, Int) -> Bool),
  verifyOrder: Bool
) {
  let extract = extractValue
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }
  let c = makeWrappedCollection(elements)
  let closureLifetimeTracker = LifetimeTracked(0)
  let result = c.sorted {
    (lhs, rhs) in
    _blackHole(closureLifetimeTracker)
    return lessImpl(extractValue(lhs).value, extractValue(rhs).value)
  }

  // Check that the original collection is unchanged.
  expectEqualSequence(
    elements.map { $0.value },
    c.map { extract($0).value })

  let extractedResult = result.map(extract)

  // Check that we didn't lose any elements.
  expectEqualsUnordered(
    0..<sequence.count,
    extractedResult.map { $0.identity })

  // Check that the elements are sorted.
  if verifyOrder {
    for i in extractedResult.indices {
      if i != extractedResult.index(before: extractedResult.endIndex) {
        let first = extractedResult[i].value
        let second = extractedResult[extractedResult.index(after: i)].value
        expectFalse(lessImpl(second, first))
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/Predicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      checkSort_Predicate(
        sequence: sequence,
        equalImpl: { $0 == $1 },
        lessImpl: { $0 < $1 },
        verifyOrder: true)
    }
  }
}

self.test("\(testNamePrefix).sorted/Predicate/InvalidOrderings") {
  withInvalidOrderings { (comparisonPredicate: @escaping (Int, Int) -> Bool) in
    for i in 0..<7 {
      forAllPermutations(i) { (sequence) in
        checkSort_Predicate(
          sequence: sequence,
          equalImpl: {
            !comparisonPredicate($0, $1) &&
            !comparisonPredicate($1, $0)
          },
          lessImpl: comparisonPredicate,
          verifyOrder: false)
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/ThrowingPredicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      for i in 0..<sequence.count {
      checkSortedPredicateThrow(
       sequence: sequence,
       lessImpl: { $0 < $1 },
       throwIndex: i)
      }
    }
  }
}

self.test("\(testNamePrefix).sorted/ThrowingPredicateWithLargeNumberElements") {
  for sequence in largeElementSortTests {
      for i in 0..<sequence.count {
      checkSortedPredicateThrow(
       sequence: sequence,
       lessImpl: { $0 < $1 },
       throwIndex: i)
    }
  }
}


//===----------------------------------------------------------------------===//
// partition(by:)
//===----------------------------------------------------------------------===//

func checkPartition(
  sequence: [Int],
  pivotValue: Int,
  lessImpl: ((Int, Int) -> Bool),
  verifyOrder: Bool
) {
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }

  var c = makeWrappedCollection(elements)
  let closureLifetimeTracker = LifetimeTracked(0)
  let pivot = c.partition(by: { val in
    _blackHole(closureLifetimeTracker)
    return !lessImpl(extractValue(val).value, pivotValue)
  })

  // Check that we didn't lose any elements.
  let identities = c.map { extractValue($0).identity }
  expectEqualsUnordered(0..<sequence.count, identities)

  if verifyOrder {
    // All the elements in the first partition are less than the pivot
    // value.
    for i in c[c.startIndex..<pivot].indices {
      expectLT(extractValue(c[i]).value, pivotValue)
    }
    // All the elements in the second partition are greater or equal to
    // the pivot value.
    for i in c[pivot..<c.endIndex].indices {
      expectGE(extractValue(c[i]).value, pivotValue)
    }
  }
}

self.test("\(testNamePrefix).partition") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      checkPartition(
        sequence: sequence,
        pivotValue: sequence.first ?? 0,
        lessImpl: { $0 < $1 },
        verifyOrder: true)

      // Pivot value where all elements will pass the partitioning predicate
      checkPartition(
        sequence: sequence,
        pivotValue: Int.min,
        lessImpl: { $0 < $1 },
        verifyOrder: true)

      // Pivot value where no element will pass the partitioning predicate
      checkPartition(
        sequence: sequence,
        pivotValue: Int.max,
        lessImpl: { $0 < $1 },
        verifyOrder: true)
    }
  }
}

self.test("\(testNamePrefix).partition/InvalidOrderings") {
  withInvalidOrderings { (comparisonPredicate) in
    for i in 0..<7 {
      forAllPermutations(i) { (sequence) in
        checkPartition(
          sequence: sequence,
          pivotValue: sequence.first ?? 0,
          lessImpl: comparisonPredicate,
          verifyOrder: false)
      }
    }
  }
}

//===----------------------------------------------------------------------===//

  } // addMutableCollectionTests

  public func addMutableBidirectionalCollectionTests<
    C : BidirectionalCollection & MutableCollection,
    CollectionWithEquatableElement : BidirectionalCollection & MutableCollection,
    CollectionWithComparableElement : BidirectionalCollection & MutableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    makeCollectionOfComparable: @escaping ([CollectionWithComparableElement.Element]) -> CollectionWithComparableElement,
    wrapValueIntoComparable: @escaping (MinimalComparableValue) -> CollectionWithComparableElement.Element,
    extractValueFromComparable: @escaping ((CollectionWithComparableElement.Element) -> MinimalComparableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1,
    outOfBoundsSubscriptOffset: Int = 1,
    withUnsafeMutableBufferPointerIsSupported: Bool,
    isFixedLengthCollection: Bool
  ) where
    CollectionWithEquatableElement.Element : Equatable,
    CollectionWithComparableElement.Element : Comparable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addMutableCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      makeCollectionOfComparable: makeCollectionOfComparable,
      wrapValueIntoComparable: wrapValueIntoComparable,
      extractValueFromComparable: extractValueFromComparable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
      outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset,
      withUnsafeMutableBufferPointerIsSupported:
        withUnsafeMutableBufferPointerIsSupported,
      isFixedLengthCollection: isFixedLengthCollection,
      collectionIsBidirectional: true
    )

    addBidirectionalCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
      outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    testNamePrefix += String(describing: C.self)

//===----------------------------------------------------------------------===//
// subscript(_: Index)
//===----------------------------------------------------------------------===//

if resiliencyChecks.subscriptOnOutOfBoundsIndicesBehavior != .none {
  self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Left/NonEmpty/Set") {
    var c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
    var index = c.startIndex
    expectCrashLater()
    index = c.index(index, offsetBy: -outOfBoundsSubscriptOffset)
    c[index] = wrapValue(OpaqueValue(9999))
  }

  self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Left/Empty/Set") {
    var c = makeWrappedCollection([])
    var index = c.startIndex
    expectCrashLater()
    index = c.index(index, offsetBy: -outOfBoundsSubscriptOffset)
    c[index] = wrapValue(OpaqueValue(9999))
  }
}

//===----------------------------------------------------------------------===//
// reverse()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).reverse()") {
  for test in reverseTests {
    var c = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
    c.reverse()
    expectEqual(
      test.expected, c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// partition(by:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).partition/DispatchesThrough_withUnsafeMutableBufferPointerIfSupported") {
  let sequence = [ 5, 4, 3, 2, 1 ]
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }
  let c = makeWrappedCollection(elements)
  var lc = makeBufferAccessLoggingMutableCollection(wrapping: c)

  let closureLifetimeTracker = LifetimeTracked(0)
  let first = c.first
  let pivot = lc.partition(by: { val in
    _blackHole(closureLifetimeTracker)
    return !(extractValue(val).value < extractValue(first!).value)
  })

  expectEqual(
    1, lc.log._withUnsafeMutableBufferPointerIfSupported[type(of: lc)])
  expectEqual(
    withUnsafeMutableBufferPointerIsSupported ? 1 : 0,
    lc.log._withUnsafeMutableBufferPointerIfSupportedNonNilReturns[type(of: lc)])

  expectEqual(4, lc.distance(from: lc.startIndex, to: pivot))
  expectEqualsUnordered([1, 2, 3, 4], lc.prefix(upTo: pivot).map { extractValue($0).value })
  expectEqualsUnordered([5], lc.suffix(from: pivot).map { extractValue($0).value })
}

//===----------------------------------------------------------------------===//

  } // addMutableBidirectionalCollectionTests

  public func addMutableRandomAccessCollectionTests<
    C : RandomAccessCollection & MutableCollection,
    CollectionWithEquatableElement : RandomAccessCollection & MutableCollection,
    CollectionWithComparableElement : RandomAccessCollection & MutableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    makeCollectionOfComparable: @escaping ([CollectionWithComparableElement.Element]) -> CollectionWithComparableElement,
    wrapValueIntoComparable: @escaping (MinimalComparableValue) -> CollectionWithComparableElement.Element,
    extractValueFromComparable: @escaping ((CollectionWithComparableElement.Element) -> MinimalComparableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1,
    outOfBoundsSubscriptOffset: Int = 1,
    withUnsafeMutableBufferPointerIsSupported: Bool,
    isFixedLengthCollection: Bool
  ) where
    CollectionWithEquatableElement.Element : Equatable,
    CollectionWithComparableElement.Element : Comparable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addMutableBidirectionalCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      makeCollectionOfComparable: makeCollectionOfComparable,
      wrapValueIntoComparable: wrapValueIntoComparable,
      extractValueFromComparable: extractValueFromComparable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
      outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset,
      withUnsafeMutableBufferPointerIsSupported:
        withUnsafeMutableBufferPointerIsSupported,
      isFixedLengthCollection: isFixedLengthCollection)

    addRandomAccessCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
      outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    func makeWrappedCollectionWithComparableElement(
      _ elements: [MinimalComparableValue]
    ) -> CollectionWithComparableElement {
      return makeCollectionOfComparable(elements.map(wrapValueIntoComparable))
    }

    testNamePrefix += String(describing: C.self)

//===----------------------------------------------------------------------===//
// sort()
//===----------------------------------------------------------------------===//

func checkSortPredicateThrow(
  sequence: [Int],
  lessImpl: ((Int, Int) -> Bool),
  throwIndex: Int
) {
  let extract = extractValue
  let throwElement = sequence[throwIndex]
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }
  var c = makeWrappedCollection(elements)
  let closureLifetimeTracker = LifetimeTracked(0)
  do {
    try c.sort {
      (lhs, rhs) throws -> Bool in
      _blackHole(closureLifetimeTracker)
      if throwElement == extractValue(rhs).value {
        throw SillyError.JazzHands
      }
      return lessImpl(extractValue(lhs).value, extractValue(rhs).value)
    }
  } catch {}

  //Check no element should lost and added
  expectEqualsUnordered(
    sequence,
    c.map { extract($0).value })
}


func checkSortInPlace_WhereElementIsComparable(
  sequence: [Int],
  equalImpl: @escaping ((Int, Int) -> Bool),
  lessImpl: @escaping ((Int, Int) -> Bool),
  verifyOrder: Bool
) {
  MinimalComparableValue.equalImpl.value = equalImpl
  MinimalComparableValue.lessImpl.value = lessImpl

  let extract = extractValueFromComparable
  let elements: [MinimalComparableValue] =
    zip(sequence, 0..<sequence.count).map {
      MinimalComparableValue($0, identity: $1)
    }

  var c = makeWrappedCollectionWithComparableElement(elements)
  c.sort()

  let extractedResult = c.map(extract)

  // Check that we didn't lose any elements.
  expectEqualsUnordered(
    0..<sequence.count,
    extractedResult.map { $0.identity })

  // Check that the elements are sorted.
  if verifyOrder {
    for i in extractedResult.indices {
      if i != extractedResult.index(before: extractedResult.endIndex) {
        let first = extractedResult[i].value
        let second = extractedResult[extractedResult.index(after: i)].value
        expectFalse(lessImpl(second, first))
      }
    }
  }
}

self.test("\(testNamePrefix).sort/WhereElementIsEquatable") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      checkSortInPlace_WhereElementIsComparable(
        sequence: sequence,
        equalImpl: { $0 == $1 },
        lessImpl: { $0 < $1 },
        verifyOrder: true)
    }
  }
}

self.test("\(testNamePrefix).sort/WhereElementIsEquatable/InvalidOrderings") {
  withInvalidOrderings { (comparisonPredicate : @escaping (Int, Int) -> Bool) in
    for i in 0..<7 {
      forAllPermutations(i) { (sequence) in
        checkSortInPlace_WhereElementIsComparable(
          sequence: sequence,
          equalImpl: {
            !comparisonPredicate($0, $1) &&
            !comparisonPredicate($1, $0)
          },
          lessImpl: comparisonPredicate,
          verifyOrder: false)
      }
    }
  }
}

self.test("\(testNamePrefix).sort/ThrowingPredicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      for i in 0..<sequence.count {
        checkSortPredicateThrow(
         sequence: sequence,
         lessImpl: { $0 < $1 },
         throwIndex: i)
      }
    }
  }
}

self.test("\(testNamePrefix).sort/ThrowingPredicateWithLargeNumberElements") {
  for sequence in largeElementSortTests {
    for i in 0..<sequence.count {
      checkSortPredicateThrow(
        sequence: sequence,
        lessImpl: { $0 < $1 },
        throwIndex: i)
    }
  }
}


func checkSortInPlace_Predicate(
  sequence: [Int],
  equalImpl: @escaping ((Int, Int) -> Bool),
  lessImpl: @escaping ((Int, Int) -> Bool),
  verifyOrder: Bool
) {
  let extract = extractValue
  let elements: [OpaqueValue<Int>] =
    zip(sequence, 0..<sequence.count).map {
      OpaqueValue($0, identity: $1)
    }

  var c = makeWrappedCollection(elements)
  let closureLifetimeTracker = LifetimeTracked(0)
  c.sort {
    (lhs, rhs) in
    _blackHole(closureLifetimeTracker)
    return lessImpl(extractValue(lhs).value, extractValue(rhs).value)
  }

  let extractedResult = c.map(extract)

  // Check that we didn't lose any elements.
  expectEqualsUnordered(
    0..<sequence.count,
    extractedResult.map { $0.identity })

  // Check that the elements are sorted.
  if verifyOrder {
    for i in extractedResult.indices {
      if i != extractedResult.index(before: extractedResult.endIndex) {
        let first = extractedResult[i].value
        let second = extractedResult[extractedResult.index(after: i)].value
        expectFalse(lessImpl(second, first))
      }
    }
  }
}

self.test("\(testNamePrefix).sort/Predicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      checkSortInPlace_Predicate(
        sequence: sequence,
        equalImpl: { $0 == $1 },
        lessImpl: { $0 < $1 },
        verifyOrder: true)
    }
  }
}

self.test("\(testNamePrefix).sort/Predicate/InvalidOrderings") {
  withInvalidOrderings { (comparisonPredicate : @escaping (Int, Int) -> Bool) in
    for i in 0..<7 {
      forAllPermutations(i) { (sequence) in
        checkSortInPlace_Predicate(
          sequence: sequence,
          equalImpl: {
            !comparisonPredicate($0, $1) &&
            !comparisonPredicate($1, $0)
          },
          lessImpl: comparisonPredicate,
          verifyOrder: false)
      }
    }
  }
}

self.test("\(testNamePrefix).sort/ThrowingPredicate") {
  for test in partitionExhaustiveTests {
    forAllPermutations(test.sequence) { (sequence) in
      for i in 0..<sequence.count {
        checkSortPredicateThrow(
         sequence: sequence,
         lessImpl: { $0 < $1 },
         throwIndex: i)
      }
    }
  }
}

self.test("\(testNamePrefix).sort/ThrowingPredicateWithLargeNumberElements") {
  for sequence in largeElementSortTests {
    for i in 0..<sequence.count {
      checkSortPredicateThrow(
        sequence: sequence,
        lessImpl: { $0 < $1 },
        throwIndex: i)
    }
  }
}

//===----------------------------------------------------------------------===//

  } // addMutableRandomAccessCollectionTests
}


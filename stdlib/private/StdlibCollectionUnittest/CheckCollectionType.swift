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

internal var checksAdded: Set<String> = []

public struct SubscriptRangeTest {
  public let expected: [OpaqueValue<Int>]
  public let collection: [OpaqueValue<Int>]
  public let bounds: Range<Int>
  public let count: Int
  public let loc: SourceLoc

  public var isEmpty: Bool { return count == 0 }

  public func bounds<C : Collection>(in c: C) -> Range<C.Index> {
    let i = c.startIndex
    return c.index(i, offsetBy: bounds.lowerBound) ..<
           c.index(i, offsetBy: bounds.upperBound)
  }

  public init(
    expected: [Int], collection: [Int], bounds: Range<Int>,
    count: Int,
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected.map(OpaqueValue.init)
    self.collection = collection.map(OpaqueValue.init)
    self.bounds = bounds
    self.count = count
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct PrefixThroughTest {
  public var collection: [Int]
  public let position: Int
  public let expected: [Int]
  public let loc: SourceLoc

  init(
    collection: [Int], position: Int, expected: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection
    self.position = position
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "prefix(through:) test data")
  }
}

public struct PrefixUpToTest {
  public var collection: [Int]
  public let end: Int
  public let expected: [Int]
  public let loc: SourceLoc

  public init(
    collection: [Int], end: Int, expected: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection
    self.end = end
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "prefix(upTo:) test data")
  }
}

internal struct RemoveFirstNTest {
  let collection: [Int]
  let numberToRemove: Int
  let expectedCollection: [Int]
  let loc: SourceLoc

  init(
    collection: [Int], numberToRemove: Int, expectedCollection: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection
    self.numberToRemove = numberToRemove
    self.expectedCollection = expectedCollection
    self.loc = SourceLoc(file, line, comment: "removeFirst(n: Int) test data")
  }
}

public struct SuffixFromTest {
  public var collection: [Int]
  public let start: Int
  public let expected: [Int]
  public let loc: SourceLoc

  init(
    collection: [Int], start: Int, expected: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection
    self.start = start
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "suffix(from:) test data")
  }
}

public struct FindLastTest {
  public let expected: Int?
  public let comparisons: Int
  public let element: MinimalEquatableValue
  public let sequence: [MinimalEquatableValue]
  public let loc: SourceLoc
  
  public init(
    expected: Int?, comparisons: Int, element: Int, sequence: [Int],
    file: String = #file, line: UInt = #line
    ) {
    self.expected = expected
    self.comparisons = comparisons
    self.element = MinimalEquatableValue(element)
    self.sequence = sequence.enumerated().map {
      return MinimalEquatableValue($1, identity: $0)
    }
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public let subscriptRangeTests = [
  // Slice an empty collection.
  SubscriptRangeTest(
    expected: [],
    collection: [],
    bounds: 0..<0,
    count: 0),

  // Slice to the full extent.
  SubscriptRangeTest(
    expected: [1010],
    collection: [1010],
    bounds: 0..<1,
    count: 1),
  SubscriptRangeTest(
    expected: [1010, 2020, 3030],
    collection: [1010, 2020, 3030],
    bounds: 0..<3,
    count: 3),
  SubscriptRangeTest(
    expected: [1010, 2020, 3030, 4040, 5050],
    collection: [1010, 2020, 3030, 4040, 5050],
    bounds: 0..<5,
    count: 5),

  // Slice an empty prefix.
  SubscriptRangeTest(
    expected: [],
    collection: [1010, 2020, 3030],
    bounds: 0..<0,
    count: 3),

  // Slice a prefix.
  SubscriptRangeTest(
    expected: [1010, 2020],
    collection: [1010, 2020, 3030],
    bounds: 0..<2,
    count: 3),
  SubscriptRangeTest(
    expected: [1010, 2020],
    collection: [1010, 2020, 3030, 4040, 5050],
    bounds: 0..<2,
    count: 5),

  // Slice an empty suffix.
  SubscriptRangeTest(
    expected: [],
    collection: [1010, 2020, 3030],
    bounds: 3..<3,
    count: 3),

  // Slice a suffix.
  SubscriptRangeTest(
    expected: [2020, 3030],
    collection: [1010, 2020, 3030],
    bounds: 1..<3,
    count: 3),
  SubscriptRangeTest(
    expected: [4040, 5050],
    collection: [1010, 2020, 3030, 4040, 5050],
    bounds: 3..<5,
    count: 5),

  // Slice an empty range in the middle.
  SubscriptRangeTest(
    expected: [],
    collection: [1010, 2020, 3030],
    bounds: 1..<1,
    count: 3),
  SubscriptRangeTest(
    expected: [],
    collection: [1010, 2020, 3030],
    bounds: 2..<2,
    count: 3),

  // Slice the middle part.
  SubscriptRangeTest(
    expected: [2020],
    collection: [1010, 2020, 3030],
    bounds: 1..<2,
    count: 3),
  SubscriptRangeTest(
    expected: [3030],
    collection: [1010, 2020, 3030, 4040],
    bounds: 2..<3,
    count: 4),
  SubscriptRangeTest(
    expected: [2020, 3030, 4040],
    collection: [1010, 2020, 3030, 4040, 5050, 6060],
    bounds: 1..<4,
    count: 6),
]

public let prefixUpToTests = [
  PrefixUpToTest(
    collection: [],
    end: 0,
    expected: []
  ),
  PrefixUpToTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    end: 3,
    expected: [1010, 2020, 3030]
  ),
  PrefixUpToTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    end: 5,
    expected: [1010, 2020, 3030, 4040, 5050]
  ),
]

public let prefixThroughTests = [
  PrefixThroughTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    position: 0,
    expected: [1010]
  ),
  PrefixThroughTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    position: 2,
    expected: [1010, 2020, 3030]
  ),
  PrefixThroughTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    position: 4,
    expected: [1010, 2020, 3030, 4040, 5050]
  ),
]

public let suffixFromTests = [
  SuffixFromTest(
    collection: [],
    start: 0,
    expected: []
  ),
  SuffixFromTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    start: 0,
    expected: [1010, 2020, 3030, 4040, 5050]
  ),
  SuffixFromTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    start: 3,
    expected: [4040, 5050]
  ),
  SuffixFromTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    start: 5,
    expected: []
  ),
]

let removeFirstTests: [RemoveFirstNTest] = [
  RemoveFirstNTest(
    collection: [1010],
    numberToRemove: 0,
    expectedCollection: [1010]
  ),
  RemoveFirstNTest(
    collection: [1010],
    numberToRemove: 1,
    expectedCollection: []
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 0,
    expectedCollection: [1010, 2020, 3030, 4040, 5050]
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 1,
    expectedCollection: [2020, 3030, 4040, 5050]
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 2,
    expectedCollection: [3030, 4040, 5050]
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 3,
    expectedCollection: [4040, 5050]
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 4,
    expectedCollection: [5050]
  ),
  RemoveFirstNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 5,
    expectedCollection: []
  ),
]

let findLastTests = [
  FindLastTest(
    expected: nil,
    comparisons: 0,
    element: 42,
    sequence: []),
  
  FindLastTest(
    expected: nil,
    comparisons: 1,
    element: 42,
    sequence: [ 1010 ]),
  FindLastTest(
    expected: 0,
    comparisons: 1,
    element: 1010,
    sequence: [ 1010 ]),
  
  FindLastTest(
    expected: nil,
    comparisons: 2,
    element: 42,
    sequence: [ 1010, 1010 ]),
  FindLastTest(
    expected: 1,
    comparisons: 1,
    element: 1010,
    sequence: [ 1010, 1010 ]),
  
  FindLastTest(
    expected: nil,
    comparisons: 4,
    element: 42,
    sequence: [ 1010, 2020, 3030, 4040 ]),
  FindLastTest(
    expected: 0,
    comparisons: 4,
    element: 1010,
    sequence: [ 1010, 2020, 3030, 4040 ]),
  FindLastTest(
    expected: 1,
    comparisons: 3,
    element: 2020,
    sequence: [ 1010, 2020, 3030, 4040 ]),
  FindLastTest(
    expected: 2,
    comparisons: 2,
    element: 3030,
    sequence: [ 1010, 2020, 3030, 4040 ]),
  FindLastTest(
    expected: 3,
    comparisons: 1,
    element: 4040,
    sequence: [ 1010, 2020, 3030, 4040 ]),
  
  FindLastTest(
    expected: 3,
    comparisons: 2,
    element: 2020,
    sequence: [ 1010, 2020, 3030, 2020, 4040 ]),
]

extension Collection {
  public func nthIndex(_ offset: Int) -> Index {
    return self.index(self.startIndex, offsetBy: offset)
  }
}

public struct DistanceFromToTest {
  public let startOffset: Int
  public let endOffset: Int
  public var expectedDistance : Int { return endOffset - startOffset }
  public let loc: SourceLoc

  public init(
    start: Int, end: Int,
    file: String = #file, line: UInt = #line
  ) {
    self.startOffset = start
    self.endOffset = end
    self.loc = SourceLoc(file, line, comment: "distance(from:to:) test data")
  }
}

public struct IndexOffsetByTest {
  public let startOffset: Int
  public let distance: Int
  public let limit: Int?
  public let expectedOffset: Int?

  public let loc: SourceLoc

  public init(
    startOffset: Int, distance: Int, expectedOffset: Int?,
    limitedBy limit: Int? = nil,
    file: String = #file, line: UInt = #line
  ) {
    self.startOffset = startOffset
    self.distance = distance
    self.expectedOffset = expectedOffset
    self.limit = limit
    self.loc = SourceLoc(file, line, comment: "index(_:offsetBy:) test data")
  }
}

public let distanceFromToTests = [
  // 0 - 1: no distance.
  DistanceFromToTest(start: 0, end: 0),
  DistanceFromToTest(start: 10, end: 10),
  // 2 - 3: forward distance.
  DistanceFromToTest(start: 10, end: 13),
  DistanceFromToTest(start: 7, end: 10),
  // 4 - 6: backward distance.
  DistanceFromToTest(start: 12, end: 4),
  DistanceFromToTest(start: 8, end: 2),
  DistanceFromToTest(start: 19, end: 0)
]

public let indexOffsetByTests = [
  IndexOffsetByTest(startOffset: 0, distance: 0, expectedOffset: 0),
  IndexOffsetByTest(startOffset: 7, distance: 0, expectedOffset: 7),
  IndexOffsetByTest(startOffset: 0, distance: -1, expectedOffset: -1),
  IndexOffsetByTest(startOffset: 4, distance: -9, expectedOffset: -5),
  IndexOffsetByTest(startOffset: 8, distance: -2, expectedOffset: 6),
  IndexOffsetByTest(startOffset: 4, distance: -4, expectedOffset: 0),
  IndexOffsetByTest(startOffset: 3, distance: 12, expectedOffset: 15),
  IndexOffsetByTest(startOffset: 9, distance: 1, expectedOffset: 10),
  IndexOffsetByTest(startOffset: 2, distance: 4, expectedOffset: 6),
  IndexOffsetByTest(startOffset: 0, distance: 9, expectedOffset: 9),
  IndexOffsetByTest(
    startOffset: 0, distance: 0, expectedOffset: 0, limitedBy: 0),
  IndexOffsetByTest(
    startOffset: 0, distance: 0, expectedOffset: 0, limitedBy: 10),
  IndexOffsetByTest(
    startOffset: 0, distance: 10, expectedOffset: nil, limitedBy: 0),
  IndexOffsetByTest(
    startOffset: 0, distance: -10, expectedOffset: nil, limitedBy: 0),
  IndexOffsetByTest(
    startOffset: 0, distance: 10, expectedOffset: 10, limitedBy: 10),
  IndexOffsetByTest(
    startOffset: 0, distance: 20, expectedOffset: nil, limitedBy: 10),
  IndexOffsetByTest(
    startOffset: 10, distance: -20, expectedOffset: nil, limitedBy: 0),
  IndexOffsetByTest(
    startOffset: 5, distance: 5, expectedOffset: 10, limitedBy: 2),
  IndexOffsetByTest(
    startOffset: 5, distance: 5, expectedOffset: nil, limitedBy: 5),
  IndexOffsetByTest(
    startOffset: 5, distance: -5, expectedOffset: 0, limitedBy: 7),
  IndexOffsetByTest(
    startOffset: 5, distance: -5, expectedOffset: nil, limitedBy: 5)
]

public struct IndexAfterTest {
  public let start: Int
  public let end: Int
  public let loc: SourceLoc

  public init(start: Int, end: Int, file: String = #file, line: UInt = #line) {
    self.start = start
    self.end = end
    self.loc = SourceLoc(file, line,
      comment: "index(after:) and formIndex(after:) test data")
  }
}

public let indexAfterTests = [
  IndexAfterTest(start: 0, end: 1),
  IndexAfterTest(start: 0, end: 10),
  IndexAfterTest(start: 5, end: 12),
  IndexAfterTest(start: -58, end: -54),
]

internal func _allIndices<C : Collection>(
  into c: C, in bounds: Range<C.Index>
) -> [C.Index] {
  var result: [C.Index] = []
  var i = bounds.lowerBound
  while i != bounds.upperBound {
    result.append(i)
    i = c.index(after: i)
  }
  return result
}

internal enum _SubSequenceSubscriptOnIndexMode {
  case inRange
  case outOfRangeToTheLeft
  case outOfRangeToTheRight
  case baseEndIndex
  case sliceEndIndex

  static var all: [_SubSequenceSubscriptOnIndexMode] {
    return [
      .inRange,
      .outOfRangeToTheLeft,
      .outOfRangeToTheRight,
      .baseEndIndex,
      .sliceEndIndex,
    ]
  }
}

internal enum _SubSequenceSubscriptOnRangeMode {
  case inRange
  case outOfRangeToTheLeftEmpty
  case outOfRangeToTheLeftNonEmpty
  case outOfRangeToTheRightEmpty
  case outOfRangeToTheRightNonEmpty
  case outOfRangeBothSides
  case baseEndIndex

  static var all: [_SubSequenceSubscriptOnRangeMode] {
    return [
      .inRange,
      .outOfRangeToTheLeftEmpty,
      .outOfRangeToTheLeftNonEmpty,
      .outOfRangeToTheRightEmpty,
      .outOfRangeToTheRightNonEmpty,
      .outOfRangeBothSides,
      .baseEndIndex,
    ]
  }
}

extension TestSuite {
  public func addCollectionTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool
 = false
  ) where

  C : Collection,
  CollectionWithEquatableElement : Collection,
  CollectionWithEquatableElement.Element : Equatable
 {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addSequenceTests(
      testNamePrefix,
      makeSequence: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeSequenceOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    func makeWrappedCollectionWithEquatableElement(
      _ elements: [MinimalEquatableValue]
    ) -> CollectionWithEquatableElement {
      return makeCollectionOfEquatable(elements.map(wrapValueIntoEquatable))
    }

    testNamePrefix += String(describing: C.self)

    //===------------------------------------------------------------------===//
    // generate()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).generate()/semantics") {
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        for _ in 0..<3 {
          checkSequence(
            test.collection.map(wrapValue),
            c,
            resiliencyChecks: .none) {
            extractValue($0).value == extractValue($1).value
          }
        }
      }
    }

    //===------------------------------------------------------------------===//
    // Index
    //===------------------------------------------------------------------===//

    if resiliencyChecks.creatingOutOfBoundsIndicesBehavior != .none {
      self.test("\(testNamePrefix).Index/OutOfBounds/Right/NonEmpty") {
        let c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
        let index = c.endIndex
        expectCrashLater()
        _blackHole(c.index(index, offsetBy: outOfBoundsIndexOffset))
      }

      self.test("\(testNamePrefix).Index/OutOfBounds/Right/Empty") {
        let c = makeWrappedCollection([])
        let index = c.endIndex
        expectCrashLater()
        _blackHole(c.index(index, offsetBy: outOfBoundsIndexOffset))
      }
    }

    //===------------------------------------------------------------------===//
    // subscript(_: Index)
    //===------------------------------------------------------------------===//

    if resiliencyChecks.subscriptOnOutOfBoundsIndicesBehavior != .none {
      self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Right/NonEmpty/Get") {
        let c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
        var index = c.endIndex
        expectCrashLater()
        index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
        _blackHole(c[index])
      }

      self.test("\(testNamePrefix).subscript(_: Index)/OutOfBounds/Right/Empty/Get") {
        let c = makeWrappedCollection([])
        var index = c.endIndex
        expectCrashLater()
        index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
        _blackHole(c[index])
      }

      let tests = cartesianProduct(
        subscriptRangeTests,
        _SubSequenceSubscriptOnIndexMode.all)

      self.test("\(testNamePrefix).SubSequence.subscript(_: Index)/Get/OutOfBounds")
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
              expectEqual(
                elements[i].value,
                extractValue(slice[index]).value)
              expectEqual(
                extractValue(base[index]).value,
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
        _blackHole(slice[index])
      }
    }

    //===------------------------------------------------------------------===//
    // subscript(_: Range)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).subscript(_: Range)/Get/semantics") {
      for test in subscriptRangeTests {
        let base = makeWrappedCollection(test.collection)
        let sliceBounds = test.bounds(in: base)
        let slice = base[sliceBounds]

        expectEqual(sliceBounds.lowerBound, slice.startIndex)
        expectEqual(sliceBounds.upperBound, slice.endIndex)
        /*
        // TODO: swift-3-indexing-model: uncomment the following.
        // FIXME: improve checkForwardCollection to check the SubSequence type.
        checkCollection(
          expected: test.expected.map(wrapValue),
          slice,
          resiliencyChecks: .none) {
          extractValue($0).value == extractValue($1).value
        }
        */
      }
    }

    if resiliencyChecks.subscriptRangeOnOutOfBoundsRangesBehavior != .none {
      self.test("\(testNamePrefix).subscript(_: Range)/OutOfBounds/Right/NonEmpty/Get") {
        let c = makeWrappedCollection([ 1010, 2020, 3030 ].map(OpaqueValue.init))
        var index = c.endIndex
        expectCrashLater()
        index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
        _blackHole(c[index..<index])
      }

      self.test("\(testNamePrefix).subscript(_: Range)/OutOfBounds/Right/Empty/Get") {
        let c = makeWrappedCollection([])
        var index = c.endIndex
        expectCrashLater()
        index = c.index(index, offsetBy: outOfBoundsSubscriptOffset)
        _blackHole(c[index..<index])
      }

      let tests = cartesianProduct(
        subscriptRangeTests,
        _SubSequenceSubscriptOnRangeMode.all)

      self.test(
        "\(testNamePrefix).SubSequence.subscript(_: Range)/Get/OutOfBounds")
        .forEach(in: tests) {
        (test, mode) in
        let elements = test.collection
        let sliceFromLeft = test.bounds.lowerBound
        let sliceFromRight = elements.count - test.bounds.upperBound
        print(
          "\(elements)/sliceFromLeft=\(sliceFromLeft)"
          + "/sliceFromRight=\(sliceFromRight)")
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
                let subSlice = slice[subSliceStartIndex..<subSliceEndIndex]
                for (k, index) in subSlice.indices.enumerated() {
                  expectEqual(
                    elements[i + k].value,
                    extractValue(subSlice[index]).value)
                  expectEqual(
                    extractValue(base[index]).value,
                    extractValue(subSlice[index]).value)
                  expectEqual(
                    extractValue(slice[index]).value,
                    extractValue(subSlice[index]).value)
                }
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

        expectCrashLater()
        _blackHole(slice[bounds])
      }
    }

    // FIXME: swift-3-indexing-model - add tests for the following?
    //          _failEarlyRangeCheck(index: Index, bounds: Range<Index>)
    //          _failEarlyRangeCheck(range: Range<Index>, bounds: Range<Index>)

    //===------------------------------------------------------------------===//
    // isEmpty
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).isEmpty/semantics") {
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        expectEqual(test.isEmpty, c.isEmpty)
      }
    }

    //===------------------------------------------------------------------===//
    // count
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).count/semantics") {
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        expectEqual(test.count, c.count)
      }
    }

    //===------------------------------------------------------------------===//
    // firstIndex(of:)/firstIndex(where:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).firstIndex(of:)/semantics") {
      for test in findTests {
        let c = makeWrappedCollectionWithEquatableElement(test.sequence)
        var result = c.firstIndex(of: wrapValueIntoEquatable(test.element))
        expectType(
          Optional<CollectionWithEquatableElement.Index>.self,
          &result)
        let zeroBasedIndex = result.map {
          c.distance(from: c.startIndex, to: $0)
        }
        expectEqual(
          test.expected,
          zeroBasedIndex,
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).firstIndex(where:)/semantics") {
      for test in findTests {
        let closureLifetimeTracker = LifetimeTracked(0)
        expectEqual(1, LifetimeTracked.instances)
        let c = makeWrappedCollectionWithEquatableElement(test.sequence)
        let result = c.firstIndex {
          (candidate) in
          _blackHole(closureLifetimeTracker)
          return
            extractValueFromEquatable(candidate).value == test.element.value
        }
        let zeroBasedIndex = result.map {
          c.distance(from: c.startIndex, to: $0)
        }
        expectEqual(
          test.expected,
          zeroBasedIndex,
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // first
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).first") {
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        let result = c.first
        if test.isEmpty {
          expectNil(result)
        } else {
          expectOptionalEqual(
            test.collection[0],
            result.map(extractValue)
          ) { $0.value == $1.value }
        }
      }
    }

    //===------------------------------------------------------------------===//
    // indices
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).indices") {
      // TODO: swift-3-indexing-model: improve this test.  `indices`
      // is not just a `Range` anymore, it can be anything.
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        let indices = c.indices
        expectEqual(c.startIndex, indices.startIndex)
        expectEqual(c.endIndex, indices.endIndex)
      }
    }

    //===------------------------------------------------------------------===//
    // dropFirst()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).dropFirst/semantics") {
      for test in dropFirstTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.dropFirst(test.dropElements)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // dropLast()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).dropLast/semantics") {
      for test in dropLastTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.dropLast(test.dropElements)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // prefix()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).prefix/semantics") {
      for test in prefixTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.prefix(test.maxLength)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // suffix()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).suffix/semantics") {
      for test in suffixTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.suffix(test.maxLength)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // split()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).split/semantics") {
      for test in splitTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.split(
          maxSplits: test.maxSplits,
          omittingEmptySubsequences: test.omittingEmptySubsequences
        ) {
          extractValue($0).value == test.separator
        }
        expectEqualSequence(test.expected, result.map {
          $0.map {
            extractValue($0).value
          }
        },
        stackTrace: SourceLocStack().with(test.loc)) { $0 == $1 }
      }
    }

    //===------------------------------------------------------------------===//
    // prefix(through:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).prefix(through:)/semantics") {
      for test in prefixThroughTests {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        let index = c.index(
          c.startIndex, offsetBy: test.position)
        let result = c.prefix(through: index)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // prefix(upTo:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).prefix(upTo:)/semantics") {
      for test in prefixUpToTests {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        let index = c.index(c.startIndex, offsetBy: test.end)
        let result = c.prefix(upTo: index)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // suffix(from:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).suffix(from:)/semantics") {
      for test in suffixFromTests {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        let index = c.index(c.startIndex, offsetBy: test.start)
        let result = c.suffix(from: index)

        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // removeFirst()/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeFirst()/slice/semantics") {
      for test in removeFirstTests.filter({ $0.numberToRemove == 1 }) {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        var slice = c[...]
        let survivingIndices = _allIndices(
          into: slice,
          in: slice.index(after: slice.startIndex)..<slice.endIndex)
        let removedElement = slice.removeFirst()
        expectEqual(test.collection.first, extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the tail of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "removeFirst() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection,
          c.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).removeFirst()/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      _ = slice.removeFirst() // Should trap.
    }

    //===------------------------------------------------------------------===//
    // removeFirst(n: Int)/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeFirst(n: Int)/slice/semantics") {
      for test in removeFirstTests {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        var slice = c[...]
        let survivingIndices = _allIndices(
          into: slice,
          in: slice.index(
            slice.startIndex,
            offsetBy: test.numberToRemove) ..< slice.endIndex
        )
        slice.removeFirst(test.numberToRemove)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the tail of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "removeFirst() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection,
          c.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).removeFirst(n: Int)/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeFirst(1) // Should trap.
    }

    self.test(
      "\(testNamePrefix).removeFirst(n: Int)/slice/removeNegative/semantics") {
      let c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeFirst(-1) // Should trap.
    }

    self.test(
      "\(testNamePrefix).removeFirst(n: Int)/slice/removeTooMany/semantics") {
      let c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeFirst(3) // Should trap.
    }

    //===------------------------------------------------------------------===//
    // popFirst()/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).popFirst()/slice/semantics") {
      // This can just reuse the test data for removeFirst()
      for test in removeFirstTests.filter({ $0.numberToRemove == 1 }) {
        let c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        var slice = c[...]
        let survivingIndices = _allIndices(
          into: slice,
          in: slice.index(after: slice.startIndex)..<slice.endIndex)
        let removedElement = slice.popFirst()!
        expectEqual(test.collection.first, extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "popFirst() shouldn't mutate the tail of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "popFirst() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection,
          c.map { extractValue($0).value },
          "popFirst() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).popFirst()/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectNil(slice.popFirst())
    }

    //===------------------------------------------------------------------===//
    self.addCommonTests(
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
    collectionIsBidirectional: collectionIsBidirectional)
  } // addCollectionTests

  public func addBidirectionalCollectionTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool
 = true
  ) where

  C : BidirectionalCollection,
  CollectionWithEquatableElement : BidirectionalCollection,
  CollectionWithEquatableElement.Element : Equatable
 {

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
    collectionIsBidirectional: collectionIsBidirectional)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    func makeWrappedCollectionWithEquatableElement(
      _ elements: [MinimalEquatableValue]
    ) -> CollectionWithEquatableElement {
      return makeCollectionOfEquatable(elements.map(wrapValueIntoEquatable))
    }

    testNamePrefix += String(describing: C.self)

    // FIXME: swift-3-indexing-model - add tests for the follow?
    //          index(before: of i: Index) -> Index
    //          formIndex(before: i: inout Index)

    // FIXME: swift-3-indexing-model -
    //   enhance the following for negative direction?
    //          advance(i: Index, by n: Int) -> Index
    //          advance(
    //             i: Index, by n: Int, limitedBy: Index) -> Index
    //          distance(from start: Index, to end: Index) -> Int

    //===------------------------------------------------------------------===//
    // last
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).last") {
      for test in subscriptRangeTests {
        let c = makeWrappedCollection(test.collection)
        let result = c.last
        if test.isEmpty {
          expectNil(result)
        } else {
          expectOptionalEqual(
            test.collection[test.count - 1],
            result.map(extractValue)
          ) { $0.value == $1.value }
        }
      }
    }

    //===------------------------------------------------------------------===//
    // last(where:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).last(where:)/semantics") {
      for test in findLastTests {
        let c = makeWrappedCollectionWithEquatableElement(test.sequence)
        var closureCounter = 0
        let closureLifetimeTracker = LifetimeTracked(0)
        let found = c.last(where: {
          _blackHole(closureLifetimeTracker)
          closureCounter += 1
          return $0 == wrapValueIntoEquatable(test.element)
        })
        expectEqual(
          test.expected == nil ? nil : wrapValueIntoEquatable(test.element),
          found,
          stackTrace: SourceLocStack().with(test.loc))
        expectEqual(
          test.comparisons,
          closureCounter,
          stackTrace: SourceLocStack().with(test.loc))
        if let expectedIdentity = test.expected {
          expectEqual(
            expectedIdentity, extractValueFromEquatable(found!).identity,
            "last(where:) should find only the first element matching its predicate")
        }
      }
    }

    //===------------------------------------------------------------------===//
    // lastIndex(of:)/lastIndex(where:)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).lastIndex(of:)/semantics") {
      for test in findLastTests {
        let c = makeWrappedCollectionWithEquatableElement(test.sequence)
        MinimalEquatableValue.timesEqualEqualWasCalled = 0
        let wrappedElement = wrapValueIntoEquatable(test.element)
        var result = c.lastIndex(of: wrappedElement)
        expectType(
          Optional<CollectionWithEquatableElement.Index>.self,
          &result)
        let zeroBasedIndex = result.map {
          c.distance(from: c.startIndex, to: $0)
        }
        expectEqual(
          test.expected,
          zeroBasedIndex,
          stackTrace: SourceLocStack().with(test.loc))
        if wrappedElement is MinimalEquatableValue {
          expectEqual(
            test.comparisons,
            MinimalEquatableValue.timesEqualEqualWasCalled,
            stackTrace: SourceLocStack().with(test.loc))
        }
      }
    }

    self.test("\(testNamePrefix).lastIndex(where:)/semantics") {
      for test in findLastTests {
        let closureLifetimeTracker = LifetimeTracked(0)
        expectEqual(1, LifetimeTracked.instances)
        let c = makeWrappedCollectionWithEquatableElement(test.sequence)
        var closureCounter = 0
        let result = c.lastIndex(where: {
          (candidate) in
          _blackHole(closureLifetimeTracker)
          closureCounter += 1
          return
            extractValueFromEquatable(candidate).value == test.element.value
        })
        let zeroBasedIndex = result.map {
          c.distance(from: c.startIndex, to: $0)
        }
        expectEqual(
          test.expected,
          zeroBasedIndex,
          stackTrace: SourceLocStack().with(test.loc))
        expectEqual(
          test.comparisons,
          closureCounter,
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // removeLast()/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeLast()/slice/semantics") {
      for test in removeLastTests.filter({ $0.numberToRemove == 1 }) {
        let c = makeWrappedCollection(test.collection)
        var slice = c[...]
        let survivingIndices = _allIndices(
            into: slice,
          in: slice.startIndex
            ..< slice.index(
              slice.endIndex, offsetBy: -test.numberToRemove)
        )
        let removedElement = slice.removeLast()
        expectEqual(
          test.collection.last!.value,
          extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "removeLast() shouldn't mutate the head of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "removeLast() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection.map { $0.value },
          c.map { extractValue($0).value },
          "removeLast() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).removeLast()/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      _ = slice.removeLast() // Should trap.
    }

    //===------------------------------------------------------------------===//
    // removeLast(n: Int)/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeLast(n: Int)/slice/semantics") {
      for test in removeLastTests {
        let c = makeWrappedCollection(test.collection)
        var slice = c[...]
        let survivingIndices = _allIndices(
          into: slice,
          in: slice.startIndex
            ..< slice.index(
                  slice.endIndex, offsetBy: -test.numberToRemove)
        )
        slice.removeLast(test.numberToRemove)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "removeLast() shouldn't mutate the head of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "removeLast() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection.map { $0.value },
          c.map { extractValue($0).value },
          "removeLast() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).removeLast(n: Int)/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeLast(1) // Should trap.
    }

    self.test(
      "\(testNamePrefix).removeLast(n: Int)/slice/removeNegative/semantics"
    ) {
      let c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeLast(-1) // Should trap.
    }

    self.test(
      "\(testNamePrefix).removeLast(n: Int)/slice/removeTooMany/semantics"
    ) {
      let c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      var slice = c[c.startIndex..<c.startIndex]
      expectCrashLater()
      slice.removeLast(3) // Should trap.
    }

    //===------------------------------------------------------------------===//
    // popLast()/slice
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).popLast()/slice/semantics") {
      // This can just reuse the test data for removeLast()
      for test in removeLastTests.filter({ $0.numberToRemove == 1 }) {
        let c = makeWrappedCollection(test.collection)
        var slice = c[...]
        let survivingIndices = _allIndices(
          into: slice,
          in: slice.startIndex
              ..< slice.index(
                    slice.endIndex, offsetBy: -test.numberToRemove)
        )
        let removedElement = slice.popLast()!
        expectEqual(
          test.collection.last!.value,
          extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          slice.map { extractValue($0).value },
          "popLast() shouldn't mutate the head of the slice",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(slice[$0]).value },
          "popLast() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.collection.map { $0.value },
          c.map { extractValue($0).value },
          "popLast() shouldn't mutate the collection that was sliced",
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    self.test("\(testNamePrefix).popLast()/slice/empty/semantics") {
      let c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      var slice = c[c.startIndex..<c.startIndex]
      expectNil(slice.popLast())
    }


    //===------------------------------------------------------------------===//
    // Index
    //===------------------------------------------------------------------===//

    if resiliencyChecks.creatingOutOfBoundsIndicesBehavior != .none {
      self.test("\(testNamePrefix).Index/OutOfBounds/Left/NonEmpty") {
        let c = makeWrappedCollection(
          [ 1010, 2020, 3030 ].map(OpaqueValue.init))
        let index = c.startIndex
        expectCrashLater()
        _blackHole(
          c.index(index, offsetBy: -outOfBoundsIndexOffset))
      }

      self.test("\(testNamePrefix).Index/OutOfBounds/Left/Empty") {
        let c = makeWrappedCollection([])
        let index = c.startIndex
        expectCrashLater()
        _blackHole(
          c.index(index, offsetBy: -outOfBoundsIndexOffset))
      }
    }

    //===------------------------------------------------------------------===//
    // subscript(_: Index)
    //===------------------------------------------------------------------===//

    if resiliencyChecks.subscriptOnOutOfBoundsIndicesBehavior != .none {
      self.test(
        "\(testNamePrefix).subscript(_: Index)/OutOfBounds/Left/NonEmpty/Get"
      ) {
        let c = makeWrappedCollection(
          [ 1010, 2020, 3030 ].map(OpaqueValue.init))
        var index = c.startIndex
        expectCrashLater()
        index = c.index(
          index, offsetBy: -outOfBoundsSubscriptOffset)
        _blackHole(c[index])
      }

      self.test(
        "\(testNamePrefix).subscript(_: Index)/OutOfBounds/Left/Empty/Get"
      ) {
        let c = makeWrappedCollection([])
        var index = c.startIndex
        expectCrashLater()
        index = c.index(
          index, offsetBy: -outOfBoundsSubscriptOffset)
        _blackHole(c[index])
      }
    }

    //===------------------------------------------------------------------===//
    // subscript(_: Range)
    //===------------------------------------------------------------------===//

    if resiliencyChecks.subscriptRangeOnOutOfBoundsRangesBehavior != .none {
      self.test(
        "\(testNamePrefix).subscript(_: Range)/OutOfBounds/Left/NonEmpty/Get"
      ) {
        let c = makeWrappedCollection(
          [ 1010, 2020, 3030 ].map(OpaqueValue.init))
        var index = c.startIndex
        expectCrashLater()
        index = c.index(
          index, offsetBy: -outOfBoundsSubscriptOffset)
        _blackHole(c[index..<index])
      }

      self.test(
        "\(testNamePrefix).subscript(_: Range)/OutOfBounds/Left/Empty/Get"
      ) {
        let c = makeWrappedCollection([])
        var index = c.startIndex
        expectCrashLater()
        index = c.index(
          index, offsetBy: -outOfBoundsSubscriptOffset)
        _blackHole(c[index..<index])
      }
    }

    //===------------------------------------------------------------------===//
    // dropLast()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).dropLast/semantics") {
      for test in dropLastTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.dropLast(test.dropElements)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // suffix()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).suffix/semantics") {
      for test in suffixTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.suffix(test.maxLength)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//

    self.addCommonTests(
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
    collectionIsBidirectional: collectionIsBidirectional)
  } // addBidirectionalCollectionTests

  public func addRandomAccessCollectionTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool
 = true
  ) where

  C : RandomAccessCollection,
  CollectionWithEquatableElement : RandomAccessCollection,
  CollectionWithEquatableElement.Element : Equatable
 {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

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
    outOfBoundsSubscriptOffset: outOfBoundsSubscriptOffset,
    collectionIsBidirectional: collectionIsBidirectional)

    testNamePrefix += String(describing: C.self)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    //===------------------------------------------------------------------===//
    // prefix()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).prefix/semantics") {
      for test in prefixTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.prefix(test.maxLength)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    // suffix()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).suffix/semantics") {
      for test in suffixTests {
        let s = makeWrappedCollection(test.sequence.map(OpaqueValue.init))
        let result = s.suffix(test.maxLength)
        expectEqualSequence(
          test.expected, result.map(extractValue).map { $0.value },
          stackTrace: SourceLocStack().with(test.loc))
      }
    }

    //===------------------------------------------------------------------===//
    self.addCommonTests(
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
    collectionIsBidirectional: collectionIsBidirectional)
  } // addRandomAccessCollectionTests

  func addCommonTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool

  ) where

  C : Collection,
  CollectionWithEquatableElement : Collection,
  CollectionWithEquatableElement.Element : Equatable
 {

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    func toCollection(_ r: Range<Int>) -> C {
      return makeCollection(r.map { wrapValue(OpaqueValue($0)) })
    }

    self.test("\(testNamePrefix).index(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          currentIndex = c.index(after: currentIndex)
          counter += 1
        } while counter < test.end
      }
    }
    self.test("\(testNamePrefix).formIndex(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          c.formIndex(after: &currentIndex)
          counter += 1
        } while counter < test.end
      }
    }

    self.test("\(testNamePrefix)/distance(from:to:)/semantics")
      .forEach(in: distanceFromToTests) {
      test in
      let c = toCollection(0..<20)
      let backwards = (test.startOffset > test.endOffset)
      if backwards && !collectionIsBidirectional {
        expectCrashLater()
      }

      let d = c.distance(
        from: c.nthIndex(test.startOffset), to: c.nthIndex(test.endOffset))
      expectEqual(
        test.expectedDistance,
        d, stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      let new = c.index(
        c.nthIndex(test.startOffset),
        offsetBy: test.distance)

      // Since the `nthIndex(offset:)` method performs the same operation
      // (i.e. advances `c.startIndex` by `test.distance`, it would be
      // silly to compare index values. Luckily the underlying collection
      // contains exactly index offsets.
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let start = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      let new = c.index(start, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }
    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        var new = c.nthIndex(test.startOffset)
        let exact = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          // Clamped to the limit
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      }
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      if collectionIsBidirectional {
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      } else {
        expectCrashLater()
        _ = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      var new = c.nthIndex(test.startOffset)
      if collectionIsBidirectional {
        let exact = c.formIndex(
          &new,
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      } else {
        expectCrashLater()
        _ = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
      }
    }

  }
  func addCommonTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool

  ) where

  C : BidirectionalCollection,
  CollectionWithEquatableElement : BidirectionalCollection,
  CollectionWithEquatableElement.Element : Equatable
 {

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    func toCollection(_ r: Range<Int>) -> C {
      return makeCollection(r.map { wrapValue(OpaqueValue($0)) })
    }

    self.test("\(testNamePrefix).index(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          currentIndex = c.index(after: currentIndex)
          counter += 1
        } while counter < test.end
      }
    }
    self.test("\(testNamePrefix).formIndex(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          c.formIndex(after: &currentIndex)
          counter += 1
        } while counter < test.end
      }
    }

    self.test("\(testNamePrefix)/distance(from:to:)/semantics")
      .forEach(in: distanceFromToTests) {
      test in
      let c = toCollection(0..<20)
      let backwards = (test.startOffset > test.endOffset)
      if backwards && !collectionIsBidirectional {
        expectCrashLater()
      }

      let d = c.distance(
        from: c.nthIndex(test.startOffset), to: c.nthIndex(test.endOffset))
      expectEqual(
        test.expectedDistance,
        d, stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      let new = c.index(
        c.nthIndex(test.startOffset),
        offsetBy: test.distance)

      // Since the `nthIndex(offset:)` method performs the same operation
      // (i.e. advances `c.startIndex` by `test.distance`, it would be
      // silly to compare index values. Luckily the underlying collection
      // contains exactly index offsets.
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let start = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      let new = c.index(start, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }
    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        var new = c.nthIndex(test.startOffset)
        let exact = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          // Clamped to the limit
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      }
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      if collectionIsBidirectional {
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      } else {
        expectCrashLater()
        _ = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      var new = c.nthIndex(test.startOffset)
      if collectionIsBidirectional {
        let exact = c.formIndex(
          &new,
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      } else {
        expectCrashLater()
        _ = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
      }
    }

  }
  func addCommonTests<
    C, CollectionWithEquatableElement
  >(

  _ testNamePrefix: String = "",
  makeCollection: @escaping ([C.Element]) -> C,
  wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
  extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

  makeCollectionOfEquatable: @escaping (
    [CollectionWithEquatableElement.Element]
  ) -> CollectionWithEquatableElement,

  wrapValueIntoEquatable: @escaping (
    MinimalEquatableValue) -> CollectionWithEquatableElement.Element,

  extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  outOfBoundsIndexOffset: Int = 1,
  outOfBoundsSubscriptOffset: Int = 1,
  collectionIsBidirectional: Bool

  ) where

  C : RandomAccessCollection,
  CollectionWithEquatableElement : RandomAccessCollection,
  CollectionWithEquatableElement.Element : Equatable
 {

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    func toCollection(_ r: Range<Int>) -> C {
      return makeCollection(r.map { wrapValue(OpaqueValue($0)) })
    }

    self.test("\(testNamePrefix).index(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          currentIndex = c.index(after: currentIndex)
          counter += 1
        } while counter < test.end
      }
    }
    self.test("\(testNamePrefix).formIndex(after:)/semantics") {
      for test in indexAfterTests {
        let c = toCollection(test.start..<test.end)
        var currentIndex = c.startIndex
        var counter = test.start
        repeat {
          expectEqual(counter, extractValue(c[currentIndex]).value,
            stackTrace: SourceLocStack().with(test.loc))
          c.formIndex(after: &currentIndex)
          counter += 1
        } while counter < test.end
      }
    }

    self.test("\(testNamePrefix)/distance(from:to:)/semantics")
      .forEach(in: distanceFromToTests) {
      test in
      let c = toCollection(0..<20)
      let backwards = (test.startOffset > test.endOffset)
      if backwards && !collectionIsBidirectional {
        expectCrashLater()
      }

      let d = c.distance(
        from: c.nthIndex(test.startOffset), to: c.nthIndex(test.endOffset))
      expectEqual(
        test.expectedDistance,
        d, stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      let new = c.index(
        c.nthIndex(test.startOffset),
        offsetBy: test.distance)

      // Since the `nthIndex(offset:)` method performs the same operation
      // (i.e. advances `c.startIndex` by `test.distance`, it would be
      // silly to compare index values. Luckily the underlying collection
      // contains exactly index offsets.
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance >= 0 }
      ) {
      test in
      let max = 10
      let c = toCollection(0..<max)

      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! >= max {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let start = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      let new = c.index(start, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }
    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit == nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      var new = c.nthIndex(test.startOffset)

      if test.expectedOffset! < 0 || !collectionIsBidirectional {
        expectCrashLater()
      }
      c.formIndex(&new, offsetBy: test.distance)
      expectEqual(test.expectedOffset!, extractValue(c[new]).value,
        stackTrace: SourceLocStack().with(test.loc))
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: n, limitedBy:)/semantics") {
      for test in indexOffsetByTests.filter(
        {$0.limit != nil && $0.distance >= 0}
      ) {
        let c = toCollection(0..<20)
        let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
        var new = c.nthIndex(test.startOffset)
        let exact = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          // Clamped to the limit
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      }
    }

    self.test("\(testNamePrefix)/index(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      if collectionIsBidirectional {
        let new = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new!,
            stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectNil(new)
        }
      } else {
        expectCrashLater()
        _ = c.index(
          c.nthIndex(test.startOffset),
          offsetBy: test.distance,
          limitedBy: limit)
      }
    }

    self.test("\(testNamePrefix)/formIndex(_:offsetBy: -n, limitedBy:)/semantics")
      .forEach(
        in: indexOffsetByTests.filter { $0.limit != nil && $0.distance < 0 }
      ) {
      test in
      let c = toCollection(0..<20)
      let limit = c.nthIndex(test.limit.unsafelyUnwrapped)
      var new = c.nthIndex(test.startOffset)
      if collectionIsBidirectional {
        let exact = c.formIndex(
          &new,
          offsetBy: test.distance,
          limitedBy: limit)
        if let expectedOffset = test.expectedOffset {
          expectEqual(c.nthIndex(expectedOffset), new,
            stackTrace: SourceLocStack().with(test.loc))
          expectTrue(exact, stackTrace: SourceLocStack().with(test.loc))
        } else {
          expectEqual(limit, new, stackTrace: SourceLocStack().with(test.loc))
          expectFalse(exact, stackTrace: SourceLocStack().with(test.loc))
        }
      } else {
        expectCrashLater()
        _ = c.formIndex(&new, offsetBy: test.distance, limitedBy: limit)
      }
    }

  }
}

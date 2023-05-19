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

// A minimal RRC conformance, to test default implementations with
public struct NaiveRRC : RangeReplaceableCollection, ExpressibleByArrayLiteral {
  
  // we're trying to move away from calling reserveCapacity inside most mutating
  // methods, this will let us verify that we don't
  internal var allowReserveCapacity = true
  var storage:[Int] = []
  
  public init() {}
  
  public init(arrayLiteral elements: Element...) {
    storage.append(contentsOf: elements)
  }
  
  public func index(after i: Int) -> Int {
    i + 1
  }
  
  public func index(before i: Int) -> Int {
    i - 1
  }
  
  public var startIndex: Int {
    0
  }
  
  public var endIndex: Int {
    count
  }
  
  public var count: Int {
    storage.count
  }
  
  public subscript(position: Int) -> Int {
    get {
      storage[position]
    }
    set(newValue) {
      storage[position] = newValue
    }
  }
  
  public mutating func replaceSubrange(_ subrange: Range<Int>, with newElements: some Collection<Int>) {
    storage.replaceSubrange(subrange, with: newElements)
  }
  
  public mutating func reserveCapacity(_ n: Int) {
    precondition(allowReserveCapacity)
    storage.reserveCapacity(n)
  }
}

internal enum IndexSelection {
  case start
  case middle
  case end
  case last

  internal func index<C : Collection>(in c: C) -> C.Index {
    switch self {
      case .start: return c.startIndex
      case .middle: return c.index(c.startIndex, offsetBy: c.count / 2)
      case .end: return c.endIndex
      case .last: return c.index(c.startIndex, offsetBy: c.count - 1)
    }
  }
}

public struct ReplaceSubrangeTest<C: RangeReplaceableCollection> where C.Element == Int {
  public let collection: [OpaqueValue<Int>]
  public let newElements: [OpaqueValue<Int>]
  public let rangeSelection: RangeSelection
  public let expected: C
  public let closedExpected: C?  // Expected array for closed ranges
  public let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    newElements: some Collection<Int>,
    rangeSelection: RangeSelection, expected: C, closedExpected: C? = nil,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.newElements = newElements.map(OpaqueValue.init)
    self.rangeSelection = rangeSelection
    self.expected = expected
    self.closedExpected = closedExpected
    self.loc = SourceLoc(file, line, comment: "replaceSubrange() test data")
  }
}

internal struct AppendTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let newElement: OpaqueValue<Int>
  let expected: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    newElement: Int, expected: C,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.newElement = OpaqueValue(newElement)
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "append() test data")
  }
}

internal struct AppendContentsOfTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let newElements: [OpaqueValue<Int>]
  let expected: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    newElements: some RangeReplaceableCollection<Int>, expected: C,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.newElements = newElements.map(OpaqueValue.init)
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "append() test data")
  }
}

internal struct InsertTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let newElement: OpaqueValue<Int>
  let indexSelection: IndexSelection
  let expected: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    newElement: Int, indexSelection: IndexSelection,
    expected: C, file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.newElement = OpaqueValue(newElement)
    self.indexSelection = indexSelection
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "insert() test data")
  }
}

internal struct InsertContentsOfTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let newElements: [OpaqueValue<Int>]
  let indexSelection: IndexSelection
  let expected: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    newElements: some RangeReplaceableCollection<Int>,
    indexSelection: IndexSelection,
    expected: C, file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.newElements = newElements.map(OpaqueValue.init)
    self.indexSelection = indexSelection
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "insert(contentsOf:at:) test data")
  }
}

internal struct RemoveAtIndexTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let indexSelection: IndexSelection
  let expectedRemovedElement: Int
  let expectedCollection: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    indexSelection: IndexSelection, expectedRemovedElement: Int,
    expectedCollection: C, file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.indexSelection = indexSelection
    self.expectedRemovedElement = expectedRemovedElement
    self.expectedCollection = expectedCollection
    self.loc = SourceLoc(file, line, comment: "remove(at:) test data")
  }
}

internal struct RemoveLastNTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let numberToRemove: Int
  let expectedCollection: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    numberToRemove: Int, expectedCollection: C,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.numberToRemove = numberToRemove
    self.expectedCollection = expectedCollection
    self.loc = SourceLoc(file, line, comment: "removeLast(n: Int) test data")
  }
}

public struct RemoveSubrangeTest<C: RangeReplaceableCollection> where C.Element == Int {
  public let collection: [OpaqueValue<Int>]
  public let rangeSelection: RangeSelection
  public let expected: C
  public let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>,
    rangeSelection: RangeSelection, expected: C,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.rangeSelection = rangeSelection
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "removeSubrange() test data")
  }
}

internal struct RemoveAllTest<C: RangeReplaceableCollection> where C.Element == Int {
  let collection: [OpaqueValue<Int>]
  let expected: C
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>, expected: C,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "removeAll() test data")
  }
}

internal struct ReserveCapacityTest {
  let collection: [OpaqueValue<Int>]
  let requestedCapacity: Int
  let loc: SourceLoc

  internal init(
    collection: some RangeReplaceableCollection<Int>, requestedCapacity: Int,
    file: String = #file, line: UInt = #line
  ) {
    self.collection = collection.map(OpaqueValue.init)
    self.requestedCapacity = requestedCapacity
    self.loc = SourceLoc(file, line, comment: "removeAll() test data")
  }
}

internal struct OperatorPlusTest<C: RangeReplaceableCollection> where C.Element == Int {
  let lhs: [OpaqueValue<Int>]
  let rhs: [OpaqueValue<Int>]
  let expected: C
  let loc: SourceLoc

  internal init(
    lhs: some RangeReplaceableCollection<Int>,
    rhs: some RangeReplaceableCollection<Int>,
    expected: C,
    file: String = #file, line: UInt = #line
  ) {
    self.lhs = lhs.map(OpaqueValue.init)
    self.rhs = rhs.map(OpaqueValue.init)
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "`func +` test data")
  }
}

let removeLastTests: [RemoveLastNTest] = [
  RemoveLastNTest(
    collection: [1010],
    numberToRemove: 0,
    expectedCollection: [1010]
  ),
  RemoveLastNTest(
    collection: [1010],
    numberToRemove: 1,
    expectedCollection: []
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 0,
    expectedCollection: [1010, 2020, 3030, 4040, 5050]
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 1,
    expectedCollection: [1010, 2020, 3030, 4040]
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 2,
    expectedCollection: [1010, 2020, 3030]
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 3,
    expectedCollection: [1010, 2020]
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 4,
    expectedCollection: [1010]
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    numberToRemove: 5,
    expectedCollection: []
  ),
  RemoveLastNTest(
    collection: [1010] as NaiveRRC,
    numberToRemove: 0,
    expectedCollection: [1010] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010] as NaiveRRC,
    numberToRemove: 1,
    expectedCollection: [] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 0,
    expectedCollection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 1,
    expectedCollection: [1010, 2020, 3030, 4040] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 2,
    expectedCollection: [1010, 2020, 3030] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 3,
    expectedCollection: [1010, 2020] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 4,
    expectedCollection: [1010] as NaiveRRC
  ),
  RemoveLastNTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    numberToRemove: 5,
    expectedCollection: [] as NaiveRRC
  ),
]

let appendContentsOfTests: [AppendContentsOfTest] = [
  AppendContentsOfTest(
    collection: [],
    newElements: [],
    expected: []),

  AppendContentsOfTest(
    collection: [1010],
    newElements: [],
    expected: [1010]),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040],
    newElements: [],
    expected: [1010, 2020, 3030, 4040]),

  AppendContentsOfTest(
    collection: [],
    newElements: [1010],
    expected: [1010]),

  AppendContentsOfTest(
    collection: [1010],
    newElements: [2020],
    expected: [1010, 2020]),

  AppendContentsOfTest(
    collection: [1010],
    newElements: [2020, 3030, 4040],
    expected: [1010, 2020, 3030, 4040]),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040],
    newElements: [5050, 6060, 7070, 8080],
    expected: [1010, 2020, 3030, 4040, 5050, 6060, 7070, 8080]),
  
  // NaiveRRC doesn't implement withContiguousStorageIfAvailable, so this tests discontiguous appendees
  AppendContentsOfTest(
    collection: [] as NaiveRRC,
    newElements: [] as NaiveRRC,
    expected: [] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [] as NaiveRRC,
    expected: [1010] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    newElements: [] as NaiveRRC,
    expected: [1010, 2020, 3030, 4040] as NaiveRRC),

  AppendContentsOfTest(
    collection: [] as NaiveRRC,
    newElements: [1010] as NaiveRRC,
    expected: [1010] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [2020] as NaiveRRC,
    expected: [1010, 2020] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [2020, 3030, 4040] as NaiveRRC,
    expected: [1010, 2020, 3030, 4040] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    newElements: [5050, 6060, 7070, 8080] as NaiveRRC,
    expected: [1010, 2020, 3030, 4040, 5050, 6060, 7070, 8080] as NaiveRRC),
  
  // Here we use Array to catch the contiguous cases
  AppendContentsOfTest(
    collection: [] as NaiveRRC,
    newElements: [],
    expected: [] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [],
    expected: [1010] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    newElements: [],
    expected: [1010, 2020, 3030, 4040] as NaiveRRC),

  AppendContentsOfTest(
    collection: [] as NaiveRRC,
    newElements: [1010],
    expected: [1010] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [2020],
    expected: [1010, 2020] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010] as NaiveRRC,
    newElements: [2020, 3030, 4040],
    expected: [1010, 2020, 3030, 4040] as NaiveRRC),

  AppendContentsOfTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    newElements: [5050, 6060, 7070, 8080],
    expected: [1010, 2020, 3030, 4040, 5050, 6060, 7070, 8080] as NaiveRRC),
]

// Also used in RangeReplaceable.swift.gyb to test `replaceSubrange()`
// overloads with the countable range types.
public let replaceRangeTests: [ReplaceSubrangeTest] = [
  ReplaceSubrangeTest(
    collection: [],
    newElements: [],
    rangeSelection: .emptyRange,
    expected: []),

  ReplaceSubrangeTest(
    collection: [],
    newElements: [1010],
    rangeSelection: .emptyRange,
    expected: [1010]),

  ReplaceSubrangeTest(
    collection: [],
    newElements: [1010, 2020, 3030],
    rangeSelection: .emptyRange,
    expected: [1010, 2020, 3030]),

  ReplaceSubrangeTest(
    collection: [4040],
    newElements: [1010, 2020, 3030],
    rangeSelection: .leftEdge,
    expected: [1010, 2020, 3030, 4040],
    closedExpected: [1010, 2020, 3030]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030],
    newElements: [4040],
    rangeSelection: .leftEdge,
    expected: [4040, 1010, 2020, 3030],
    closedExpected: [4040, 2020, 3030]),

  ReplaceSubrangeTest(
    collection: [1010],
    newElements: [2020, 3030, 4040],
    rangeSelection: .rightEdge,
    expected: [1010, 2020, 3030, 4040],
    closedExpected: [2020, 3030, 4040]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030],
    newElements: [4040],
    rangeSelection: .rightEdge,
    expected: [1010, 2020, 3030, 4040],
    closedExpected: [1010, 2020, 4040]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    newElements: [9090],
    rangeSelection: .offsets(1, 1),
    expected: [1010, 9090, 2020, 3030, 4040, 5050],
    closedExpected: [1010, 9090, 3030, 4040, 5050]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    newElements: [9090],
    rangeSelection: .offsets(1, 2),
    expected: [1010, 9090, 3030, 4040, 5050],
    closedExpected: [1010, 9090, 4040, 5050]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    newElements: [9090],
    rangeSelection: .offsets(1, 3),
    expected: [1010, 9090, 4040, 5050],
    closedExpected: [1010, 9090, 5050]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    newElements: [9090],
    rangeSelection: .offsets(1, 4),
    expected: [1010, 9090, 5050],
    closedExpected: [1010, 9090]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    newElements: [9090],
    rangeSelection: .offsets(1, 5),
    expected: [1010, 9090]),

  ReplaceSubrangeTest(
    collection: [1010, 2020, 3030],
    newElements: [8080, 9090],
    rangeSelection: .offsets(1, 2),
    expected: [1010, 8080, 9090, 3030],
    closedExpected: [1010, 8080, 9090]),
  
  //replaceSubrange is the protocol requirement, so we don't test NaiveRRC here
]

public let removeRangeTests: [RemoveSubrangeTest] = [
  RemoveSubrangeTest(
    collection: [],
    rangeSelection: .emptyRange,
    expected: []),

  RemoveSubrangeTest(
    collection: [1010],
    rangeSelection: .middle,
    expected: []),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040],
    rangeSelection: .leftHalf,
    expected: [3030, 4040]),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040],
    rangeSelection: .rightHalf,
    expected: [1010, 2020]),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050],
    rangeSelection: .middle,
    expected: [1010, 5050]),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060],
    rangeSelection: .leftHalf,
    expected: [4040, 5050, 6060]),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060],
    rangeSelection: .rightHalf,
    expected: [1010, 2020, 3030]),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060],
    rangeSelection: .middle,
    expected: [1010, 6060]),
  
  RemoveSubrangeTest(
    collection: [] as NaiveRRC,
    rangeSelection: .emptyRange,
    expected: [] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010] as NaiveRRC,
    rangeSelection: .middle,
    expected: [] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    rangeSelection: .leftHalf,
    expected: [3030, 4040] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040] as NaiveRRC,
    rangeSelection: .rightHalf,
    expected: [1010, 2020] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050] as NaiveRRC,
    rangeSelection: .middle,
    expected: [1010, 5050] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060] as NaiveRRC,
    rangeSelection: .leftHalf,
    expected: [4040, 5050, 6060] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060] as NaiveRRC,
    rangeSelection: .rightHalf,
    expected: [1010, 2020, 3030] as NaiveRRC),

  RemoveSubrangeTest(
    collection: [1010, 2020, 3030, 4040, 5050, 6060] as NaiveRRC,
    rangeSelection: .middle,
    expected: [1010, 6060] as NaiveRRC),
]

extension TestSuite {
  /// Adds a set of tests for `RangeReplaceableCollection`.
  ///
  /// - parameter makeCollection: a factory function that creates a collection
  ///   instance with provided elements.
  ///
  ///   This facility can be used to test collection instances that can't be
  ///   constructed using APIs in the protocol (for example, `Array`s that wrap
  ///   `NSArray`s).
  public func addRangeReplaceableCollectionTests<
    C : RangeReplaceableCollection,
    CollectionWithEquatableElement : RangeReplaceableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1,
    collectionIsBidirectional: Bool = false
  ) where
    CollectionWithEquatableElement.Element : Equatable {

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
      collectionIsBidirectional: collectionIsBidirectional
    )

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    testNamePrefix += String(describing: C.Type.self)

//===----------------------------------------------------------------------===//
// init()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).init()/semantics") {
  let c = C()
  expectEqualSequence([], c.map { extractValue($0).value })
}

//===----------------------------------------------------------------------===//
// init(Sequence)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).init(Sequence)/semantics") {
  for test in appendContentsOfTests {
    let c = C(test.newElements.map(wrapValue))
    expectEqualSequence(
      test.newElements.map { $0.value },
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// replaceSubrange()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).replaceSubrange()/range/semantics") {
  for test in replaceRangeTests {
    var c = makeWrappedCollection(test.collection)
    let rangeToReplace = test.rangeSelection.range(in: c)
    let newElements =
      MinimalCollection(elements: test.newElements.map(wrapValue))
    c.replaceSubrange(rangeToReplace, with: newElements)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).replaceSubrange()/closedRange/semantics") {
  for test in replaceRangeTests {
    guard let closedExpected = test.closedExpected else { continue }
    var c = makeWrappedCollection(test.collection)
    let rangeToReplace = test.rangeSelection.closedRange(in: c)
    let newElements = makeWrappedCollection(test.newElements)
    c.replaceSubrange(rangeToReplace, with: newElements)
    expectEqualSequence(
      closedExpected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// append()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).append()/semantics") {
  let tests: [AppendTest] = [
    AppendTest(
      collection: [],
      newElement: 1010,
      expected: [1010]),

    AppendTest(
      collection: [1010],
      newElement: 2020,
      expected: [1010, 2020]),

    AppendTest(
      collection: [1010, 2020, 3030, 4040, 5050, 6060, 7070],
      newElement: 8080,
      expected: [1010, 2020, 3030, 4040, 5050, 6060, 7070, 8080]),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    let newElement = wrapValue(test.newElement)
    c.append(newElement)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// append(contentsOf:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).append(contentsOf:)/semantics") {
  for test in appendContentsOfTests {
    var c = makeWrappedCollection(test.collection)
    let newElements =
      MinimalCollection(elements: test.newElements.map(wrapValue))
    c.append(contentsOf: newElements)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).OperatorPlusEquals") {
  for test in appendContentsOfTests {
    var c = makeWrappedCollection(test.collection)
    let newElements =
      MinimalCollection(elements: test.newElements.map(wrapValue))
    c += newElements
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// insert()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).insert()/semantics") {
  let tests: [InsertTest] = [
    InsertTest(
      collection: [],
      newElement: 1010,
      indexSelection: IndexSelection.start,
      expected: [1010]),

    InsertTest(
      collection: [2020],
      newElement: 1010,
      indexSelection: .start,
      expected: [1010, 2020]),

    InsertTest(
      collection: [1010],
      newElement: 2020,
      indexSelection: .end,
      expected: [1010, 2020]),

    InsertTest(
      collection: [2020, 3030, 4040, 5050],
      newElement: 1010,
      indexSelection: .start,
      expected: [1010, 2020, 3030, 4040, 5050]),

    InsertTest(
      collection: [1010, 2020, 3030, 4040],
      newElement: 5050,
      indexSelection: .end,
      expected: [1010, 2020, 3030, 4040, 5050]),

    InsertTest(
      collection: [1010, 2020, 4040, 5050],
      newElement: 3030,
      indexSelection: .middle,
      expected: [1010, 2020, 3030, 4040, 5050]),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    let newElement = wrapValue(test.newElement)
    c.insert(newElement, at: test.indexSelection.index(in: c))
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// insert(contentsOf:at:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).insert(contentsOf:at:)/semantics") {
  let tests: [InsertContentsOfTest] = [
    InsertContentsOfTest(
      collection: [],
      newElements: [],
      indexSelection: IndexSelection.start,
      expected: []),

    InsertContentsOfTest(
      collection: [],
      newElements: [1010],
      indexSelection: .start,
      expected: [1010]),

    InsertContentsOfTest(
      collection: [],
      newElements: [1010, 2020, 3030, 4040],
      indexSelection: .start,
      expected: [1010, 2020, 3030, 4040]),

    InsertContentsOfTest(
      collection: [2020],
      newElements: [1010],
      indexSelection: .start,
      expected: [1010, 2020]),

    InsertContentsOfTest(
      collection: [1010],
      newElements: [2020],
      indexSelection: .end,
      expected: [1010, 2020]),

    InsertContentsOfTest(
      collection: [4040],
      newElements: [1010, 2020, 3030],
      indexSelection: .start,
      expected: [1010, 2020, 3030, 4040]),

    InsertContentsOfTest(
      collection: [1010],
      newElements: [2020, 3030, 4040],
      indexSelection: .end,
      expected: [1010, 2020, 3030, 4040]),

    InsertContentsOfTest(
      collection: [1010, 2020, 4040, 5050],
      newElements: [3030],
      indexSelection: .middle,
      expected: [1010, 2020, 3030, 4040, 5050]),

    InsertContentsOfTest(
      collection: [4040, 5050, 6060],
      newElements: [1010, 2020, 3030],
      indexSelection: .start,
      expected: [1010, 2020, 3030, 4040, 5050, 6060]),

    InsertContentsOfTest(
      collection: [1010, 2020, 3030],
      newElements: [4040, 5050, 6060],
      indexSelection: .end,
      expected: [1010, 2020, 3030, 4040, 5050, 6060]),

    InsertContentsOfTest(
      collection: [1010, 2020, 3030, 7070, 8080, 9090],
      newElements: [4040, 5050, 6060],
      indexSelection: .middle,
      expected: [1010, 2020, 3030, 4040, 5050, 6060, 7070, 8080, 9090]),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    let newElements =
      MinimalCollection(elements: test.newElements.map(wrapValue))
    c.insert(contentsOf: newElements, at: test.indexSelection.index(in: c))
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// remove(at:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).remove(at:)/semantics") {
  let tests: [RemoveAtIndexTest] = [
    RemoveAtIndexTest(
      collection: [1010],
      indexSelection: .start,
      expectedRemovedElement: 1010,
      expectedCollection: []),

    RemoveAtIndexTest(
      collection: [1010, 2020, 3030],
      indexSelection: .start,
      expectedRemovedElement: 1010,
      expectedCollection: [2020, 3030]),

    RemoveAtIndexTest(
      collection: [1010, 2020, 3030],
      indexSelection: .middle,
      expectedRemovedElement: 2020,
      expectedCollection: [1010, 3030]),

    RemoveAtIndexTest(
      collection: [1010, 2020, 3030],
      indexSelection: .last,
      expectedRemovedElement: 3030,
      expectedCollection: [1010, 2020]),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    let removedElement = c.remove(at: test.indexSelection.index(in: c))
    expectEqualSequence(
      test.expectedCollection,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
    expectEqual(
      test.expectedRemovedElement,
      extractValue(removedElement).value,
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// removeFirst()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeFirst()/semantics") {
  for test in removeFirstTests.filter({ $0.numberToRemove == 1 }) {
    var c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
    let removedElement = c.removeFirst()
    expectEqual(test.collection.first, extractValue(removedElement).value)
    expectEqualSequence(
      test.expectedCollection,
      c.map { extractValue($0).value },
      "removeFirst() shouldn't mutate the tail of the collection",
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).removeFirst()/empty/semantics") {
  var c = makeWrappedCollection(Array<OpaqueValue<Int>>())
  expectCrashLater()
  _ = c.removeFirst() // Should trap.
}

//===----------------------------------------------------------------------===//
// removeFirst(n: Int)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeFirst(n: Int)/semantics") {
  for test in removeFirstTests {
    var c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
    c.removeFirst(test.numberToRemove)
    expectEqualSequence(
      test.expectedCollection,
      c.map { extractValue($0).value },
      "removeFirst() shouldn't mutate the tail of the collection",
      stackTrace: SourceLocStack().with(test.loc)
    )
  }
}

self.test("\(testNamePrefix).removeFirst(n: Int)/empty/semantics") {
  var c = makeWrappedCollection(Array<OpaqueValue<Int>>())
  expectCrashLater()
  c.removeFirst(1) // Should trap.
}

self.test("\(testNamePrefix).removeFirst(n: Int)/removeNegative/semantics") {
  var c = makeWrappedCollection([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  c.removeFirst(-1) // Should trap.
}

self.test("\(testNamePrefix).removeFirst(n: Int)/removeTooMany/semantics") {
  var c = makeWrappedCollection([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  c.removeFirst(5) // Should trap.
}

//===----------------------------------------------------------------------===//
// removeSubrange()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeSubrange()/range/semantics") {
  for test in removeRangeTests {
    var c = makeWrappedCollection(test.collection)
    let rangeToRemove = test.rangeSelection.range(in: c)
    c.removeSubrange(rangeToRemove)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).replaceSubrange()/closedRange/semantics") {
  for test in removeRangeTests.filter({ !$0.rangeSelection.isEmpty }) {
    var c = makeWrappedCollection(test.collection)
    let rangeToRemove = test.rangeSelection.closedRange(in: c)
    c.removeSubrange(rangeToRemove)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// removeAll()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeAll()/semantics") {
  let tests: [RemoveAllTest] = [
    RemoveAllTest(
      collection: [],
      expected: []),

    RemoveAllTest(
      collection: [1010],
      expected: []),

    RemoveAllTest(
      collection: [1010, 2020, 3030, 4040, 5050],
      expected: []),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    c.removeAll()
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    c.removeAll(keepingCapacity: false)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    c.removeAll(keepingCapacity: true)
    expectEqualSequence(
      test.expected,
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// reserveCapacity()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).reserveCapacity()/semantics") {
  let tests: [ReserveCapacityTest] = [
    ReserveCapacityTest(
      collection: [],
      requestedCapacity: 0),

    ReserveCapacityTest(
      collection: [],
      requestedCapacity: 3),

    ReserveCapacityTest(
      collection: [],
      requestedCapacity: 100),

    ReserveCapacityTest(
      collection: [ 1010 ],
      requestedCapacity: 0),

    ReserveCapacityTest(
      collection: [ 1010 ],
      requestedCapacity: 3),

    ReserveCapacityTest(
      collection: [ 1010 ],
      requestedCapacity: 100),

    ReserveCapacityTest(
      collection: [ 1010, 2020, 3030 ],
      requestedCapacity: 0),

    ReserveCapacityTest(
      collection: [ 1010, 2020, 3030 ],
      requestedCapacity: 3),

    ReserveCapacityTest(
      collection: [ 1010, 2020, 3030 ],
      requestedCapacity: 100),
  ]

  for test in tests {
    var c = makeWrappedCollection(test.collection)
    c.reserveCapacity(test.requestedCapacity)
    expectEqualSequence(
      test.collection.map { $0.value },
      c.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// + operator
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).OperatorPlus") {
  let tests: [OperatorPlusTest] = [
    OperatorPlusTest(
      lhs: [],
      rhs: [],
      expected: []),

    OperatorPlusTest(
      lhs: [],
      rhs: [ 1010 ],
      expected: [ 1010 ]),

    OperatorPlusTest(
      lhs: [ 1010 ],
      rhs: [],
      expected: [ 1010 ]),

    OperatorPlusTest(
      lhs: [ 1010 ],
      rhs: [ 2020 ],
      expected: [ 1010, 2020 ]),

    OperatorPlusTest(
      lhs: [ 1010, 2020, 3030 ],
      rhs: [],
      expected: [ 1010, 2020, 3030 ]),

    OperatorPlusTest(
      lhs: [],
      rhs: [ 1010, 2020, 3030 ],
      expected: [ 1010, 2020, 3030 ]),

    OperatorPlusTest(
      lhs: [ 1010 ],
      rhs: [ 2020, 3030, 4040 ],
      expected: [ 1010, 2020, 3030, 4040 ]),

    OperatorPlusTest(
      lhs: [ 1010, 2020, 3030 ],
      rhs: [ 4040 ],
      expected: [ 1010, 2020, 3030, 4040 ]),

    OperatorPlusTest(
      lhs: [ 1010, 2020, 3030, 4040 ],
      rhs: [ 5050, 6060, 7070 ],
      expected: [ 1010, 2020, 3030, 4040, 5050, 6060, 7070 ]),

    OperatorPlusTest(
      lhs: [ 1010, 2020, 3030 ],
      rhs: [ 4040, 5050, 6060, 7070 ],
      expected: [ 1010, 2020, 3030, 4040, 5050, 6060, 7070 ]),
  ]

  // RangeReplaceableCollection + Sequence
  for test in tests {
    let lhs = makeWrappedCollection(test.lhs)
    let rhs = MinimalSequence(elements: test.rhs.map(wrapValue))

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.lhs.map { $0.value },
      lhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  // Sequence + RangeReplaceableCollection
  for test in tests {
    let lhs = MinimalSequence(elements: test.lhs.map(wrapValue))
    let rhs = makeWrappedCollection(test.rhs)

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.rhs.map { $0.value },
      rhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  // RangeReplaceableCollection + Collection
  for test in tests {
    let lhs = makeWrappedCollection(test.lhs)
    let rhs = MinimalCollection(elements: test.rhs.map(wrapValue))

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.lhs.map { $0.value },
      lhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
    expectEqualSequence(
      test.rhs.map { $0.value },
      rhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  // RangeReplaceableCollection + same RangeReplaceableCollection
  for test in tests {
    let lhs = makeWrappedCollection(test.lhs)
    let rhs = makeWrappedCollection(test.rhs)

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.lhs.map { $0.value },
      lhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
    expectEqualSequence(
      test.rhs.map { $0.value },
      rhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  // RangeReplaceableCollection + MinimalRangeReplaceableCollection
  for test in tests {
    let lhs = makeWrappedCollection(test.lhs)
    let rhs = MinimalRangeReplaceableCollection(
      elements: test.rhs.map(wrapValue))

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.lhs.map { $0.value },
      lhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
    expectEqualSequence(
      test.rhs.map { $0.value },
      rhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }

  // MinimalRangeReplaceableCollection + RangeReplaceableCollection
  for test in tests {
    let lhs = MinimalRangeReplaceableCollection(
      elements: test.lhs.map(wrapValue))
    let rhs = makeWrappedCollection(test.rhs)

    let result = lhs + rhs
    expectEqualSequence(
      test.expected,
      result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))

    expectEqualSequence(
      test.lhs.map { $0.value },
      lhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
    expectEqualSequence(
      test.rhs.map { $0.value },
      rhs.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//

  } // addRangeReplaceableCollectionTests

  public func addRangeReplaceableBidirectionalCollectionTests<
    C : BidirectionalCollection & RangeReplaceableCollection,
    CollectionWithEquatableElement : BidirectionalCollection & RangeReplaceableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1
  ) where
    CollectionWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addRangeReplaceableCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset,
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
      outOfBoundsIndexOffset: outOfBoundsIndexOffset)

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    testNamePrefix += String(describing: C.Type.self)

//===----------------------------------------------------------------------===//
// removeLast()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeLast()/whereIndexIsBidirectional/semantics") {
  for test in removeLastTests.filter({ $0.numberToRemove == 1 }) {
    var c = makeWrappedCollection(test.collection)
    let removedElement = c.removeLast()
    expectEqual(
      test.collection.last!.value,
      extractValue(removedElement).value,
      stackTrace: SourceLocStack().with(test.loc))
    expectEqualSequence(
      test.expectedCollection,
      c.map { extractValue($0).value },
      "removeLast() shouldn't mutate the head of the collection",
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).removeLast()/whereIndexIsBidirectional/empty/semantics") {
  var c = makeWrappedCollection([])
  expectCrashLater()
  _ = c.removeLast() // Should trap.
}

//===----------------------------------------------------------------------===//
// removeLast(n: Int)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).removeLast(n: Int)/whereIndexIsBidirectional/semantics") {
  for test in removeLastTests {
    var c = makeWrappedCollection(test.collection)
    c.removeLast(test.numberToRemove)
    expectEqualSequence(
      test.expectedCollection,
      c.map { extractValue($0).value },
      "removeLast() shouldn't mutate the head of the collection",
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).removeLast(n: Int)/whereIndexIsBidirectional/empty/semantics") {
  var c = makeWrappedCollection([])
  expectCrashLater()
  c.removeLast(1) // Should trap.
}

self.test("\(testNamePrefix).removeLast(n: Int)/whereIndexIsBidirectional/removeNegative/semantics") {
  var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
  expectCrashLater()
  c.removeLast(-1) // Should trap.
}

self.test("\(testNamePrefix).removeLast(n: Int)/whereIndexIsBidirectional/removeTooMany/semantics") {
  var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
  expectCrashLater()
  c.removeLast(3) // Should trap.
}

//===----------------------------------------------------------------------===//

  } // addRangeReplaceableBidirectionalCollectionTests

  public func addRangeReplaceableRandomAccessCollectionTests<
    C : RandomAccessCollection & RangeReplaceableCollection,
    CollectionWithEquatableElement : RandomAccessCollection & RangeReplaceableCollection
  >(
    _ testNamePrefix: String = "",
    makeCollection: @escaping ([C.Element]) -> C,
    wrapValue: @escaping (OpaqueValue<Int>) -> C.Element,
    extractValue: @escaping (C.Element) -> OpaqueValue<Int>,

    makeCollectionOfEquatable: @escaping ([CollectionWithEquatableElement.Element]) -> CollectionWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> CollectionWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((CollectionWithEquatableElement.Element) -> MinimalEquatableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
    outOfBoundsIndexOffset: Int = 1
  ) where
    CollectionWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addRangeReplaceableBidirectionalCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset)

    addRandomAccessCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset)

    testNamePrefix += String(describing: C.Type.self)

    // No extra checks for collections with random access traversal so far.
  } // addRangeReplaceableRandomAccessCollectionTests
}

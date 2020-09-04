//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import StdlibUnittest

internal enum TestError : Error {
  case error1
  case error2
}

public struct DropFirstTest {
  public var sequence: [Int]
  public let dropElements: Int
  public let expected: [Int]
  public let loc: SourceLoc

  public init(sequence: [Int], dropElements: Int, expected: [Int],
      file: String = #file, line: UInt = #line) {
    self.sequence = sequence
    self.dropElements = dropElements
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "dropFirst() test data")
  }
}

public struct DropLastTest {
  public var sequence: [Int]
  public let dropElements: Int
  public let expected: [Int]
  public let loc: SourceLoc

  public init(sequence: [Int], dropElements: Int, expected: [Int],
      file: String = #file, line: UInt = #line) {
    self.sequence = sequence
    self.dropElements = dropElements
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "dropLast() test data")
  }
}

public struct ElementsEqualTest {
  public let expected: Bool
  public let sequence: [Int]
  public let other: [Int]
  public let expectedLeftoverSequence: [Int]
  public let expectedLeftoverOther: [Int]
  public let loc: SourceLoc

  public init(
    _ expected: Bool, _ sequence: [Int], _ other: [Int],
    _ expectedLeftoverSequence: [Int],
    _ expectedLeftoverOther: [Int],
    file: String = #file, line: UInt = #line,
    comment: String = ""
  ) {
    self.expected = expected
    self.sequence = sequence
    self.other = other
    self.expectedLeftoverSequence = expectedLeftoverSequence
    self.expectedLeftoverOther = expectedLeftoverOther
    self.loc = SourceLoc(file, line, comment: "test data" + comment)
  }

  func flip() -> ElementsEqualTest {
    return ElementsEqualTest(
      expected, other, sequence,
      expectedLeftoverOther, expectedLeftoverSequence,
      file: loc.file, line: loc.line, comment: " (flipped)")
  }
}

public struct ElementsEqualWithPredicateTest {
    public let expected: Bool
    public let sequence: [Int]
    public let other: [String]
    public let predicate: (Int, String) -> Bool
    public let expectedLeftoverSequence: [Int]
    public let expectedLeftoverOther: [String]
    public let loc: SourceLoc
    
    public init(
        _ expected: Bool, _ sequence: [Int], _ other: [String],
        _ predicate: @escaping (Int, String) -> Bool,
        _ expectedLeftoverSequence: [Int],
        _ expectedLeftoverOther: [String],
        file: String = #file, line: UInt = #line,
        comment: String = ""
        ) {
        self.expected = expected
        self.sequence = sequence
        self.other = other
        self.predicate = predicate
        self.expectedLeftoverSequence = expectedLeftoverSequence
        self.expectedLeftoverOther = expectedLeftoverOther
        self.loc = SourceLoc(file, line, comment: "test data" + comment)
    }
}

public struct EnumerateTest {
  public let expected: [(Int, Int)]
  public let sequence: [Int]
  public let loc: SourceLoc

  public init(
    _ expected: [(Int, Int)], _ sequence: [Int],
    file: String = #file, line: UInt = #line,
    comment: String = ""
  ) {
    self.expected = expected
    self.sequence = sequence
    self.loc = SourceLoc(file, line, comment: "test data" + comment)
  }
}

public struct FilterTest {
  public let expected: [Int]
  public let sequence: [Int]
  public let includeElement: (Int) -> Bool
  public let loc: SourceLoc

  public init(
    _ expected: [Int],
    _ sequence: [Int],
    _ includeElement: @escaping (Int) -> Bool,
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.includeElement = includeElement
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct PredicateCountTest {
  public let expected: Int
  public let sequence: [Int]
  public let includeElement: (Int) -> Bool
  public let loc: SourceLoc

  public init(
    _ expected: Int,
    _ sequence: [Int],
    _ includeElement: @escaping (Int) -> Bool,
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.includeElement = includeElement
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct FindTest {
  public let expected: Int?
  public let element: MinimalEquatableValue
  public let sequence: [MinimalEquatableValue]
  public let expectedLeftoverSequence: [MinimalEquatableValue]
  public let loc: SourceLoc

  public init(
    expected: Int?, element: Int, sequence: [Int],
    expectedLeftoverSequence: [Int] = [],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.element = MinimalEquatableValue(element)
    self.sequence = sequence.enumerated().map {
      return MinimalEquatableValue($1, identity: $0) 
    }
    self.expectedLeftoverSequence = expectedLeftoverSequence.map(
      MinimalEquatableValue.init)
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct CollectionBinaryOperationTest {
  public let expected: [MinimalEquatableValue]
  public let lhs: [MinimalEquatableValue]
  public let rhs: [MinimalEquatableValue]
  public let loc: SourceLoc

  public init(
    expected: [Int], lhs: [Int], rhs: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected.enumerated().map {
      return MinimalEquatableValue($1, identity: $0) 
    }
    self.lhs = lhs.map {
      return MinimalEquatableValue($0, identity: $0) 
    }
    self.rhs = rhs.map {
      return MinimalEquatableValue($0, identity: $0) 
    }
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct CollectionPredicateTest {
  public let expected: Bool
  public let lhs: [MinimalEquatableValue]
  public let rhs: [MinimalEquatableValue]
  public let loc: SourceLoc

  public init(
    expected: Bool, lhs: [Int], rhs: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.lhs = lhs.enumerated().map {
      return MinimalEquatableValue($1, identity: $0) 
    }
    self.rhs = rhs.enumerated().map {
      return MinimalEquatableValue($1, identity: $0) 
    }
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct FlatMapTest {
  public let expected: [Int32]
  public let sequence: [Int]
  public let transform: (Int) -> [Int32]
  public let loc: SourceLoc

  public init(
    expected: [Int32],
    sequence: [Int],
    transform: @escaping (Int) -> [Int32],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.transform = transform
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct FlatMapToOptionalTest {
  public let expected: [Int32]
  public let sequence: [Int]
  public let transform: (Int) -> Int32?
  public let loc: SourceLoc

  public init(
    _ expected: [Int32],
    _ sequence: [Int],
    _ transform: @escaping (Int) -> Int32?,
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.transform = transform
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

/// This test performs a side effect on each element of `sequence`. The expected
/// side effect for the purposes of testing is to append each element to an
/// external array, which can be compared to `sequence`.
internal struct ForEachTest {
  let sequence: [Int]
  let loc: SourceLoc

  init(
    _ sequence: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.sequence = sequence
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct LexicographicallyPrecedesTest {
  public let expected: ExpectedComparisonResult
  public let sequence: [Int]
  public let other: [Int]
  public let expectedLeftoverSequence: [Int]
  public let expectedLeftoverOther: [Int]
  public let loc: SourceLoc

  public init(
    _ expected: ExpectedComparisonResult, _ sequence: [Int], _ other: [Int],
    _ expectedLeftoverSequence: [Int],
    _ expectedLeftoverOther: [Int],
    file: String = #file, line: UInt = #line,
    comment: String = ""
  ) {
    self.expected = expected
    self.sequence = sequence
    self.other = other
    self.expectedLeftoverSequence = expectedLeftoverSequence
    self.expectedLeftoverOther = expectedLeftoverOther
    self.loc = SourceLoc(file, line, comment: "test data" + comment)
  }

  func flip() -> LexicographicallyPrecedesTest {
    return LexicographicallyPrecedesTest(
      expected.flip(), other, sequence,
      expectedLeftoverOther, expectedLeftoverSequence,
      file: loc.file, line: loc.line, comment: " (flipped)")
  }
}

public struct MapTest {
  public let expected: [Int32]
  public let sequence: [Int]
  public let transform: (Int) -> Int32
  public let loc: SourceLoc

  public init(
    _ expected: [Int32],
    _ sequence: [Int],
    _ transform: @escaping (Int) -> Int32,
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.transform = transform
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct MinMaxTest {
  public let expectedMinValue: Int?
  public let expectedMinIndex: Int?
  public let expectedMaxValue: Int?
  public let expectedMaxIndex: Int?
  public let sequence: [Int]
  public let loc: SourceLoc

  public init(
    minValue expectedMinValue: Int?,
    index expectedMinIndex: Int?,
    maxValue expectedMaxValue: Int?,
    index expectedMaxIndex: Int?,
    _ sequence: [Int],
    file: String = #file, line: UInt = #line,
    comment: String = ""
  ) {
    self.expectedMinValue = expectedMinValue
    self.expectedMinIndex = expectedMinIndex
    self.expectedMaxValue = expectedMaxValue
    self.expectedMaxIndex = expectedMaxIndex
    self.sequence = sequence
    self.loc = SourceLoc(file, line, comment: "test data" + comment)
  }
}

public struct PrefixTest {
  public var sequence: [Int]
  public let maxLength: Int
  public let expected: [Int]
  public let loc: SourceLoc

  public init(sequence: [Int], maxLength: Int, expected: [Int],
      file: String = #file, line: UInt = #line) {
    self.sequence = sequence
    self.maxLength = maxLength
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "prefix() test data")
  }
}

public struct ReduceTest {
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

public struct ReverseTest {
  public let expected: [Int]
  public let sequence: [Int]
  public let loc: SourceLoc

  public init(
    _ expected: [Int], _ sequence: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

public struct SuffixTest {
  public var sequence: [Int]
  public let maxLength: Int
  public let expected: [Int]
  public let loc: SourceLoc

  public init(sequence: [Int], maxLength: Int, expected: [Int],
      file: String = #file, line: UInt = #line) {
    self.sequence = sequence
    self.maxLength = maxLength
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "suffix() test data")
  }
}

public struct SplitTest {
  public var sequence: [Int]
  public let maxSplits: Int
  public let separator: Int
  public let omittingEmptySubsequences: Bool
  public let expected: [[Int]]
  public let loc: SourceLoc

  public init(
    sequence: [Int], maxSplits: Int, separator: Int, expected: [[Int]],
    omittingEmptySubsequences: Bool, file: String = #file, line: UInt = #line
  ) {
    self.sequence = sequence
    self.maxSplits = maxSplits
    self.separator = separator
    self.omittingEmptySubsequences = omittingEmptySubsequences
    self.expected = expected
    self.loc = SourceLoc(file, line, comment: "suffix() test data")
  }
}

public struct StartsWithTest {
  public let expected: Bool
  public let sequence: [Int]
  public let prefix: [Int]
  public let expectedLeftoverSequence: [Int]
  public let expectedLeftoverPrefix: [Int]
  public let loc: SourceLoc

  public init(
    _ expected: Bool, _ sequence: [Int], _ prefix: [Int],
    _ expectedLeftoverSequence: [Int],
    _ expectedLeftoverPrefix: [Int],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.prefix = prefix
    self.expectedLeftoverSequence = expectedLeftoverSequence
    self.expectedLeftoverPrefix = expectedLeftoverPrefix
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}
public struct ZipTest {
  public let expected: [(Int, Int32)]
  public let sequence: [Int]
  public let other: [Int32]
  public let expectedLeftoverSequence: [Int]
  public let expectedLeftoverOther: [Int32]
  public let loc: SourceLoc

  public init(
    _ expected: [(Int, Int32)],
    sequences sequence: [Int],
    _ other: [Int32],
    leftovers expectedLeftoverSequence: [Int],
    _ expectedLeftoverOther: [Int32],
    file: String = #file, line: UInt = #line
  ) {
    self.expected = expected
    self.sequence = sequence
    self.other = other
    self.expectedLeftoverSequence = expectedLeftoverSequence
    self.expectedLeftoverOther = expectedLeftoverOther
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}


public let elementsEqualTests: [ElementsEqualTest] = [
  ElementsEqualTest(true, [], [], [], []),

  ElementsEqualTest(false, [ 1 ], [], [], []),
  ElementsEqualTest(false, [], [ 1 ], [], []),

  ElementsEqualTest(false, [ 1, 2 ], [], [ 2 ], []),
  ElementsEqualTest(false, [], [ 1, 2 ], [], [ 2 ]),

  ElementsEqualTest(false, [ 1, 2, 3, 4 ], [ 1, 2 ], [ 4 ], []),
  ElementsEqualTest(false, [ 1, 2 ], [ 1, 2, 3, 4 ], [], [ 4 ]),
].flatMap { [ $0, $0.flip() ] }

func elementsEqualPredicate(_ x: Int, y: String) -> Bool {
    if let intVal = Int(y) {
        return x == intVal
    } else {
        return false
    }
}

public let elementsEqualWithPredicateTests: [ElementsEqualWithPredicateTest] = [
    ElementsEqualWithPredicateTest(true, [], [], elementsEqualPredicate, [], []),
    
    ElementsEqualWithPredicateTest(false, [ 1 ], [], elementsEqualPredicate, [ 1 ], []),
    ElementsEqualWithPredicateTest(false, [], [ "1" ], elementsEqualPredicate, [], [ "1" ]),
    
    ElementsEqualWithPredicateTest(false, [ 1, 2 ], [], elementsEqualPredicate, [ 1, 2 ], []),
    ElementsEqualWithPredicateTest(false, [], [ "1", "2" ], elementsEqualPredicate, [], [ "1", "2" ]),
    
    ElementsEqualWithPredicateTest(false, [ 1, 2, 3, 4 ], [ "1", "2" ], elementsEqualPredicate, [ 3, 4 ], []),
    ElementsEqualWithPredicateTest(false, [ 1, 2 ], [ "1", "2", "3", "4" ], elementsEqualPredicate, [], [ "3", "4" ]),
    
    ElementsEqualWithPredicateTest(true, [ 1, 2, 3, 4 ], [ "1", "2", "3", "4" ], elementsEqualPredicate, [], []),
    ElementsEqualWithPredicateTest(true, [ 1, 2 ], [ "1", "2" ], elementsEqualPredicate, [], []),
]

public let enumerateTests = [
  EnumerateTest([], []),
  EnumerateTest([ (0, 10) ], [ 10 ]),
  EnumerateTest([ (0, 10), (1, 20) ], [ 10, 20 ]),
  EnumerateTest([ (0, 10), (1, 20), (2, 30) ], [ 10, 20, 30 ]),
]

public let filterTests = [
  FilterTest(
    [], [],
    { _ -> Bool in expectUnreachable(); return true }),

  FilterTest([], [ 0, 30, 10, 90 ], { _ -> Bool in false }),
  FilterTest(
    [ 0, 30, 10, 90 ], [ 0, 30, 10, 90 ], { _ -> Bool in true }
  ),
  FilterTest(
    [ 0, 30, 90 ], [ 0, 30, 10, 90 ], { (x: Int) -> Bool in x % 3 == 0 }
  ),
]

public let predicateCountTests = [
  PredicateCountTest(
    0, [],
    { _ -> Bool in expectUnreachable(); return true }),

  PredicateCountTest(0, [ 0, 30, 10, 90 ], { _ -> Bool in false }),
  PredicateCountTest(
    4, [ 0, 30, 10, 90 ], { _ -> Bool in true }
  ),
  PredicateCountTest(
    3, [ 0, 30, 10, 90 ], { (x: Int) -> Bool in x % 3 == 0 }
  ),
]


public let findTests = [
  FindTest(
    expected: nil,
    element: 42,
    sequence: [],
    expectedLeftoverSequence: []),

  FindTest(
    expected: nil,
    element: 42,
    sequence: [ 1010 ],
    expectedLeftoverSequence: []),
  FindTest(
    expected: 0,
    element: 1010,
    sequence: [ 1010 ],
    expectedLeftoverSequence: []),

  FindTest(
    expected: nil,
    element: 42,
    sequence: [ 1010, 1010 ],
    expectedLeftoverSequence: []),
  FindTest(
    expected: 0,
    element: 1010,
    sequence: [ 1010, 1010 ],
    expectedLeftoverSequence: [ 1010 ]),

  FindTest(
    expected: nil,
    element: 42,
    sequence: [ 1010, 2020, 3030, 4040 ],
    expectedLeftoverSequence: []),
  FindTest(
    expected: 0,
    element: 1010,
    sequence: [ 1010, 2020, 3030, 4040 ],
    expectedLeftoverSequence: [ 2020, 3030, 4040 ]),
  FindTest(
    expected: 1,
    element: 2020,
    sequence: [ 1010, 2020, 3030, 4040 ],
    expectedLeftoverSequence: [ 3030, 4040 ]),
  FindTest(
    expected: 2,
    element: 3030,
    sequence: [ 1010, 2020, 3030, 4040 ],
    expectedLeftoverSequence: [ 4040 ]),
  FindTest(
    expected: 3,
    element: 4040,
    sequence: [ 1010, 2020, 3030, 4040 ],
    expectedLeftoverSequence: []),

  FindTest(
    expected: 1,
    element: 2020,
    sequence: [ 1010, 2020, 3030, 2020, 4040 ],
    expectedLeftoverSequence: [ 3030, 2020, 4040 ]),
]

public let unionTests = [
  CollectionBinaryOperationTest(expected: [1, 2, 3, 4, 5], lhs: [1, 3, 5], rhs: [2, 4]),
  CollectionBinaryOperationTest(expected: [3, 5], lhs: [3], rhs: [5])
]

public let intersectionTests = [
  CollectionBinaryOperationTest(expected: [1, 5], lhs: [1, 3, 5], rhs: [1, 2, 5])
]

public let symmetricDifferenceTests = [
  CollectionBinaryOperationTest(expected: [1, 3, 5], lhs: [1, 2, 3, 4], rhs: [2, 4, 5])
]

public let subtractTests = [
  CollectionBinaryOperationTest(expected: [1, 3], lhs: [1, 2, 3, 4], rhs: [2, 4, 5])
]

public let subtractingTests = [
  CollectionBinaryOperationTest(expected: [1, 3, 4], lhs: [1, 2, 3, 4, 5], rhs: [2, 5, 6, 7])
]

public let strictSupersetTests = [
  CollectionPredicateTest(expected: true, lhs: [1, 2, 3, 4, 5, 6], rhs: [1, 2, 3, 4]),
  CollectionPredicateTest(expected: false, lhs: [1, 2], rhs: [1, 2, 4])
]

/// For a number of form `NNN_MMM`, returns an array of `NNN` numbers that all
/// have `MMM` as their last three digits.
func flatMapTransformation(_ x: Int) -> [Int32] {
  let repetitions = x / 1000
  let identity = x % 1000
  let range = (1..<(repetitions+1))
  return range.map { Int32($0 * 1000 + identity) }
}

public let flatMapTests = [
  FlatMapTest(
    expected: [],
    sequence: [],
    transform: { _ -> [Int32] in
      expectUnreachable()
      return [ 0xffff ]
    }),

  FlatMapTest(
    expected: [],
    sequence: [ 1 ],
    transform: { _ -> [Int32] in [] }),
  FlatMapTest(
    expected: [],
    sequence: [ 1, 2 ],
    transform: { _ -> [Int32] in [] }),
  FlatMapTest(
    expected: [],
    sequence: [ 1, 2, 3 ],
    transform: { _ -> [Int32] in [] }),

  FlatMapTest(
    expected: [ 101 ],
    sequence: [ 1 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100) ] }),
  FlatMapTest(
    expected: [ 101, 102 ],
    sequence: [ 1, 2 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100) ] }),
  FlatMapTest(
    expected: [ 101, 102, 103 ],
    sequence: [ 1, 2, 3 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100) ] }),

  FlatMapTest(
    expected: [ 101, 201 ],
    sequence: [ 1 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100), Int32(x + 200) ] }),
  FlatMapTest(
    expected: [ 101, 201, 102, 202 ],
    sequence: [ 1, 2 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100), Int32(x + 200) ] }),
  FlatMapTest(
    expected: [ 101, 201, 102, 202, 103, 203 ],
    sequence: [ 1, 2, 3 ],
    transform: { (x: Int) -> [Int32] in [ Int32(x + 100), Int32(x + 200) ] }),

  FlatMapTest(
    expected: [ 1_071, 1_075 ],
    sequence: [ 1_071, 72, 73, 74, 1_075 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_072, 1_073, 2_073 ],
    sequence: [ 1, 1_072, 2_073 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_071, 2_071, 1_073, 2_073, 3_073, 1_074 ],
    sequence: [ 2_071, 2, 3_073, 1_074 ],
    transform: flatMapTransformation),

  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1, 2, 1_073, 4, 5, 2_076, 7, 8, 3_079, 10, 11 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1_073, 4, 5, 2_076, 7, 8, 3_079, 10, 11 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1, 2, 1_073, 4, 5, 2_076, 7, 8, 3_079 ],
    transform: flatMapTransformation),

  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1, 1_073, 4, 2_076, 7, 3_079, 10 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1_073, 4, 2_076, 7, 3_079, 10 ],
    transform: flatMapTransformation),
  FlatMapTest(
    expected: [ 1_073, 1_076, 2_076, 1_079, 2_079, 3_079 ],
    sequence: [ 1, 1_073, 4, 2_076, 7, 3_079 ],
    transform: flatMapTransformation),
]

public let flatMapToOptionalTests = [
  FlatMapToOptionalTest(
    [], [],
    { _ -> Int32? in expectUnreachable(); return 0xffff }),

  FlatMapToOptionalTest([], [ 1 ], { _ -> Int32? in nil }),
  FlatMapToOptionalTest([], [ 1, 2 ], { _ -> Int32? in nil }),
  FlatMapToOptionalTest([], [ 1, 2, 3 ], { _ -> Int32? in nil }),

  FlatMapToOptionalTest(
    [ 1 ], [ 1 ],
    { (x: Int) -> Int32? in x > 10 ? nil : Int32(x) }),
  FlatMapToOptionalTest(
    [ 2 ], [ 11, 2, 13, 14 ],
    { (x: Int) -> Int32? in x > 10 ? nil : Int32(x) }),
  FlatMapToOptionalTest(
    [ 1, 4 ], [ 1, 12, 13, 4 ],
    { (x: Int) -> Int32? in x > 10 ? nil : Int32(x) }),
  FlatMapToOptionalTest(
    [ 1, 2, 3 ], [ 1, 2, 3 ],
    { (x: Int) -> Int32? in x > 10 ? nil : Int32(x) }),
]


public let dropFirstTests = [
  DropFirstTest(
    sequence: [],
    dropElements: 0,
    expected: []
  ),
  DropFirstTest(
    sequence: [1010, 2020, 3030],
    dropElements: 1,
    expected: [2020, 3030]
  ),
  DropFirstTest(
    sequence: [1010, 2020, 3030],
    dropElements: 2,
    expected: [3030]
  ),
  DropFirstTest(
    sequence: [1010, 2020, 3030],
    dropElements: 3,
    expected: []
  ),
  DropFirstTest(
    sequence: [1010, 2020, 3030],
    dropElements: 777,
    expected: []
  ),
  DropFirstTest(
    sequence: [1010, 2020, 3030],
    dropElements: 0,
    expected: [1010, 2020, 3030]
  ),
]

public let dropLastTests = [
  DropLastTest(
    sequence: [],
    dropElements: 0,
    expected: []
  ),
  DropLastTest(
    sequence: [1010, 2020, 3030],
    dropElements: 1,
    expected: [1010, 2020]
  ),
  DropLastTest(
    sequence: [1010, 2020, 3030],
    dropElements: 2,
    expected: [1010]
  ),
  DropLastTest(
    sequence: [1010, 2020, 3030],
    dropElements: 3,
    expected: []
  ),
  DropLastTest(
    sequence: [1010, 2020, 3030],
    dropElements: 777,
    expected: []
  ),
  DropLastTest(
    sequence: [1010, 2020, 3030],
    dropElements: 0,
    expected: [1010, 2020, 3030]
  ),
]

internal let forEachTests = [
  ForEachTest([]),
  ForEachTest([1010]),
  ForEachTest([1010, 2020, 3030, 4040, 5050]),
]

public let lexicographicallyPrecedesTests = [
  LexicographicallyPrecedesTest(.eq, [], [], [], []),
  LexicographicallyPrecedesTest(.eq, [ 1 ], [ 1 ], [], []),

  LexicographicallyPrecedesTest(.gt, [ 1 ], [], [], []),

  LexicographicallyPrecedesTest(.gt, [ 1 ], [ 0 ], [], []),
  LexicographicallyPrecedesTest(.eq, [ 1 ], [ 1 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1 ], [ 2 ], [], []),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [], [ 2 ], []),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 0 ], [ 2 ], []),
  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 1 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 2 ], [ 2 ], []),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 0, 0 ], [ 2 ], [ 0 ]),
  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 1, 0 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 2, 0 ], [ 2 ], [ 0 ]),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 0, 1 ], [ 2 ], [ 1 ]),
  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 1, 1 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 2, 1 ], [ 2 ], [ 1 ]),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 0, 2 ], [ 2 ], [ 2 ]),
  LexicographicallyPrecedesTest(.eq, [ 1, 2 ], [ 1, 2 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 2, 2 ], [ 2 ], [ 2 ]),

  LexicographicallyPrecedesTest(.gt, [ 1, 2 ], [ 0, 3 ], [ 2 ], [ 3 ]),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 1, 3 ], [], []),
  LexicographicallyPrecedesTest(.lt, [ 1, 2 ], [ 2, 3 ], [ 2 ], [ 3 ]),
].flatMap { [ $0, $0.flip() ] }

public let mapTests = [
  MapTest(
    [], [],
    { _ -> Int32 in expectUnreachable(); return 0xffff }),

  MapTest([ 101 ], [ 1 ],
    { (x: Int) -> Int32 in Int32(x + 100) }),
  MapTest([ 101, 102 ], [ 1, 2 ],
    { (x: Int) -> Int32 in Int32(x + 100) }),
  MapTest([ 101, 102, 103 ], [ 1, 2, 3 ],
    { (x: Int) -> Int32 in Int32(x + 100) }),
  MapTest(Array(101..<200), Array(1..<100),
    { (x: Int) -> Int32 in Int32(x + 100) }),
]

public let minMaxTests = [
  MinMaxTest(
    minValue: nil, index: nil,
    maxValue: nil, index: nil,
    []),
  MinMaxTest(
    minValue: 42, index: 0,
    maxValue: 42, index: 0,
    [ 42 ]),
  MinMaxTest(
    minValue: -1, index: 1,
    maxValue: 30, index: 2,
    [ 10, -1, 30, -1, 30 ]),
  MinMaxTest(
    minValue: -2, index: 5,
    maxValue: 31, index: 6,
    [ 10, -1, 30, -1, 30, -2, 31 ]),
]

public let reduceTests = [
  ReduceTest([]),
  ReduceTest([ 1 ]),
  ReduceTest([ 1, 2 ]),
  ReduceTest([ 1, 2, 3 ]),
  ReduceTest([ 1, 2, 3, 4, 5, 6, 7 ]),
]

public let reverseTests: [ReverseTest] = [
  ReverseTest([], []),
  ReverseTest([ 1 ], [ 1 ]),
  ReverseTest([ 2, 1 ], [ 1, 2 ]),
  ReverseTest([ 3, 2, 1 ], [ 1, 2, 3 ]),
  ReverseTest([ 4, 3, 2, 1 ], [ 1, 2, 3, 4]),
  ReverseTest(
    [ 7, 6, 5, 4, 3, 2, 1 ],
    [ 1, 2, 3, 4, 5, 6, 7 ]),
]

public let splitTests: [SplitTest] = [
  //
  // Empty sequence.
  //

  // Empty sequence, maxSplits == 0.
  SplitTest(
    sequence: [],
    maxSplits: 0,
    separator: 99,
    expected: [[]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [],
    maxSplits: 0,
    separator: 99,
    expected: [],
    omittingEmptySubsequences: true
  ),

  // Empty sequence, maxSplits == 1.
  SplitTest(
    sequence: [],
    maxSplits: 1,
    separator: 99,
    expected: [[]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [],
    maxSplits: 1,
    separator: 99,
    expected: [],
    omittingEmptySubsequences: true
  ),

  // Empty sequence, maxSplits == Int.max.
  SplitTest(
    sequence: [],
    maxSplits: Int.max,
    separator: 99,
    expected: [[]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [],
    maxSplits: Int.max,
    separator: 99,
    expected: [],
    omittingEmptySubsequences: true
  ),

  //
  // 1-element sequence.
  //

  // 1-element sequence, maxSplits == 0.
  SplitTest(
    sequence: [1],
    maxSplits: 0,
    separator: 1,
    expected: [[1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 0,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 0,
    separator: 1,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 0,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),

  // 1-element sequence, maxSplits == 1.
  SplitTest(
    sequence: [1],
    maxSplits: 1,
    separator: 1,
    expected: [[], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 1,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 1,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1],
    maxSplits: 1,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),

  // 1-element sequence, maxSplits == Int.max.
  SplitTest(
    sequence: [1],
    maxSplits: Int.max,
    separator: 1,
    expected: [[], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: Int.max,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1],
    maxSplits: Int.max,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1],
    maxSplits: Int.max,
    separator: 99,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),

  //
  // 2-element sequence [1, 2].
  //

  // 2-element sequence [1, 2], maxSplits == 0.
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 1,
    expected: [[1, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 2,
    expected: [[1, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 1,
    expected: [[1, 2]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 2,
    expected: [[1, 2]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 0,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: true
  ),

  // 2-element sequence [1, 2], maxSplits == 1.
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 1,
    expected: [[], [2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 2,
    expected: [[1], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 1,
    expected: [[2]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 2,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: 1,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: true
  ),

  // 2-element sequence [1, 2], maxSplits == Int.max.
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 1,
    expected: [[], [2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 1,
    expected: [[2]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2],
    maxSplits: Int.max,
    separator: 99,
    expected: [[1, 2]],
    omittingEmptySubsequences: true
  ),

  //
  // 2-element sequence [1, 1].
  //

  // 2-element sequence [1, 1], maxSplits == 0.
  SplitTest(
    sequence: [1, 1],
    maxSplits: 0,
    separator: 1,
    expected: [[1, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 0,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 0,
    separator: 1,
    expected: [[1, 1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 0,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: true
  ),

  // 2-element sequence [1, 1], maxSplits == 1.
  SplitTest(
    sequence: [1, 1],
    maxSplits: 1,
    separator: 1,
    expected: [[], [1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 1,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 1,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 1,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: true
  ),

  // 2-element sequence [1, 1], maxSplits == 2.
  SplitTest(
    sequence: [1, 1],
    maxSplits: 2,
    separator: 1,
    expected: [[], [], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 2,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 2,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1],
    maxSplits: 2,
    separator: 99,
    expected: [[1, 1]],
    omittingEmptySubsequences: true
  ),

  //
  // 3-element sequence [1, 1, 1].
  //

  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 1,
    separator: 1,
    expected: [[], [1, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 2,
    separator: 1,
    expected: [[], [], [1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 3,
    separator: 1,
    expected: [[], [], [], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: Int.max,
    separator: 1,
    expected: [[], [], [], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 1,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 2,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: 3,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 1, 1],
    maxSplits: Int.max,
    separator: 1,
    expected: [],
    omittingEmptySubsequences: true
  ),

  //
  // Other tests.
  //

  SplitTest(
    sequence: [1, 2, 2, 2, 1],
    maxSplits: 1,
    separator: 2,
    expected: [[1], [2, 2, 1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2, 1],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [], [], [1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2, 1],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [], [], [1], [], [], [1], [], [], [1]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [1], [1], [1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2],
    maxSplits: Int.max,
    separator: 2,
    expected:
      [[], [], [], [1], [], [], [1], [], [], [1], [], [], [1], [], [], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [1], [1], [1]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2, 3, 4, 5, 6, 7],
    maxSplits: 4,
    separator: 4,
    expected: [[1, 2, 3], [5, 6, 7]],
    omittingEmptySubsequences: true
  ),
  SplitTest(
    sequence: [1, 2, 3, 3, 4, 5, 6, 7],
    maxSplits: 3,
    separator: 3,
    expected: [[1, 2], [], [4, 5, 6, 7]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2, 2, 2, 2, 2, 2],
    maxSplits: 3,
    separator: 2,
    expected: [[1], [], [], [2, 2, 2, 2, 2]],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2],
    maxSplits: Int.max,
    separator: 2,
    expected: [[1], [], [], []],
    omittingEmptySubsequences: false
  ),
  SplitTest(
    sequence: [1, 2, 2, 2],
    maxSplits: Int.max,
    separator: 3,
    expected: [[1, 2, 2, 2]],
    omittingEmptySubsequences: false
  ),
]

public let prefixTests = [
  PrefixTest(
    sequence: [],
    maxLength: 0,
    expected: []
  ),
  PrefixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 1,
    expected: [1010]
  ),
  PrefixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 2,
    expected: [1010, 2020]
  ),
  PrefixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 3,
    expected: [1010, 2020, 3030]
  ),
  PrefixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 777,
    expected: [1010, 2020, 3030]
  ),
  PrefixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 0,
    expected: []
  ),
  PrefixTest(
    sequence: [2, 3, 5, 7, 11],
    maxLength: 6,
    expected: [2, 3, 5, 7, 11]
  ),
  PrefixTest(
    sequence: [2, 3, 5, 7, 11],
    maxLength: 5,
    expected: [2, 3, 5, 7, 11]
  ),
]

public let startsWithTests = [
  // Corner cases.
  StartsWithTest(true, [], [], [], []),

  StartsWithTest(false, [], [ 1 ], [], []),
  StartsWithTest(true, [ 1 ], [], [], []),

  // Equal sequences.
  StartsWithTest(true, [ 1 ], [ 1 ], [], []),
  StartsWithTest(true, [ 1, 2 ], [ 1, 2 ], [], []),

  // Proper prefix.
  StartsWithTest(true, [ 0, 1, 2 ], [ 0, 1 ], [], []),
  StartsWithTest(false, [ 0, 1 ], [ 0, 1, 2 ], [], []),

  StartsWithTest(true, [ 1, 2, 3, 4 ], [ 1, 2 ], [ 4 ], []),
  StartsWithTest(false, [ 1, 2 ], [ 1, 2, 3, 4 ], [], [ 4 ]),

  // Not a prefix.
  StartsWithTest(false, [ 1, 2, 3, 4 ], [ 1, 2, 10 ], [ 4 ], []),
  StartsWithTest(false, [ 1, 2, 10 ], [ 1, 2, 3, 4 ], [], [ 4 ]),

  StartsWithTest(false, [ 1, 2, 3, 4, 10 ], [ 1, 2, 10 ], [ 4, 10 ], []),
  StartsWithTest(false, [ 1, 2, 10 ], [ 1, 2, 3, 4, 10 ], [], [ 4, 10 ]),
]


public let suffixTests = [
  SuffixTest(
    sequence: [],
    maxLength: 0,
    expected: []
  ),
  SuffixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 1,
    expected: [3030]
  ),
  SuffixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 2,
    expected: [2020, 3030]
  ),
  SuffixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 3,
    expected: [1010, 2020, 3030]
  ),
  SuffixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 777,
    expected: [1010, 2020, 3030]
  ),
  SuffixTest(
    sequence: [1010, 2020, 3030],
    maxLength: 0,
    expected: []
  ),
]

public let zipTests = [
  ZipTest([], sequences: [], [], leftovers: [], []),
  ZipTest([], sequences: [], [ 1 ], leftovers: [], [ 1 ]),
  ZipTest([], sequences: [], [ 1, 2 ], leftovers: [], [ 1, 2 ]),
  ZipTest([], sequences: [], [ 1, 2, 3 ], leftovers: [], [ 1, 2, 3 ]),

  ZipTest([], sequences: [ 10 ], [], leftovers: [], []),
  ZipTest([ (10, 1) ], sequences: [ 10 ], [ 1 ], leftovers: [], []),
  ZipTest([ (10, 1) ], sequences: [ 10 ], [ 1, 2 ], leftovers: [], [ 2 ]),
  ZipTest([ (10, 1) ], sequences: [ 10 ], [ 1, 2, 3 ], leftovers: [], [ 2, 3 ]),

  ZipTest(
    [],
    sequences: [ 10, 20 ], [],
    leftovers: [ 20 ], []),
  ZipTest(
    [ (10, 1) ],
    sequences: [ 10, 20 ], [ 1 ],
    leftovers: [], []),
  ZipTest(
    [ (10, 1), (20, 2) ],
    sequences: [ 10, 20 ], [ 1, 2 ],
    leftovers: [], []),
  ZipTest(
    [ (10, 1), (20, 2) ],
    sequences: [ 10, 20 ], [ 1, 2, 3 ],
    leftovers: [], [ 3 ]),

  ZipTest(
    [],
    sequences: [ 10, 20, 30 ], [],
    leftovers: [ 20, 30 ], []),
  ZipTest(
    [ (10, 1) ],
    sequences: [ 10, 20, 30 ], [ 1 ],
    leftovers: [ 30 ], []),
  ZipTest(
    [ (10, 1), (20, 2) ],
    sequences: [ 10, 20, 30 ], [ 1, 2 ],
    leftovers: [], []),
  ZipTest(
    [ (10, 1), (20, 2), (30, 3) ],
    sequences: [ 10, 20, 30 ], [ 1, 2, 3 ],
    leftovers: [], []),
]

public func callGenericUnderestimatedCount<S : Sequence>(_ s: S) -> Int {
  return s.underestimatedCount
}

extension TestSuite {
  public func addSequenceTests<
    S : Sequence,
    SequenceWithEquatableElement : Sequence
  >(
    _ testNamePrefix: String = "",
    makeSequence: @escaping ([S.Element]) -> S,
    wrapValue: @escaping (OpaqueValue<Int>) -> S.Element,
    extractValue: @escaping (S.Element) -> OpaqueValue<Int>,

    makeSequenceOfEquatable: @escaping ([SequenceWithEquatableElement.Element]) -> SequenceWithEquatableElement,
    wrapValueIntoEquatable: @escaping (MinimalEquatableValue) -> SequenceWithEquatableElement.Element,
    extractValueFromEquatable: @escaping ((SequenceWithEquatableElement.Element) -> MinimalEquatableValue),

    resiliencyChecks: CollectionMisuseResiliencyChecks = .all
  ) where
    SequenceWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    if !checksAdded.insert(
        "\(testNamePrefix).\(S.self).\(#function)"
      ).inserted {
      return
    }

    func makeWrappedSequence(_ elements: [OpaqueValue<Int>]) -> S {
      return makeSequence(elements.map(wrapValue))
    }

    func makeWrappedSequenceWithEquatableElement(
      _ elements: [MinimalEquatableValue]
    ) -> SequenceWithEquatableElement {
      return makeSequenceOfEquatable(elements.map(wrapValueIntoEquatable))
    }

    testNamePrefix += String(describing: S.self)

    // FIXME: swift-3-indexing-model: add tests for `underestimatedCount`
    // Check that it is non-negative, and an underestimate of the actual
    // element count.

//===----------------------------------------------------------------------===//
// contains()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).contains()/WhereElementIsEquatable/semantics") {
  for test in findTests {
    let s = makeWrappedSequenceWithEquatableElement(test.sequence)

    expectEqual(
      test.expected != nil,
      s.contains(wrapValueIntoEquatable(test.element)),
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// dropFirst()
//===----------------------------------------------------------------------===//
self.test("\(testNamePrefix).dropFirst/semantics") {
  for test in dropFirstTests {
    let s = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    let result = s.dropFirst(test.dropElements)
    expectEqualSequence(
      test.expected, result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).dropFirst/semantics/equivalence") {
  // Calling dropFirst(1) twice on a sequence should be the same as
  // calling dropFirst(2) once on an equivalent sequence.
  if true {
    let s1 = makeWrappedSequence([1010, 2020, 3030, 4040].map(OpaqueValue.init))
    let s2 = makeWrappedSequence([1010, 2020, 3030, 4040].map(OpaqueValue.init))

    let result0 = s1.dropFirst(1)
    let result1 = result0.dropFirst(1)
    let result2 = s2.dropFirst(2)

    expectEqualSequence(
      result1.map { extractValue($0).value },
      result2.map { extractValue($0).value }
    )
  }
}

self.test("\(testNamePrefix).dropFirst/semantics/dropFirst()==dropFirst(1)") {
  let s1 = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))
  let s2 = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))

  let result1 = s1.dropFirst()
  let result2 = s2.dropFirst(1)

  expectEqualSequence(
    result1.map { extractValue($0).value },
    result2.map { extractValue($0).value }
  )
}

self.test("\(testNamePrefix).dropFirst/semantics/negative") {
  let s = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  _ = s.dropFirst(-1)
}

//===----------------------------------------------------------------------===//
// dropLast()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).dropLast/semantics") {
  for test in dropLastTests {
    let s = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    let result = s.dropLast(test.dropElements)
    expectEqualSequence(test.expected, result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).dropLast/semantics/equivalence") {
  // Calling `dropLast(2)` twice on a sequence is equivalent to calling
  // `dropLast(4)` once.
  if true {
    let s1 = makeWrappedSequence(
      [1010, 2020, 3030, 4040, 5050].map(OpaqueValue.init))
    let s2 = makeWrappedSequence(
      [1010, 2020, 3030, 4040, 5050].map(OpaqueValue.init))

    let droppedOnce = s1.dropLast(4)

    // FIXME: this line should read:
    //
    //   let droppedTwice_ = s2.dropLast(2).dropLast(2)
    //
    // We can change it when we have real default implementations in protocols
    // that don't affect regular name lookup.
    let droppedTwice_ = s2.dropLast(2)
    let droppedTwice = droppedTwice_.dropLast(2)

    expectEqualSequence(droppedOnce, droppedTwice) {
      extractValue($0).value == extractValue($1).value
    }
  }
}

self.test("\(testNamePrefix).dropLast/semantics/negative") {
  let s = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  _ = s.dropLast(-1)
}

//===----------------------------------------------------------------------===//
// drop(while:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).drop(while:)/semantics").forEach(in: findTests) {
  test in
  let s = makeWrappedSequenceWithEquatableElement(test.sequence)
  let closureLifetimeTracker = LifetimeTracked(0)
  let remainingSequence = s.drop {
    _blackHole(closureLifetimeTracker)
    return $0 != wrapValueIntoEquatable(test.element)
  }
  let remaining = Array(remainingSequence)
  let expectedSuffix = test.sequence.suffix(
    from: test.expected ?? test.sequence.endIndex)
  expectEqual(expectedSuffix.count, remaining.count)
  expectEqualSequence(expectedSuffix.map(wrapValueIntoEquatable), remaining)
}

//===----------------------------------------------------------------------===//
// prefix()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).prefix/semantics") {
  for test in prefixTests {
    let s = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    let result = s.prefix(test.maxLength)
    expectEqualSequence(test.expected, result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).prefix/semantics/equivalence") {
  // Calling `prefix(3)` on a sequence twice in a row should be the same
  // as calling it once.
  let expected = [
    1010, 2020, 3030, 4040, 5050,
    6060, 7070, 8080, 9090, 10010
  ].map(OpaqueValue.init)

  let s1 = makeWrappedSequence(expected)
  let s2 = makeWrappedSequence(expected)

  let prefixedOnce = s1.prefix(3)
  let temp = s2.prefix(3)
  let prefixedTwice = temp.prefix(3)

  expectEqualSequence(prefixedOnce, prefixedTwice) {
    extractValue($0).value == extractValue($1).value
  }
}

self.test("\(testNamePrefix).prefix/semantics/negative") {
  let s = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  _ = s.prefix(-1)
}

//===----------------------------------------------------------------------===//
// prefix(while:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).prefix(while:)/semantics").forEach(in: findTests) {
  test in
  let s = makeWrappedSequenceWithEquatableElement(test.sequence)
  let closureLifetimeTracker = LifetimeTracked(0)
  let remainingSequence = s.prefix {
    _blackHole(closureLifetimeTracker)
    return $0 != wrapValueIntoEquatable(test.element)
  }
  let expectedPrefix = test.sequence.prefix(
    upTo: test.expected ?? test.sequence.endIndex)
  let remaining = Array(remainingSequence)
  expectEqual(expectedPrefix.count, remaining.count)
  expectEqualSequence(expectedPrefix.map(wrapValueIntoEquatable), remaining)
}

//===----------------------------------------------------------------------===//
// suffix()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).suffix/semantics") {
  for test in suffixTests {
    let s = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    let result = s.suffix(test.maxLength)
    expectEqualSequence(test.expected, result.map { extractValue($0).value },
      stackTrace: SourceLocStack().with(test.loc))
  }
}

self.test("\(testNamePrefix).suffix/semantics/equivalence") {
  // Calling `suffix(3)` on a sequence twice in a row should be the same
  // as calling it once.
  let expected = [
    1010, 2020, 3030, 4040, 5050,
    6060, 7070, 8080, 9090, 10010
  ].map(OpaqueValue.init)

  let s1 = makeWrappedSequence(expected)
  let s2 = makeWrappedSequence(expected)

  let prefixedOnce = s1.suffix(3)
  let temp = s2.suffix(3)
  let prefixedTwice = temp.prefix(3)

  expectEqualSequence(prefixedOnce, prefixedTwice) {
    extractValue($0).value == extractValue($1).value
  }
}

self.test("\(testNamePrefix).suffix/semantics/negative") {
  let s = makeWrappedSequence([1010, 2020, 3030].map(OpaqueValue.init))
  expectCrashLater()
  _ = s.suffix(-1)
}

//===----------------------------------------------------------------------===//
// split()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).split/closure/semantics") {
  for test in splitTests {
    let closureLifetimeTracker = LifetimeTracked(0)
    expectEqual(1, LifetimeTracked.instances)
    let s: S = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    let result = s.split(
      maxSplits: test.maxSplits,
      omittingEmptySubsequences: test.omittingEmptySubsequences) {
      _blackHole(closureLifetimeTracker)
      return extractValue($0).value == test.separator
    }
    expectEqualSequence(test.expected, result.map {
      $0.map {
        extractValue($0).value
      }
    },
    stackTrace: SourceLocStack().with(test.loc)) { $0 == $1 }
  }
}

self.test("\(testNamePrefix).split/separator/semantics") {
  for test in splitTests {
    let s = makeWrappedSequenceWithEquatableElement(
      test.sequence.map(MinimalEquatableValue.init)
    )
    let separator = wrapValueIntoEquatable(MinimalEquatableValue(test.separator))
    let result = s.split(
      separator: separator,
      maxSplits: test.maxSplits,
      omittingEmptySubsequences: test.omittingEmptySubsequences)
    expectEqualSequence(
      test.expected,
      result.map {
        $0.map {
          extractValueFromEquatable($0).value
        }
      },
      stackTrace: SourceLocStack().with(test.loc)) { $0 == $1 }
  }
}

self.test("\(testNamePrefix).split/semantics/closure/negativeMaxSplit") {
  let s = makeWrappedSequenceWithEquatableElement([MinimalEquatableValue(1)])
  let separator = MinimalEquatableValue(1)
  expectCrashLater()
  _ = s.split(
    maxSplits: -1,
    omittingEmptySubsequences: false) { extractValueFromEquatable($0) == separator }
}

self.test("\(testNamePrefix).split/semantics/separator/negativeMaxSplit") {
  let s = makeWrappedSequenceWithEquatableElement([MinimalEquatableValue(1)])
  let separator = wrapValueIntoEquatable(MinimalEquatableValue(1))
  expectCrashLater()
  _ = s.split(separator: separator, maxSplits: -1, omittingEmptySubsequences: false)
}

//===----------------------------------------------------------------------===//
// forEach()
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).forEach/semantics") {
  for test in forEachTests {
    var elements: [Int] = []
    let closureLifetimeTracker = LifetimeTracked(0)
    let s = makeWrappedSequence(test.sequence.map(OpaqueValue.init))
    s.forEach {
      (element) in
      _blackHole(closureLifetimeTracker)
      elements.append(extractValue(element).value)
    }
    expectEqualSequence(
      test.sequence, elements,
      stackTrace: SourceLocStack().with(test.loc))
  }
}

//===----------------------------------------------------------------------===//
// first(where:)
//===----------------------------------------------------------------------===//

self.test("\(testNamePrefix).first(where:)/semantics") {
  for test in findTests {
    let s = makeWrappedSequenceWithEquatableElement(test.sequence)
    let closureLifetimeTracker = LifetimeTracked(0)
    let found = s.first(where: {
      _blackHole(closureLifetimeTracker)
      return $0 == wrapValueIntoEquatable(test.element)
    })
    expectEqual(
      test.expected == nil ? nil : wrapValueIntoEquatable(test.element),
      found,
      stackTrace: SourceLocStack().with(test.loc))
    if let expectedIdentity = test.expected {
      expectEqual(
        expectedIdentity, extractValueFromEquatable(found!).identity,
        "find() should find only the first element matching its predicate")
    }
  }
}

//===----------------------------------------------------------------------===//
  } // addSequenceTests
}

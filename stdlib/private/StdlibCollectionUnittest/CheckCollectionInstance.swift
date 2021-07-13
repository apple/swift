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

public struct CollectionMisuseResiliencyChecks {
  public enum FailureKind {
    case none
    case trap
  }

  public var creatingOutOfBoundsIndicesBehavior: FailureKind = .trap
  public var subscriptOnOutOfBoundsIndicesBehavior: FailureKind = .trap
  public var subscriptRangeOnOutOfBoundsRangesBehavior: FailureKind = .trap

  public static var all: CollectionMisuseResiliencyChecks {
    return CollectionMisuseResiliencyChecks()
  }

  public static var none: CollectionMisuseResiliencyChecks {
    return CollectionMisuseResiliencyChecks(
      creatingOutOfBoundsIndicesBehavior: .none,
      subscriptOnOutOfBoundsIndicesBehavior: .none,
      subscriptRangeOnOutOfBoundsRangesBehavior: .none)
  }
}


/// Test that the elements of `instances` satisfy
/// the semantic
/// requirements of `Collection`, using `equalityOracle` to
/// generate equality expectations from pairs of positions in
/// `instances`.
///
/// - Precondition: `endIndex` is reachable from all
///   elements of `instances`.
public func checkIncrementable<Instances, BaseCollection>(
  _ instances: Instances,
  of baseCollection: BaseCollection,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  endIndex: Instances.Element,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  BaseCollection : Collection,
  Instances.Element == BaseCollection.Index {

  checkEquatable(instances, oracle: equalityOracle, message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  for i in instances {
    if i != endIndex {
      let next = baseCollection.index(after: i)
      // index(after:) gets us a new index value
      expectNotEqual(i, next, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      // Which is the same as if we apply formIndex(after:)
      var j = i
      baseCollection.formIndex(after: &j)
      expectEqual(j, next, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    }
  }
}

/// Test that the elements of `instances` satisfy
/// some of the semantic
/// requirements of `BidirectionalCollection`, using `equalityOracle` to
/// generate equality expectations from pairs of positions in
/// `instances`.
///
/// - Precondition: all
///   elements of `instances` are reachable from `startIndex`.
public func checkDecrementable<Instances, BaseCollection>(
  _ instances: Instances,
  of baseCollection: BaseCollection,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  startIndex: Instances.Element,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  BaseCollection : BidirectionalCollection,
  Instances.Element == BaseCollection.Index {

  checkEquatable(instances, oracle: equalityOracle, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  for i in instances {
    if i != startIndex {
      let next = baseCollection.index(before: i)
      // index(before:) gets us a new index value
      expectNotEqual(i, next, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      // Which is the same as if we apply formIndex(before:)
      var j = i
      baseCollection.formIndex(before: &j)
      expectEqual(j, next, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    }
  }
}

internal func _checkIncrementalAdvance<Instances, BaseCollection>(
  _ instances: Instances,
  of baseCollection : BaseCollection,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  limit: Instances.Element,
  sign: Int, // 1 or -1
  next: (Instances.Element) -> Instances.Element,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  BaseCollection : Collection,
  Instances.Element == BaseCollection.Index {
  for i in instances {
    let d: Int = sign > 0 ?
      baseCollection.distance(from: i, to: limit) :
      -baseCollection.distance(from: limit, to: i)

    var offset: Int = 0
    for _ in 0...Int64(d * sign) {
      let j = baseCollection.index(i, offsetBy: offset)
      let k = baseCollection.index(i, offsetBy: offset + sign, limitedBy: limit) ?? limit
      let jAtLimit = offset == d
      if jAtLimit {
        expectEqual(limit, j, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
      expectEqual(jAtLimit ? j : next(j), k, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      offset += sign
    }
  }
}

/// Test that the elements of `instances` satisfy the semantic requirements of
/// index for `Collection`, using `equalityOracle` to generate equality
/// expectations from pairs of positions in `instances`.
///
/// - Precondition: `endIndex` is reachable from all elements of
///   `instances`
public func checkForwardIndex<Instances, BaseCollection>(
  _ instances: Instances,
  of baseCollection: BaseCollection,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  endIndex: Instances.Element,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  BaseCollection : Collection,
  Instances.Element == BaseCollection.Index {

  checkIncrementable(instances, of: baseCollection,
    equalityOracle: equalityOracle, endIndex: endIndex, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  _checkIncrementalAdvance(instances, of: baseCollection,
    equalityOracle: equalityOracle, limit: endIndex,
    sign: 1, next: { baseCollection.index(after: $0) }, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

/// Test that the elements of `instances` satisfy the semantic requirements of
/// index for `BidirectionalCollection`, using `equalityOracle` to generate
/// equality expectations from pairs of positions in `instances`.
///
/// - Precondition:
///   - all elements of `instances` are reachable from `startIndex`.
///   - `endIndex` is reachable from all elements of `instances`.
public func checkBidirectionalIndex<Instances, BaseCollection>(
  _ instances: Instances,
  of baseCollection: BaseCollection,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  startIndex: Instances.Element,
  endIndex: Instances.Element,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances: Collection,
  BaseCollection : BidirectionalCollection,
  Instances.Element == BaseCollection.Index {

  checkForwardIndex(instances, of: baseCollection,
    equalityOracle: equalityOracle, endIndex: endIndex)

  checkDecrementable(instances, of: baseCollection,
    equalityOracle: equalityOracle, startIndex: startIndex, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  _checkIncrementalAdvance(instances, of: baseCollection,
    equalityOracle: equalityOracle, limit: startIndex,
    sign: -1, next: { baseCollection.index(before: $0) }, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

/// Test that the elements of `instances` satisfy the semantic requirements of
/// index for `RandomAccessCollection`, using `advanceOracle` and
/// 'distanceOracle' to generate expectations about the results of
/// `advanced(by:)` and `distance(to:)` from pairs of positions in `instances`
/// and `distances`.
///
/// - Precondition:
///   - all elements of `instances` are reachable from `startIndex`.
///   - `endIndex` is reachable from all elements of `instances`.
public func checkRandomAccessIndex<Instances, Distances, BaseCollection>(
  _ instances: Instances, distances: Distances,
  of baseCollection: BaseCollection,
  distanceOracle:
    (Instances.Index, Instances.Index) -> Distances.Element,
  advanceOracle:
    (Instances.Index, Distances.Index) -> Instances.Element,
  startIndex: Instances.Element,
  endIndex: Instances.Element,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  Distances : Collection,
  BaseCollection : RandomAccessCollection,
  Instances.Element == BaseCollection.Index,
  Distances.Element == Int {

  checkBidirectionalIndex(instances, of: baseCollection,
    equalityOracle: { distanceOracle($0, $1) == 0 },
    startIndex: startIndex, endIndex: endIndex, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  checkAdvancesAndDistances(
    instances, distances: distances,
    of: baseCollection,
    distanceOracle: distanceOracle,
    advanceOracle: advanceOracle, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

// Copies what checkStrideable is doing, but instead of calling
// advanced(by:) and distance(to:) on an Strideable's,
// calls corresponding methods on a base collection.
public func checkAdvancesAndDistances<Instances, Distances, BaseCollection>(
  _ instances: Instances, distances: Distances,
  of baseCollection: BaseCollection,
  distanceOracle:
    (Instances.Index, Instances.Index) -> Distances.Element,
  advanceOracle:
    (Instances.Index, Distances.Index) -> Instances.Element,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances : Collection,
  Distances : Collection,
  BaseCollection : Collection,
  Instances.Element == BaseCollection.Index,
  Distances.Element == Int {

  checkComparable(
    instances,
    oracle: {
      let d = distanceOracle($1, $0);
      return d < 0 ? .lt : d == 0 ? .eq : .gt
    },
    message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  for i in instances.indices {
    let x = instances[i]
    expectEqual(x, baseCollection.index(x, offsetBy: 0))

    for j in distances.indices {
      let y = distances[j]
      expectEqual(advanceOracle(i, j), baseCollection.index(x, offsetBy: y))
    }

    for j in instances.indices {
      let y = instances[j]
      expectEqual(distanceOracle(i, j), baseCollection.distance(from: x, to: y))
    }
  }
}

// Generate two overloads: one for Array (which will get
// picked up when the caller passes a literal), and another that
// accepts any appropriate Collection type.

// Top-level check for Collection instances. Alias for checkForwardCollection.
// Checks all slices: O(n^2).
public func checkCollection<Expected: Collection, C : Collection>(
  _ expected: Expected,
  _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  checkForwardCollection(expected, collection, message(),
    stackTrace: stackTrace, showFrame: showFrame, file: file, line: line,
    resiliencyChecks: resiliencyChecks,
    sameValue: sameValue)
}


// Calls checkForwardCollection with default `sameValue`.
public func checkForwardCollection<
  Expected: Collection, C : Collection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Expected.Element,
  Expected.Element : Equatable {

  checkForwardCollection(
    expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all Collection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkForwardCollection<
  Expected: Collection, C : Collection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  checkOneLevelOfForwardCollection(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfForwardCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithForwardIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkForwardCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `Collection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfForwardCollection<
  Expected: Collection, C : Collection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)


  let instances = _allIndices(into: collection,
    in: collection.startIndex..<partWay0)
  checkForwardIndex(instances, of: collection,
    equalityOracle: { $0 == $1 }, endIndex: partWay1, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkForwardCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithForwardIndex<
Expected: Collection, S : Collection
>(
  _ expected: Expected, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, S.Element) -> Bool
) {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfForwardCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}


// Calls checkBidirectionalCollection with default `sameValue`.
public func checkBidirectionalCollection<
  Expected: Collection, C : BidirectionalCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Expected.Element,
  Expected.Element : Equatable {

  checkBidirectionalCollection(
    expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all BidirectionalCollection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkBidirectionalCollection<
  Expected: Collection, C : BidirectionalCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  checkOneLevelOfBidirectionalCollection(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfBidirectionalCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithBidirectionalIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkBidirectionalCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `BidirectionalCollection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfBidirectionalCollection<
  Expected: Collection, C : BidirectionalCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  let pred = { collection.index(before: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)


  let instances = _allIndices(into: collection, in: partWay0..<partWay1)
  checkBidirectionalIndex(instances, of: collection,
    equalityOracle: { $0 == $1 },
    startIndex: collection.startIndex,
    endIndex: next5(partWay1), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


      for i in 1..<allIndices.count {
        let predecessor1 = pred(allIndices[i])
        var predecessor2 = allIndices[i]
        predecessor2 = pred(predecessor2)
        var predecessor3 = allIndices[i]
        predecessor3 = pred(predecessor3)
        for p in [ predecessor1, predecessor2, predecessor3 ] {
          expectEqual(allIndices[i - 1], p, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i - 1], collection[p], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }
      for i in 1..<allIndices.count {
        let index = succ(pred(allIndices[i]))
        expectEqual(allIndices[i], index, message(),
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        expectEqualTest(
          expectedArray[i], collection[index], message(),
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkBidirectionalCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithBidirectionalIndex<
Expected: Collection, S : BidirectionalCollection
>(
  _ expected: Expected, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, S.Element) -> Bool
) {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }
  let pred = { sliceable.index(before: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    if start != sliceable.endIndex {
      start = succ(start)
      start = pred(start)
      start = succ(start)
      start = pred(start)
    }
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      if end != sliceable.endIndex {
        end = succ(end)
        end = pred(end)
        end = succ(end)
        end = pred(end)
      }
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfBidirectionalCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}


// Calls checkRandomAccessCollection with default `sameValue`.
public func checkRandomAccessCollection<
  Expected: Collection, C : RandomAccessCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Expected.Element,
  Expected.Element : Equatable {

  checkRandomAccessCollection(
    expected, collection, message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all RandomAccessCollection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkRandomAccessCollection<
  Expected: Collection, C : RandomAccessCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  checkOneLevelOfRandomAccessCollection(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfRandomAccessCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithRandomAccessIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkRandomAccessCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `RandomAccessCollection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfRandomAccessCollection<
  Expected: Collection, C : RandomAccessCollection
>(
  _ expected: Expected, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, C.Element) -> Bool
) {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)

  typealias Distance = Int

  let count: Distance  = collection.count
  let offset0 = min(5, count)
  let offset1 = min(10, count)

  let distanceCandidates: [Distance] = [
    -11, -7, -5, -3, -2, -1, 0, 1, 2, 3, 5, 7, 11]

  let distances = distanceCandidates.filter { (x: Distance) -> Bool in
    x + offset0 >= 0 && x + offset1 <= count
  }

  func nextN(_ n: Distance, _ i: C.Index) -> C.Index {
    return collection.index(i, offsetBy: n)
  }

  let instances = _allIndices(into: collection, in: partWay0..<partWay1)

  checkRandomAccessIndex(
    instances,
    distances: distances,
    of: collection,
    distanceOracle: { (x:Int, y:Int) in y - x },
    advanceOracle: { x, y in nextN(distances[y], instances[x]) },
    startIndex: collection.startIndex,
    endIndex: next5(partWay1), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkRandomAccessCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithRandomAccessIndex<
Expected: Collection, S : RandomAccessCollection
>(
  _ expected: Expected, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, S.Element) -> Bool
) {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }
  let pred = { sliceable.index(before: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    if start != sliceable.endIndex {
      start = succ(start)
      start = pred(start)
      start = succ(start)
      start = pred(start)
    }
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      if end != sliceable.endIndex {
        end = succ(end)
        end = pred(end)
        end = succ(end)
        end = pred(end)
      }
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfRandomAccessCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}



// Top-level check for Collection instances. Alias for checkForwardCollection.
// Checks all slices: O(n^2).
public func checkCollection<Element, C : Collection>(
  _ expected: Array<Element>,
  _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where C.Element == Element {

  checkForwardCollection(expected, collection, message(),
    stackTrace: stackTrace, showFrame: showFrame, file: file, line: line,
    resiliencyChecks: resiliencyChecks,
    sameValue: sameValue)
}


// Calls checkForwardCollection with default `sameValue`.
public func checkForwardCollection<
  Element, C : Collection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Element,
  Element : Equatable {

  checkForwardCollection(
    expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all Collection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkForwardCollection<
  Element, C : Collection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  C.Element == Element {

  checkOneLevelOfForwardCollection(expected, collection, message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfForwardCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithForwardIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkForwardCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `Collection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfForwardCollection<
  Element, C : Collection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where C.Element == Element {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)


  let instances = _allIndices(into: collection,
    in: collection.startIndex..<partWay0)
  checkForwardIndex(instances, of: collection,
    equalityOracle: { $0 == $1 }, endIndex: partWay1, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkForwardCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithForwardIndex<
Element, S : Collection
>(
  _ expected: Array<Element>, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  S.Element == Element {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfForwardCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}


// Calls checkBidirectionalCollection with default `sameValue`.
public func checkBidirectionalCollection<
  Element, C : BidirectionalCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Element,
  Element : Equatable {

  checkBidirectionalCollection(
    expected, collection, message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all BidirectionalCollection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkBidirectionalCollection<
  Element, C : BidirectionalCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  C.Element == Element {

  checkOneLevelOfBidirectionalCollection(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfBidirectionalCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithBidirectionalIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkBidirectionalCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `BidirectionalCollection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfBidirectionalCollection<
  Element, C : BidirectionalCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where C.Element == Element {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  let pred = { collection.index(before: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)


  let instances = _allIndices(into: collection, in: partWay0..<partWay1)
  checkBidirectionalIndex(instances, of: collection,
    equalityOracle: { $0 == $1 },
    startIndex: collection.startIndex,
    endIndex: next5(partWay1), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


      for i in 1..<allIndices.count {
        let predecessor1 = pred(allIndices[i])
        var predecessor2 = allIndices[i]
        predecessor2 = pred(predecessor2)
        var predecessor3 = allIndices[i]
        predecessor3 = pred(predecessor3)
        for p in [ predecessor1, predecessor2, predecessor3 ] {
          expectEqual(allIndices[i - 1], p, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i - 1], collection[p], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }
      for i in 1..<allIndices.count {
        let index = succ(pred(allIndices[i]))
        expectEqual(allIndices[i], index, message(),
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        expectEqualTest(
          expectedArray[i], collection[index], message(),
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkBidirectionalCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithBidirectionalIndex<
Element, S : BidirectionalCollection
>(
  _ expected: Array<Element>, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  S.Element == Element {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }
  let pred = { sliceable.index(before: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    if start != sliceable.endIndex {
      start = succ(start)
      start = pred(start)
      start = succ(start)
      start = pred(start)
    }
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      if end != sliceable.endIndex {
        end = succ(end)
        end = pred(end)
        end = succ(end)
        end = pred(end)
      }
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfBidirectionalCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}


// Calls checkRandomAccessCollection with default `sameValue`.
public func checkRandomAccessCollection<
  Element, C : RandomAccessCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  C.Element == Element,
  Element : Equatable {

  checkRandomAccessCollection(
    expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks) { $0 == $1 }
}

// Top-Level check for all RandomAccessCollection semantics on a single
// instance. This constrains SubSequence associated types in order to check
// slice semantics.
// Checks all slices: O(n^2).
public func checkRandomAccessCollection<
  Element, C : RandomAccessCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  C.Element == Element {

  checkOneLevelOfRandomAccessCollection(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)

  // Avoid validation of all possible (n^2) slices on large collection.
  // Test cases should call checkOneLevelOfRandomAccessCollection instead.
  expectLT(expected.count, 30)

  _checkSliceableWithRandomAccessIndex(expected, collection, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks, sameValue: sameValue)
}

// Helper for checkRandomAccessCollection. Check that instance of `C`,
// `collection`, upholds the semantics of `RandomAccessCollection`,
// non-recursively. This does not check subsequences. It may be called for each
// subsequence without combinatorial explosion. Also, since recursive protocol
// constraints are not supported, our second level of checks cannot depend on the
// associated type properties of SubSequence.
//
// Checks all slices: O(n^2).
public func checkOneLevelOfRandomAccessCollection<
  Element, C : RandomAccessCollection
>(
  _ expected: Array<Element>, _ collection: C,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where C.Element == Element {

  // A `Collection` is a multi-pass `Sequence`.
  for _ in 0..<3 {
    checkSequence(
      expected, collection, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
      resiliencyChecks: resiliencyChecks, sameValue: sameValue)
  }

  //===------------------------------------------------------------------===//
  // Check Index semantics
  //===------------------------------------------------------------------===//

  let succ = { collection.index(after: $0) }
  // Advances up to 1 positions without passing endIndex.  Don't use
  // advanced(by: n) to do this because it's under test here.
  let next = { $0 == collection.endIndex ? $0 : succ($0) }

  // advances up to 5 positions without passing endIndex.  Picking a
  // small constant to avoid complexity explosion on large input
  // collections.
  let next5 = { next(next(next(next(next($0))))) }

  let partWay0 = next5(collection.startIndex)
  let partWay1 = next5(partWay0)

  typealias Distance = Int

  let count: Distance  = collection.count
  let offset0 = min(5, count)
  let offset1 = min(10, count)

  let distanceCandidates: [Distance] = [
    -11, -7, -5, -3, -2, -1, 0, 1, 2, 3, 5, 7, 11]

  let distances = distanceCandidates.filter { (x: Distance) -> Bool in
    x + offset0 >= 0 && x + offset1 <= count
  }

  func nextN(_ n: Distance, _ i: C.Index) -> C.Index {
    return collection.index(i, offsetBy: n)
  }

  let instances = _allIndices(into: collection, in: partWay0..<partWay1)

  checkRandomAccessIndex(
    instances,
    distances: distances,
    of: collection,
    distanceOracle: { (x:Int, y:Int) in y - x },
    advanceOracle: { x, y in nextN(distances[y], instances[x]) },
    startIndex: collection.startIndex,
    endIndex: next5(partWay1), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))


  let expectedArray = Array(expected)

  // Check `count`.
  expectEqual(Int64(expectedArray.count), Int64(collection.count), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  //===------------------------------------------------------------------===//
  // Check Iteration behavior.
  //===------------------------------------------------------------------===//

  for _ in 0..<3 {
    do {
      let startIndex = collection.startIndex
      let endIndex = collection.endIndex

      for _ in collection.indices {
        expectEqual(
          startIndex, collection.startIndex,
          "Iteration should not change startIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

        expectEqual(
          endIndex, collection.endIndex,
          "Iteration should not change endIndex",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }

    let allIndices = Array(collection.indices)

    if expectedArray.count >= 2 {
      for i in 0..<allIndices.count-1 {
        let successor1 = succ(allIndices[i])
        var successor2 = allIndices[i]
        successor2 = succ(successor2)
        var successor3 = allIndices[i]
        successor3 = succ(successor3)
        for s in [ successor1, successor2, successor3 ] {
          expectEqual(allIndices[i + 1], s, message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
          expectEqualTest(
            expectedArray[i + 1], collection[s], message(),
            stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
        }
      }


    } // end of `if expectedArray.count >= 2`

    do {
      var allIndices2: [C.Index] = []
      for i in collection.indices {
        allIndices2.append(i)
      }

      expectEqualSequence(
        allIndices, allIndices2, "iteration should not invalidate indices",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqualSequence(
        expectedArray, allIndices.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
      expectEqualSequence(
        expectedArray, allIndices2.map { collection[$0] },
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)
    }
  } // end of `for _ in 0..<3`

  // FIXME: more checks for bidirectional and random access collections.
}

// Helper for checkRandomAccessCollection to check Slices.
//
// Checks all slices: O(n^2).
internal func _checkSliceableWithRandomAccessIndex<
Element, S : RandomAccessCollection
>(
  _ expected: Array<Element>, _ sliceable: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where
  S.Element == Element {

  let expectedArray = Array(expected)

  let succ = { sliceable.index(after: $0) }
  let pred = { sliceable.index(before: $0) }

  var start = sliceable.startIndex
  for startNumericIndex in 0...expectedArray.count {
    if start != sliceable.endIndex {
      start = succ(start)
      start = pred(start)
      start = succ(start)
      start = pred(start)
    }
    var end = start
    for endNumericIndex in startNumericIndex...expectedArray.count {
      if end != sliceable.endIndex {
        end = succ(end)
        end = pred(end)
        end = succ(end)
        end = pred(end)
      }
      let expectedSlice = expectedArray[startNumericIndex..<endNumericIndex]
      let slice = sliceable[start..<end]
      // For every possible slice, verify that the slice's bounds are identical
      // to the indices used to form the slice.
      expectEqual(start, slice.startIndex)
      expectEqual(end, slice.endIndex)

      checkOneLevelOfRandomAccessCollection(expectedSlice, slice, message(),
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        resiliencyChecks: resiliencyChecks,
        sameValue: sameValue)

      if end != sliceable.endIndex {
        end = succ(end)
      }
    }
    if start != sliceable.endIndex {
      start = succ(start)
    }
  }
}



// Check RangeReplaceableCollection using a factory.
//
// Note: This does not invoke other collection tests.
public func checkRangeReplaceable<C, N>(
  _ makeCollection: @escaping () -> C,
  _ makeNewValues: (Int) -> N
) where
  C : RangeReplaceableCollection,
  N : Collection,
  C.Element : Equatable,
  C.Element == N.Element {

  typealias A = C

  // First make an independent copy of the array that we can use for
  // comparison later.
  let source = Array<A.Element>(makeCollection())

  for (ix, i) in source.indices.enumerated() {
    for (jx_, _) in (i..<source.endIndex).enumerated() {
      let jx = jx_ + ix

      let oldCount = jx - ix
      for newCount in 0..<(2 * oldCount) {
        let newValues = makeNewValues(newCount)

        func reportFailure(_ a: inout A, _ message: String) {
          print("\(message) when replacing indices \(ix)...\(jx)")
          print("  in \(Array(source)) with \(Array(newValues))")
          print("  yielding \(Array(a))")
          print("====================================")
          expectTrue(false)
        }

        var a = makeCollection()

        a.replaceSubrange(nthIndex(a, ix)..<nthIndex(a, jx), with: newValues)
        let growth = newCount - oldCount

        let expectedCount = source.count + growth
        let actualCount = a.count
        if actualCount != expectedCount {
          reportFailure(
            &a, "\(actualCount) != expected count \(expectedCount)")
        }

        for (kx, k) in a.indices.enumerated() {
          let expectedValue = kx < ix ? nth(source, kx)
          : kx < jx + growth ? nth(newValues, kx - ix)
          : nth(source, kx - growth)

          if a[k] != expectedValue {
            reportFailure(
              &a,
              // FIXME: why do we need to break this string into two parts?
              "a[\(kx)] = "
              + "\(a[k]) != expected value \(expectedValue)")
          }
        }
      }
    }
  }
}

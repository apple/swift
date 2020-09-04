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

extension TestSuite {
  /// Adds a set of tests for `RangeReplaceableCollection` that is also a
  /// slice type.
  public func addRangeReplaceableSliceTests<
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
    C.SubSequence == C,
    CollectionWithEquatableElement.SubSequence == CollectionWithEquatableElement,
    CollectionWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    // Don't run the same tests twice.
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
      collectionIsBidirectional: collectionIsBidirectional
    )

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    testNamePrefix += String(describing: C.self)

    //===------------------------------------------------------------------===//
    // removeFirst()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeFirst()/semantics") {
      for test in removeFirstTests.filter({ $0.numberToRemove == 1 }) {
        var c = makeWrappedCollection(test.collection.map(OpaqueValue.init))
        let survivingIndices = _allIndices(
          into: c,
          in: c.index(after: c.startIndex)..<c.endIndex)
        let removedElement = c.removeFirst()
        expectEqual(test.collection.first, extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          c.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the tail of the collection",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(c[$0]).value },
          "removeFirst() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
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
        let survivingIndices = _allIndices(
          into: c,
          in: c.index(c.startIndex, offsetBy: test.numberToRemove) ..<
            c.endIndex
        )
        c.removeFirst(test.numberToRemove)
        expectEqualSequence(
          test.expectedCollection,
          c.map { extractValue($0).value },
          "removeFirst() shouldn't mutate the tail of the collection",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(c[$0]).value },
          "removeFirst() shouldn't invalidate indices",
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
      var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      expectCrashLater()
      c.removeFirst(-1) // Should trap.
    }

    self.test("\(testNamePrefix).removeFirst(n: Int)/removeTooMany/semantics") {
      var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      expectCrashLater()
      c.removeFirst(3) // Should trap.
    }

    //===----------------------------------------------------------------------===//

  } // addRangeReplaceableSliceTests

  public func addRangeReplaceableBidirectionalSliceTests<
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
    C.SubSequence == C,
    CollectionWithEquatableElement.SubSequence == CollectionWithEquatableElement,
    CollectionWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    // Don't run the same tests twice.
    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addRangeReplaceableSliceTests(
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

    func makeWrappedCollection(_ elements: [OpaqueValue<Int>]) -> C {
      return makeCollection(elements.map(wrapValue))
    }

    testNamePrefix += String(describing: C.self)

    //===------------------------------------------------------------------===//
    // removeLast()
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeLast()/semantics") {
      for test in removeLastTests.filter({ $0.numberToRemove == 1 }) {
        var c = makeWrappedCollection(test.collection)
        let survivingIndices = _allIndices(
          into: c,
          in: c.startIndex..<c.index(before: c.endIndex))
        let removedElement = c.removeLast()
        expectEqual(
          test.collection.last!.value,
          extractValue(removedElement).value)
        expectEqualSequence(
          test.expectedCollection,
          c.map { extractValue($0).value },
          "removeLast() shouldn't mutate the head of the collection",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(c[$0]).value },
          "removeLast() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
      }
    }

    self.test("\(testNamePrefix).removeLast()/empty/semantics") {
      var c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      expectCrashLater()
      _ = c.removeLast() // Should trap.
    }

    //===------------------------------------------------------------------===//
    // removeLast(n: Int)
    //===------------------------------------------------------------------===//

    self.test("\(testNamePrefix).removeLast(n: Int)/semantics") {
      for test in removeLastTests {
        var c = makeWrappedCollection(test.collection)
        let survivingIndices = _allIndices(
          into: c,
          in: c.startIndex ..<
            c.index(c.endIndex, offsetBy: -test.numberToRemove)
        )
        c.removeLast(test.numberToRemove)
        expectEqualSequence(
          test.expectedCollection,
          c.map { extractValue($0).value },
          "removeLast() shouldn't mutate the head of the collection",
          stackTrace: SourceLocStack().with(test.loc)
        )
        expectEqualSequence(
          test.expectedCollection,
          survivingIndices.map { extractValue(c[$0]).value },
          "removeLast() shouldn't invalidate indices",
          stackTrace: SourceLocStack().with(test.loc)
        )
      }
    }

    self.test("\(testNamePrefix).removeLast(n: Int)/empty/semantics") {
      var c = makeWrappedCollection(Array<OpaqueValue<Int>>())
      expectCrashLater()
      c.removeLast(1) // Should trap.
    }

    self.test("\(testNamePrefix).removeLast(n: Int)/removeNegative/semantics") {
      var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      expectCrashLater()
      c.removeLast(-1) // Should trap.
    }

    self.test("\(testNamePrefix).removeLast(n: Int)/removeTooMany/semantics") {
      var c = makeWrappedCollection([1010, 2020].map(OpaqueValue.init))
      expectCrashLater()
      c.removeLast(3) // Should trap.
    }

    //===----------------------------------------------------------------------===//

  } // addRangeReplaceableBidirectionalSliceTests

  public func addRangeReplaceableRandomAccessSliceTests<
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
    C.SubSequence == C,
    CollectionWithEquatableElement.SubSequence == CollectionWithEquatableElement,
    CollectionWithEquatableElement.Element : Equatable {

    var testNamePrefix = testNamePrefix

    // Don't run the same tests twice.
    if !checksAdded.insert(
        "\(testNamePrefix).\(C.self).\(#function)"
      ).inserted {
      return
    }

    addRangeReplaceableBidirectionalSliceTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset)

    addRangeReplaceableRandomAccessCollectionTests(
      testNamePrefix,
      makeCollection: makeCollection,
      wrapValue: wrapValue,
      extractValue: extractValue,
      makeCollectionOfEquatable: makeCollectionOfEquatable,
      wrapValueIntoEquatable: wrapValueIntoEquatable,
      extractValueFromEquatable: extractValueFromEquatable,
      resiliencyChecks: resiliencyChecks,
      outOfBoundsIndexOffset: outOfBoundsIndexOffset)

    testNamePrefix += String(describing: C.self)

    // No tests yet.
  } // addRangeReplaceableRandomAccessSliceTests
}

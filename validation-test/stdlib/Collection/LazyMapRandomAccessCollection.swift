// -*- swift -*-

//===----------------------------------------------------------------------===//
// Automatically Generated From validation-test/stdlib/Collection/Inputs/LazyMapTemplate.swift.gyb
// Do Not Edit Directly!
//===----------------------------------------------------------------------===//

// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// With a non-optimized stdlib the test takes very long.
// REQUIRES: optimized_stdlib

import StdlibUnittest
import StdlibCollectionUnittest

var CollectionTests = TestSuite("Collection")

// Test collections using value types as elements.
CollectionTests.addRandomAccessCollectionTests(
  makeCollection: { (elements: [OpaqueValue<Int>]) -> LazyMapCollection<MinimalRandomAccessCollection<OpaqueValue<Int>>, OpaqueValue<Int>> in
    MinimalRandomAccessCollection(elements: elements).lazy.map(identity)
  },
  wrapValue: identity,
  extractValue: identity,
  makeCollectionOfEquatable: { (elements: [MinimalEquatableValue]) -> LazyMapCollection<MinimalRandomAccessCollection<MinimalEquatableValue>, MinimalEquatableValue> in
    MinimalRandomAccessCollection(elements: elements).lazy.map(identityEq)
  },
  wrapValueIntoEquatable: identityEq,
  extractValueFromEquatable: identityEq
)

// Test collections using reference types as elements.
CollectionTests.addRandomAccessCollectionTests(
  makeCollection: { (elements: [LifetimeTracked]) -> LazyMapCollection<MinimalRandomAccessCollection<LifetimeTracked>, LifetimeTracked> in
    MinimalRandomAccessCollection(elements: elements).lazy.map { $0 }
  },
  wrapValue: { (element: OpaqueValue<Int>) in
    LifetimeTracked(element.value, identity: element.identity)
  },
  extractValue: { (element: LifetimeTracked) in
    OpaqueValue(element.value, identity: element.identity)
  },
  makeCollectionOfEquatable: { (elements: [LifetimeTracked]) -> LazyMapCollection<MinimalRandomAccessCollection<LifetimeTracked>, LifetimeTracked> in
    MinimalRandomAccessCollection(elements: elements).lazy.map { $0 }
  },
  wrapValueIntoEquatable: { (element: MinimalEquatableValue) in
    LifetimeTracked(element.value, identity: element.identity)
  },
  extractValueFromEquatable: { (element: LifetimeTracked) in
    MinimalEquatableValue(element.value, identity: element.identity)
  }
)

// Test sequence instances and iterators.
CollectionTests.test("LazyMapCollection instances (RandomAccessCollection)") {
  do {
    let expected = ["convent", "conform", "constrict", "condone"]
    let base = ["vent", "form", "strict", "done"]
    checkRandomAccessCollection(expected,
      MinimalRandomAccessCollection(elements: base).lazy.map { "con" + $0 },
      sameValue: { $0 == $1 })
  }
  do {
    let expected = [1, 4, 9, 16, 25, 36, 49, 64]
    let base = [1, 2, 3, 4, 5, 6, 7, 8]
    checkRandomAccessCollection(
      expected,
      MinimalRandomAccessCollection(elements: base).lazy.map { $0 * $0 },
      sameValue: { $0 == $1 })
  }
}

runAllTests()


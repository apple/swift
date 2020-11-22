//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -swift-version 4 %s -o %t/Filter4
// RUN: %target-codesign %t/Filter4
// RUN: %target-run %t/Filter4
// REQUIRES: executable_test

// Tests for typealiases obsoleted in Swift 5

import StdlibUnittest

let suite = TestSuite("Filter4")
defer { runAllTests() }

suite.test("obsolete names") {
  expectEqualType(
    LazyFilterIterator<[Int]>.self,
    LazyFilterSequence<[Int]>.Iterator.self)
  expectEqualType(
    LazyMapBidirectionalCollection<[Int], Int>.self,
    LazyMapCollection<[Int], Int>.self)
  expectEqualType(
    LazyMapRandomAccessCollection<[Int], Int>.self,
    LazyMapCollection<[Int], Int>.self)
  expectEqualType(
    LazyMapIterator<[Int], Int>.self,
    LazyMapSequence<[Int], Int>.Iterator.self)
  expectEqualType(
    UnsafeBufferPointerIterator<Int>.self,
    UnsafeBufferPointer<Int>.Iterator.self)
  expectEqualType(
    IteratorOverOne<Int>.self,
    CollectionOfOne<Int>.Iterator.self)
  expectEqualType(
    EmptyIterator<Int>.self,
    EmptyCollection<Int>.Iterator.self)
}


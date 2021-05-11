//===--- RangeTraps.swift -------------------------------------------------===//
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
// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out_Debug -Onone
// RUN: %target-build-swift %s -o %t/a.out_Release -O
//
// RUN: %target-codesign %t/a.out_Debug
// RUN: %target-codesign %t/a.out_Release
// RUN: %target-run %t/a.out_Debug
// RUN: %target-run %t/a.out_Release
// REQUIRES: executable_test
// UNSUPPORTED: OS=wasi


import StdlibUnittest

let testSuiteSuffix = _isDebugAssertConfiguration() ? "_debug" : "_release"

var RangeTraps = TestSuite("RangeTraps" + testSuiteSuffix)

RangeTraps.test("HalfOpen")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var range = 1..<1
  expectType(CountableRange<Int>.self, &range)
  
  expectCrashLater()
  _ = 1..<0
}

RangeTraps.test("Closed")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var range = 1...1
  expectType(CountableClosedRange<Int>.self, &range)

  expectCrashLater()
  _ = 1...0
}

RangeTraps.test("OutOfRange")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  _ = 0..<Int.max // This is a CountableRange

  // This works for Ranges now!
  expectTrue((0...Int.max).contains(Int.max))
}

RangeTraps.test("CountablePartialRangeFrom")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
    
    let range = (Int.max - 1)...
    var it = range.makeIterator()
    _ = it.next()
    expectCrashLater()
    _ = it.next()
}

RangeTraps.test("nanLowerBound")
  .code {
  expectCrashLater()
  _ = Double.nan ... 0
}

RangeTraps.test("nanUpperBound")
  .code {
  expectCrashLater()
  _ = 0 ... Double.nan
}

RangeTraps.test("nanLowerBoundPartial")
  .code {
  expectCrashLater()
  _ = Double.nan ..< 0
}

RangeTraps.test("nanUpperBoundPartial")
  .code {
  expectCrashLater()
  _ = 0 ..< Double.nan
}

RangeTraps.test("fromNaN")
  .code {
  expectCrashLater()
  _ = Double.nan...
}

RangeTraps.test("toNaN")
  .code {
  expectCrashLater()
  _ = ..<Double.nan
}

RangeTraps.test("throughNaN")
  .code {
  expectCrashLater()
  _ = ...Double.nan
}

if #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) {
  // Debug check was introduced in https://github.com/apple/swift/pull/34961
  RangeTraps.test("UncheckedHalfOpen")
  .xfail(.custom(
      { !_isDebugAssertConfiguration() },
      reason: "assertions are disabled in Release and Unchecked mode"))
  .code {
    expectCrashLater()
    var range = Range(uncheckedBounds: (lower: 1, upper: 0))
  }

  RangeTraps.test("UncheckedClosed")
  .xfail(.custom(
      { !_isDebugAssertConfiguration() },
      reason: "assertions are disabled in Release and Unchecked mode"))
  .code {
    expectCrashLater()
    var range = ClosedRange(uncheckedBounds: (lower: 1, upper: 0))
  }
}

runAllTests()


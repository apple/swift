//===--- IntervalTraps.swift ----------------------------------------------===//
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

var IntervalTraps = TestSuite("IntervalTraps" + testSuiteSuffix)

IntervalTraps.test("HalfOpen")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var interval = 1.0..<1.0
  // FIXME: the plan is for floating point numbers to no longer be
  // strideable; then this will drop the "OfStrideable"
  expectType(Range<Double>.self, &interval)
  expectCrashLater()
  _ = 1.0..<0.0
}

IntervalTraps.test("Closed")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var interval = 1.0...1.0
  // FIXME: the plan is for floating point numbers to no longer be
  // strideable; then this will drop the "OfStrideable"
  expectType(ClosedRange<Double>.self, &interval)

  expectCrashLater()
  _ = 1.0...0.0
}

runAllTests()


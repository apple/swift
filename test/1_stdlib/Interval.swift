//===--- Interval.swift ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %target-run-simple-swift
// REQUIRES: executable_test
//

import StdlibUnittest


// Check that the generic parameter is called 'Bound'.
protocol TestProtocol1 {}

extension Range where Bound : TestProtocol1 {
  var _elementIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

extension ClosedRange where Bound : TestProtocol1 {
  var _elementIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

var tests = TestSuite("Range")

tests.test("Ambiguity") {
  // Ensure type deduction still works as expected; these will fail to
  // compile if it's broken
  var pieToPie = -3.1415927..<3.1415927
  
  expectType(Range<Double>.self, &pieToPie)

  var pieThruPie = -3.1415927...3.1415927
  expectType(ClosedRange<Double>.self, &pieThruPie)

  var zeroToOne = 0..<1
  expectType(CountableRange<Int>.self, &zeroToOne)

  var zeroThruOne = 0...1
  expectType(CountableClosedRange<Int>.self, &zeroThruOne)
}

tests.test("PatternMatching") {

  let pie = 3.1415927

  let expectations : [(Double, halfOpen: Bool, closed: Bool)] = [
    (-2 * pie, false, false),
    (-pie, true, true),
    (0, true, true),
    (pie, false, true),
    (2 * pie, false, false)
  ]

  for (x, halfOpenExpected, closedExpected) in expectations {
    var halfOpen: Bool
    switch x {
    case -3.1415927..<3.1415927:
      halfOpen = true
    default:
      halfOpen = false
    }

    var closed: Bool
    switch x {
    case -3.1415927...3.1415927:
      closed = true
    default:
      closed = false
    }
    
    expectEqual(halfOpenExpected, halfOpen)
    expectEqual(closedExpected, closed)
  }
}

tests.test("Overlaps") {
  
  func expectOverlaps<
    I0: RangeProtocol, I1: RangeProtocol where I0.Bound == I1.Bound
  >(_ expectation: Bool, _ lhs: I0, _ rhs: I1) {
    if expectation {
      expectTrue(lhs.overlaps(rhs))
      expectTrue(rhs.overlaps(lhs))
    }
    else {
      expectFalse(lhs.overlaps(rhs))
      expectFalse(rhs.overlaps(lhs))
    }
  }
  
  // 0-4, 5-10
  expectOverlaps(false, 0..<4, 5..<10)
  expectOverlaps(false, 0..<4, 5...10)
  expectOverlaps(false, 0...4, 5..<10)
  expectOverlaps(false, 0...4, 5...10)

  // 0-5, 5-10
  expectOverlaps(false, 0..<5, 5..<10)
  expectOverlaps(false, 0..<5, 5...10)
  expectOverlaps(true, 0...5, 5..<10)
  expectOverlaps(true, 0...5, 5...10)

  // 0-6, 5-10
  expectOverlaps(true, 0..<6, 5..<10)
  expectOverlaps(true, 0..<6, 5...10)
  expectOverlaps(true, 0...6, 5..<10)
  expectOverlaps(true, 0...6, 5...10)

  // 0-20, 5-10
  expectOverlaps(true, 0..<20, 5..<10)
  expectOverlaps(true, 0..<20, 5...10)
  expectOverlaps(true, 0...20, 5..<10)
  expectOverlaps(true, 0...20, 5...10)

  // 0-0, 0-5
  expectOverlaps(false, 0..<0, 0..<5)
  expectOverlaps(false, 0..<0, 0...5)
  
}

tests.test("Emptiness") {
  expectTrue((0.0..<0.0).isEmpty)
  expectFalse((0.0...0.0).isEmpty)
  expectFalse((0.0..<0.1).isEmpty)
  expectFalse((0.0..<0.1).isEmpty)
}

tests.test("start/end") {
  expectEqual(0.0, (0.0..<0.1).lowerBound)
  expectEqual(0.0, (0.0...0.1).lowerBound)
  expectEqual(0.1, (0.0..<0.1).upperBound)
  expectEqual(0.1, (0.0...0.1).upperBound)
}

// Something to test with that distinguishes debugDescription from description
struct X<T : Comparable>
  : Comparable, CustomStringConvertible, CustomDebugStringConvertible {
  init(_ a: T) {
    self.a = a
  }

  var description: String {
    return String(a)
  }

  var debugDescription: String {
    return "X(\(String(reflecting: a)))"
  }
  
  var a: T
}

func < <T : Comparable>(lhs: X<T>, rhs: X<T>) -> Bool {
  return lhs.a < rhs.a
}

func == <T : Comparable>(lhs: X<T>, rhs: X<T>) -> Bool {
  return lhs.a == rhs.a
}

tests.test("CustomStringConvertible/CustomDebugStringConvertible") {
  expectEqual("0.0..<0.1", String(X(0.0)..<X(0.1)))
  expectEqual("0.0...0.1", String(X(0.0)...X(0.1)))
  
  expectEqual(
    "Range(X(0.0)..<X(0.5))",
    String(reflecting: Range(X(0.0)..<X(0.5))))
  expectEqual(
    "ClosedRange(X(0.0)...X(0.5))",
    String(reflecting: ClosedRange(X(0.0)...X(0.5))))
}

tests.test("rdar12016900") {
  do {
    let wc = 0
    expectFalse((0x00D800 ..< 0x00E000).contains(wc))
  }
  do {
    let wc = 0x00D800
    expectTrue((0x00D800 ..< 0x00E000).contains(wc))
  }
}

tests.test("clamp") {
  expectEqual(
    (0..<3).clamped(to: 5..<10), 5..<5)
  expectEqual(
    (0..<9).clamped(to: 5..<10), 5..<9)
  expectEqual(
    (0..<13).clamped(to: 5..<10), 5..<10)
  expectEqual(
    (7..<9).clamped(to: 5..<10), 7..<9)
  expectEqual(
    (7..<13).clamped(to: 5..<10), 7..<10)
  expectEqual(
    (13..<15).clamped(to: 5..<10), 10..<10)

  expectEqual(
    (0...3).clamped(to: 5...10), 5...5)
  expectEqual(
    (0...9).clamped(to: 5...10), 5...9)
  expectEqual(
    (0...13).clamped(to: 5...10), 5...10)
  expectEqual(
    (7...9).clamped(to: 5...10), 7...9)
  expectEqual(
    (7...13).clamped(to: 5...10), 7...10)
  expectEqual(
    (13...15).clamped(to: 5...10), 10...10)
}

runAllTests()


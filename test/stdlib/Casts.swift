// Casts.swift - Tests for conversion between types.
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
// -----------------------------------------------------------------------------
///
/// Contains tests for conversions between types which shouldn't trap.
///
// -----------------------------------------------------------------------------
// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
#if _runtime(_ObjC)
import Foundation
#endif

let CastsTests = TestSuite("Casts")

// Test for SR-426: missing release for some types after failed conversion
CastsTests.test("No leak for failed tuple casts") {
    let t: Any = (1, LifetimeTracked(0))
    expectFalse(t is Any.Type)
}

protocol P {}
class ErrClass : Error { }

CastsTests.test("No overrelease of existential boxes in failed casts") {
    // Test for crash from SR-392
    // We fail casts of an existential box repeatedly
    // to ensure it does not get over-released.
    func bar<T>(_ t: T) {
        for _ in 0..<10 {
            if case let a as P = t {
                _ = a
            }
        }
    }

    let err: Error = ErrClass()
    bar(err)
}

extension Int : P {}

// Test for SR-7664: Inconsistent optional casting behaviour with generics
// Runtime failed to unwrap multiple levels of Optional when casting.
CastsTests.test("Multi-level optionals can be casted") {
  func testSuccess<From, To>(_ x: From, from: From.Type, to: To.Type) {
    expectNotNil(x as? To)
  }
  func testFailure<From, To>(_ x: From, from: From.Type, to: To.Type) {
    expectNil(x as? To)
  }
  testSuccess(42, from: Int?.self, to: Int.self)
  testSuccess(42, from: Int??.self, to: Int.self)
  testSuccess(42, from: Int???.self, to: Int.self)
  testSuccess(42, from: Int???.self, to: Int?.self)
  testSuccess(42, from: Int???.self, to: Int??.self)
  testSuccess(42, from: Int???.self, to: Int???.self)
  testFailure(42, from: Int?.self, to: String.self)
  testFailure(42, from: Int??.self, to: String.self)
  testFailure(42, from: Int???.self, to: String.self)
}

// Test for SR-9837: Optional<T>.none not casting to Optional<U>.none in generic context
CastsTests.test("Optional<T>.none can be casted to Optional<U>.none in generic context") {
  func test<T>(_ type: T.Type) -> T? {
    return Any?.none as? T
  }

  expectEqual(type(of: test(Bool.self)), Bool?.self)
  expectEqual(type(of: test(Bool?.self)), Bool??.self)
}

#if _runtime(_ObjC)
extension CFBitVector : P {
  static func makeImmutable(from values: Array<UInt8>) -> CFBitVector {
    return CFBitVectorCreate(/*allocator:*/ nil, values, values.count * 8)
  }
}

extension CFMutableBitVector {
  static func makeMutable(from values: Array<UInt8>) -> CFMutableBitVector {
    return CFBitVectorCreateMutableCopy(
      /*allocator:*/ nil,
      /*capacity:*/ 0,
      CFBitVector.makeImmutable(from: values))
  }
}

func isP<T>(_ t: T) -> Bool {
  return t is P
}

CastsTests.test("Dynamic casts of CF types to protocol existentials")
  .skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This test behaves unpredictably in optimized mode."))
  .code {
  expectTrue(isP(10 as Int))

  // FIXME: SR-2289: dynamic casting of CF types to protocol existentials
  // should work, but there is a bug in the runtime that prevents them from
  // working.
  expectFailure {
    expectTrue(isP(CFBitVector.makeImmutable(from: [10, 20])))
  }
  expectFailure {
    expectTrue(isP(CFMutableBitVector.makeMutable(from: [10, 20])))
  }
}
#endif

protocol PP1: class { }
CastsTests.test("Any<Optional<P>> => P (P is class constrained)") {
  class C : PP1 {
    let tracker = LifetimeTracked(0)
  }
  let a = C()
  let b: PP1 = a
  let c: Optional<PP1> = b
  let d = c as Any
  let e = d as? PP1
  expectNotNil(e)
  expectNotNil(d as? C)
  // FIXME: Compiler rejects these: "Cannot downcast to a more optional type"
  // But, in this case, the Any holds an Optional<PP1>, so the
  // cast should be accepted.
  //expectNotNil(d as? Optional<PP1>)
  //expectNotNil(d as? Optional<C>)
}

protocol PP2 { }
CastsTests.test("Any<Optional<P>> => P (P not class-constrained)") {
  class C : PP2 {
    let tracker = LifetimeTracked(0)
  }
  let a = C()
  let b: PP2 = a
  let c: Optional<PP2> = b
  let d = c as Any
  let e = d as? PP2
  expectNotNil(e)
  expectNotNil(d as? C)
  // FIXME: Compiler rejects these
  //expectNotNil(d as? Optional<PP2>)
  //expectNotNil(d as? Optional<C>)
}

runAllTests()

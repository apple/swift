//===--- subclass_existentials_objc.swift ---------------------------------===//
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
//
// RUN: %empty-directory(%t)
//
// RUN: %target-build-swift %s -g -Onone -swift-version 5 -o %t/a.swift5.Onone.out
// RUN: %target-codesign %t/a.swift5.Onone.out
// RUN: %target-run %t/a.swift5.Onone.out
//
// RUN: %target-build-swift %s -g -O -swift-version 5 -o %t/a.swift5.O.out
// RUN: %target-codesign %t/a.swift5.O.out
// RUN: %target-run %t/a.swift5.O.out
//
// RUN: %target-build-swift %s -g -O -swift-version 4 -o %t/a.swift4.O.out
// RUN: %target-codesign %t/a.swift4.O.out
// RUN: %target-run %t/a.swift4.O.out
//
// RUN: %target-build-swift %s -g -Onone -swift-version 4 -o %t/a.swift4.Onone.out
// RUN: %target-codesign %t/a.swift4.Onone.out
// RUN: %target-run %t/a.swift4.Onone.out
//
// REQUIRES: executable_test
// REQUIRES: objc_interop
//

import StdlibUnittest
import Foundation

func cast<T, U>(_ t: T, to: U.Type) -> U? {
  return t as? U
}

protocol CP : class {}
@objc protocol OP {}

class C {}

class D : C, OP {}

var SubclassExistentialsTestSuite = TestSuite("SubclassExistentials")

SubclassExistentialsTestSuite.test("test1(): Existential basics") {
  // Making the test body a named function allows you to set a breakpoint more easily:
  // (lldb) break set -n test1
  test1()
}

func test1() {
  // An OP instance can be cast to OP or AnyObject as expected
  let op : OP = D()
  expectTrue(op is OP)
  expectNotNil(cast(op, to: OP.self))
  expectTrue(op is D)
  expectNotNil(cast(op, to: D.self))
  expectTrue(op is AnyObject)
  expectNotNil(cast(op, to: AnyObject.self))
  expectTrue(D.self is AnyObject.Type)
  expectNotNil(cast(D.self, to: AnyObject.Type.self))
}

SubclassExistentialsTestSuite.test("test2(): ObjC Metatype self-conformance") {
  test2()
}

func test2() {
  // Obj-C protocols self-conform if they have no static requirements
  expectTrue(OP.self is OP.Type) // Self-conformance

  // The following should be the same as above, but with a runtime cast instead of compile-time
  // It fails because the runtime cannot find the necessary protocol conformance
  // Bug: Compiler isn't emitting these conformances?
  // In optimized build, this asserts because the casting oracle knows that it succeeds,
  // so the optimizer converts it into an unconditional cast.
  //expectFailure { expectNotNil(cast(OP.self, to: OP.Type.self)) }

  // The following cast should succeed, because:
  // 1. OP.self is OP.Type due to self-conformance above
  // 2. OP is a sub-type of AnyObject (because OP is implicitly class-constrained)
  // 3. OP.Type is a sub-type of AnyObject.Type
  // 4. So OP.self is AnyObject.Type
  expectFailure { expectTrue(OP.self is AnyObject.Type) }
  expectFailure { expectNotNil(cast(OP.self, to: AnyObject.Type.self)) } // Ditto
  // Casting to AnyObject doesn't change the representation, hence this equality
  expectEqual(OP.self, OP.self as AnyObject.Type)
}

SubclassExistentialsTestSuite.test("test3(): Non-ObjC metatypes do not self-conform") {
  test3()
}

func test3() {
  // Non-ObjC protocols do not generally self-conform
  expectFalse(CP.self is CP.Type) // No self-conformance
  expectNil(cast(CP.self, to: CP.Type.self)) // Ditto
  expectFalse(CP.self is AnyObject.Type)
  expectNil(cast(CP.self, to: AnyObject.Type.self))
}

SubclassExistentialsTestSuite.test("test4(): (C & OP) acts like an Obj-C protocol") {
  test4()
}

func test4() {
  // (C & OP) acts like an Obj-C protocol (because OP is)
  expectFailure { expectTrue((C & OP).self is (C & OP).Type) } // Self-conforms
  expectFailure { expectNotNil(cast((C & OP).self, to: (C & OP).Type.self)) } // Ditto

  // Casting oracle knows that the next cast must always succeed
  // (This is why there's a "warning: 'is' test is always true")
  expectTrue((C & OP).self is OP.Type) // (C & OP) is sub-protocol of OP

  // The following cast is the same as the one above, except in a form that
  // forces the compiler to use the runtime machinery.  Because the oracle
  // knows it will succeed, the optimizer legitimately converts it into an
  // unconditional runtime cast.  Which in turn leads to an assert when it fails at runtime.
  // Bug: I suspect this is the same bug as above -- the compiler isn't emitting
  // the protocol conformance information that the runtime requires to verify this cast.
  //expectFailure { expectNotNil(cast((C & OP).self, to: OP.Type.self)) } // Ditto

  expectFailure { expectTrue((C & OP).self is AnyObject.Type) } // (C & OP) is a sub-protocol of AnyObject
  expectFailure { expectNotNil(cast((C & OP).self, to: AnyObject.Type.self)) } // Ditto
}

SubclassExistentialsTestSuite.test("test5(): (C & OP) is a sub-protocol of OP") {
  test5()
}

func test5() {
  expectTrue((C & OP).self is (C & OP).Protocol) // By definition
  expectNotNil(cast((C & OP).self, to: (C & OP).Protocol.self)) // Ditto
  expectFalse(D.self is (C & OP).Protocol) // (C & OP).self is only instance of (C&OP).Protocol
  expectNil(cast(D.self, to: (C & OP).Protocol.self)) // Ditto
  expectTrue(D.self is (C & OP).Type) // True, since D conforms to (C & OP)
  expectFalse(OP.self is (C & OP).Protocol) // (C & OP).self is only instance of (C&OP).Protocol
  expectNil(cast(OP.self, to: (C & OP).Protocol.self)) // Ditto
  expectFalse(OP.self is (C & OP).Type) // True

  expectFalse((C & OP).self is OP.Protocol) // OP.self is only instance of OP.Protocol
  expectNil(cast((C & OP).self, to: OP.Protocol.self)) // ditto
}

runAllTests()

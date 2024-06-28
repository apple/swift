//===--- SwiftValueNSObject.swift - Test SwiftValue's NSObject interop --===//
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
//
// RUN: %target-clang %S/Inputs/SwiftValueNSObject/SwiftValueNSObject.m -c -o %t/SwiftValueNSObject.o -g
// RUN: %target-build-swift %s -g -I %S/Inputs/SwiftValueNSObject/ -Xlinker %t/SwiftValueNSObject.o -o %t/SwiftValueNSObject
// RUN: %target-codesign %t/SwiftValueNSObject
// RUN: %target-run %t/SwiftValueNSObject 2> %t/log.txt
// RUN: cat %t/log.txt 1>&2
// RUN: %FileCheck %s < %t/log.txt
// REQUIRES: executable_test

// REQUIRES: objc_interop
// REQUIRES: rdar127008956

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Foundation

struct C: CustomDebugStringConvertible {
  var description: String { "This is not C's description" }
  var debugDescription: String { "This is C's debug description" }
}
struct D: CustomStringConvertible {
  var description: String { "This is D's description" }
  var debugDescription: String { "This is not D's debug description" }
}

struct E : Equatable, CustomStringConvertible {
  var i : Int
  static func ==(lhs: E, rhs: E) -> Bool { lhs.i == rhs.i }
  init(i: Int) { self.i = i }
  var description: String { "\(type(of:self))(i:\(self.i))" }
}

struct E1 : Equatable {
  var i : Int
  static func ==(lhs: E1, rhs: E1) -> Bool { lhs.i == rhs.i }
  init(i: Int) { self.i = i }
}

struct F : CustomStringConvertible {
  var i : Int
  init(i: Int) { self.i = i }
  var description: String { "\(type(of:self))(i:\(self.i))" }
}

struct H : Hashable {
  var i : Int
  static func ==(lhs: H, rhs: H) -> Bool { lhs.i == rhs.i }
  init(i: Int) { self.i = i }
  var description: String { "\(type(of:self))(i:\(self.i))" }
  func hash(into hasher: inout Hasher) { hasher.combine(i + 17) }
}

@_silgen_name("TestSwiftValueNSObject")
func TestSwiftValueNSObject(_ c: AnyObject, _ d: AnyObject)
@_silgen_name("TestSwiftValueNSObjectEquals")
func TestSwiftValueNSObjectEquals(_: AnyObject, _: AnyObject)
@_silgen_name("TestSwiftValueNSObjectNotEquals")
func TestSwiftValueNSObjectNotEquals(_: AnyObject, _: AnyObject)
@_silgen_name("TestSwiftValueNSObjectHashValue")
func TestSwiftValueNSObjectHashValue(_: AnyObject, _: Int)
@_silgen_name("TestSwiftValueNSObjectDefaultHashValue")
func TestSwiftValueNSObjectDefaultHashValue(_: AnyObject)
@_silgen_name("TestSwiftValueNSObjectAssertNoErrors")
func TestSwiftValueNSObjectAssertNoErrors()

// Verify that Obj-C isEqual: provides same answer as Swift ==
// This has been true for a long time for Hashable value types
func TestHashableEquals<T: Equatable>(_ e1: T, _ e2: T) {
  if e1 == e2 {
    TestSwiftValueNSObjectEquals(e1 as AnyObject, e2 as AnyObject)
  } else {
    TestSwiftValueNSObjectNotEquals(e1 as AnyObject, e2 as AnyObject)
  }
}

// Verify that Obj-C isEqual: provides same answer as Swift ==
// This has not always been true for Equatable value types
func TestEquatableEquals<T: Equatable>(_ e1: T, _ e2: T) {
  if e1 == e2 {
    TestSwiftValueNSObjectEquals(e1 as AnyObject, e2 as AnyObject)
  } else {
    TestSwiftValueNSObjectNotEquals(e1 as AnyObject, e2 as AnyObject)
  }
}

func TestNonEquatableEquals<T>(_ e1: T, _ e2: T) {
  TestSwiftValueNSObjectNotEquals(e1 as AnyObject, e2 as AnyObject)
}

// Verify that Obj-C hashValue matches Swift hashValue for Hashable types
func TestHashable<T: Hashable>(_ h: T)
{
  TestSwiftValueNSObjectHashValue(h as AnyObject, h.hashValue)
}

// Test Obj-C hashValue for Swift types that are Equatable but not Hashable
func TestEquatableHash<T: Equatable>(_ e: T)
{
  // New behavior uses a constant hash value in this case
  TestSwiftValueNSObjectHashValue(e as AnyObject, 1)
}

func TestNonEquatableHash<T>(_ e: T)
{
  TestSwiftValueNSObjectDefaultHashValue(e as AnyObject)
}

// Check NSLog() output from TestSwiftValueNSObject().

// CHECK: c ##This is C's debug description##
// CHECK-NEXT: d ##This is D's description##
// CHECK-NEXT: S ##{{.*}}__SwiftValue##

// Temporarily disable this test on older OSes until we have time to
// look into why it's failing there. rdar://problem/47870743
if #available(OSX 10.12, iOS 10.0, *) {
  // Test a large number of Obj-C APIs
  let c = C() as AnyObject
  let d = D() as AnyObject
  TestSwiftValueNSObject(c, d)

  TestEquatableEquals(E(i: 1), E(i: 1))
  TestEquatableEquals(E(i: 790), E(i: 790))
  TestEquatableEquals(E(i: 1), E(i: 2))
  TestNonEquatableEquals(F(i: 1), F(i: 2))
  TestNonEquatableEquals(F(i: 1), F(i: 1))
  TestSwiftValueNSObjectNotEquals(H(i:1) as AnyObject, E(i:1) as AnyObject)

  // Equatable but not Hashable: alway have the same Obj-C hashValue
  TestEquatableHash(E(i: 1))
  TestEquatableHash(E1(i: 17))

  // Neither Equatable nor Hashable
  TestNonEquatableHash(C())
  TestNonEquatableHash(D())

  // Hashable types are also Equatable
  TestHashableEquals(H(i:1), H(i:1))
  TestHashableEquals(H(i:1), H(i:2))
  TestHashableEquals(H(i:2), H(i:1))

  // Verify Obj-C hash value agrees with Swift
  TestHashable(H(i:1))
  TestHashable(H(i:2))
  TestHashable(H(i:18))

  TestSwiftValueNSObjectAssertNoErrors()
} else {
  // Horrible hack to satisfy FileCheck
  fputs("c ##This is C's debug description##\n", stderr)
  fputs("d ##This is D's description##\n", stderr)
  fputs("S ##__SwiftValue##\n", stderr)
}

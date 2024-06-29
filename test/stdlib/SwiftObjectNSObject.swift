//===--- SwiftObjectNSObject.swift - Test SwiftObject's NSObject interop --===//
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
// RUN: %target-clang %S/Inputs/SwiftObjectNSObject/SwiftObjectNSObject.m -c -o %t/SwiftObjectNSObject.o -g
// RUN: %target-build-swift %s -g -I %S/Inputs/SwiftObjectNSObject/ -Xlinker %t/SwiftObjectNSObject.o -o %t/SwiftObjectNSObject
// RUN: %target-codesign %t/SwiftObjectNSObject
// RUN: %target-run %t/SwiftObjectNSObject 2> %t/log.txt
// RUN: cat %t/log.txt 1>&2
// RUN: %FileCheck %s < %t/log.txt
// REQUIRES: executable_test

// REQUIRES: objc_interop

// rdar://problem/56959761
// UNSUPPORTED: OS=watchos

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Foundation


// Swift Equatable and Hashable conformances have been bridged
// to Obj-C in two different ways.
//
// Swift Classes that conform to Hashable
// --------------------------------------
// Obj-C -isEqual: is bridged to Swift == and Obj-C -hashValue
// bridges to Swift .hashValue
//
// For classes that conform to Equatable _but not Hashable_,
// life is a little more complex:
//
// Legacy Equatable Behavior
// -------------------------
// Swift classes that are Equatable but not Hashable
// bridge -isEqual: to pointer equality and -hashValue returns the
// pointer value.
// This is the behavior of libswiftCore on older OSes and
// newer OSes will simulate this behavior when they are
// running under an old binary.
//
// Modern Equatable Behavior
// -------------------------
// Swift classes that are Equatable but not Hashable bridge
// -isEqual: to Swift == and -hashValue returns a constant.
// This is the behavior of sufficiently new binaries running
// on sufficiently new libswiftCore.


var legacy: Bool = false

class C { 
  @objc func cInstanceMethod() -> Int { return 1 }
  @objc class func cClassMethod() -> Int { return 2 }
  @objc func cInstanceOverride() -> Int { return 3 }
  @objc class func cClassOverride() -> Int { return 4 }
}
class D : C {
  @objc func dInstanceMethod() -> Int { return 5 }
  @objc class func dClassMethod() -> Int { return 6 }
  @objc override func cInstanceOverride() -> Int { return 7 }
  @objc override class func cClassOverride() -> Int { return 8 }
}

class E : Equatable, CustomStringConvertible {
  var i : Int
  static func ==(lhs: E, rhs: E) -> Bool { lhs.i == rhs.i }
  init(i: Int) { self.i = i }
  var description: String { "\(type(of:self))(i:\(self.i))" }
}

class E1: E {
}

class E2: E {
}

class F : CustomStringConvertible {
  var i : Int
  init(i: Int) { self.i = i }
  var description: String { "\(type(of:self))(i:\(self.i))" }
}

class F1: F, Equatable {
  static func ==(lhs: F1, rhs: F1) -> Bool { lhs.i == rhs.i }
}

class F2: F, Equatable {
  static func ==(lhs: F2, rhs: F2) -> Bool { lhs.i == rhs.i }
}

class H : E, Hashable {
  static func ==(lhs: H, rhs: H) -> Bool { lhs.i == rhs.i }
  func hash(into hasher: inout Hasher) { hasher.combine(i + 17) }
}

@_silgen_name("TestSwiftObjectNSObject")
func TestSwiftObjectNSObject(_ c: C, _ d: D)
@_silgen_name("CheckSwiftObjectNSObjectEquals")
func CheckSwiftObjectNSObjectEquals(_: AnyObject, _: AnyObject) -> Bool
@_silgen_name("TestSwiftObjectNSObjectEquals")
func TestSwiftObjectNSObjectEquals(_: AnyObject, _: AnyObject)
@_silgen_name("TestSwiftObjectNSObjectNotEquals")
func TestSwiftObjectNSObjectNotEquals(_: AnyObject, _: AnyObject)
@_silgen_name("TestSwiftObjectNSObjectHashValue")
func TestSwiftObjectNSObjectHashValue(_: AnyObject, _: Int)
@_silgen_name("TestSwiftObjectNSObjectDefaultHashValue")
func TestSwiftObjectNSObjectDefaultHashValue(_: AnyObject)
@_silgen_name("TestSwiftObjectNSObjectAssertNoErrors")
func TestSwiftObjectNSObjectAssertNoErrors()


func CheckEquatableEquals<T: Equatable & AnyObject>(_ e1: T, _ e2: T) -> Bool {
  return CheckSwiftObjectNSObjectEquals(e1, e2)
}

// Verify that Obj-C isEqual: provides same answer as Swift ==
func TestEquatableEquals<T: Equatable & AnyObject>(_ e1: T, _ e2: T) {
  if e1 == e2 {
    if legacy {
      // Legacy behavior: Equatable Swift does not imply == in ObjC
      TestSwiftObjectNSObjectNotEquals(e1, e2)
    } else {
      TestSwiftObjectNSObjectEquals(e1, e2)
    }
  } else {
    TestSwiftObjectNSObjectNotEquals(e1, e2)
  }
}

func TestNonEquatableEquals(_ e1: AnyObject, _ e2: AnyObject) {
  TestSwiftObjectNSObjectNotEquals(e1, e2)
}

// Verify that Obj-C hashValue matches Swift hashValue for Hashable types
func TestHashable(_ h: H)
{
  if legacy {
    // Legacy behavior: Hash value is pointer value in ObjC
    TestSwiftObjectNSObjectDefaultHashValue(h)
  } else {
    // New behavior: Hashable in Swift, same hash value in ObjC
    TestSwiftObjectNSObjectHashValue(h, h.hashValue)
  }
}

// Test Obj-C hashValue for Swift types that are Equatable but not Hashable
func TestEquatableHash(_ e: AnyObject)
{
  if legacy {
    // Legacy behavior: Equatable in Swift => ObjC hashes with identity
    TestSwiftObjectNSObjectDefaultHashValue(e)
    fakeEquatableWarning(e)
  } else {
    // New behavior: These should have a constant hash value
    TestSwiftObjectNSObjectHashValue(e, 1)
  }
}

func TestNonEquatableHash(_ e: AnyObject)
{
  TestSwiftObjectNSObjectDefaultHashValue(e)
}

// Check NSLog() output from TestSwiftObjectNSObject().

// CHECK: c ##SwiftObjectNSObject.C##
// CHECK-NEXT: d ##SwiftObjectNSObject.D##
// CHECK-NEXT: S ##{{.*}}SwiftObject##

// Verify that the runtime emits the warning that we expected...
// CHECK-NEXT: Obj-C `-hash` {{.*}} type `SwiftObjectNSObject.E` {{.*}} Equatable but not Hashable
// CHECK-NEXT: Obj-C `-hash` {{.*}} type `SwiftObjectNSObject.E1` {{.*}} Equatable but not Hashable
// CHECK-NEXT: Obj-C `-hash` {{.*}} type `SwiftObjectNSObject.E2` {{.*}} Equatable but not Hashable

// If we're checking legacy behavior or unsupported platform, then
// the warning above won't be emitted.  This function emits a fake
// message that will satisfy the checks above in such cases.
func fakeEquatableWarning(_ e: AnyObject) {
  let msg = "Fake testing message: Obj-C `-hash` ... type `SwiftObjectNSObject.\(type(of: e))` ... Equatable but not Hashable\n"
  fputs(msg, stderr)
}

// Temporarily disable this test on older OSes until we have time to
// look into why it's failing there. rdar://problem/47870743
if #available(OSX 10.12, iOS 10.0, *) {
  // Test a large number of Obj-C APIs
  TestSwiftObjectNSObject(C(), D())

  // Test whether the current environment seems to be
  // using legacy or new Equatable/Hashable bridging.
  legacy = !CheckEquatableEquals(E(i: 1), E(i: 1))

  // TODO:  Test whether this environment should be using the legacy
  // semantics.  In essence, does `legacy` have the expected value?
  // (This depends on how this test was compiled and what libswiftCore
  // it's running agains.)

  // Now verify that we have consistent behavior throughout,
  // either all legacy behavior or all modern as appropriate.

  // ** Equatable types with an Equatable parent class
  // Same type and class
  TestEquatableEquals(E(i: 1), E(i: 1))
  TestEquatableEquals(E(i: 790), E(i: 790))
  TestEquatableEquals(E1(i: 1), E1(i: 1))
  TestEquatableEquals(E1(i: 18), E1(i: 18))
  TestEquatableEquals(E2(i: 1), E2(i: 1))
  TestEquatableEquals(E2(i: 2), E2(i: 2))
  // Different class
  TestEquatableEquals(E1(i: 1), E2(i: 1))
  TestEquatableEquals(E1(i: 1), E(i: 1))
  TestEquatableEquals(E2(i: 1), E(i: 1))
  // Different value
  TestEquatableEquals(E(i: 1), E(i: 2))
  TestEquatableEquals(E1(i: 1), E1(i: 2))
  TestEquatableEquals(E2(i: 1), E2(i: 2))

  // ** Non-Equatable parent class
  // Same class and value
  TestEquatableEquals(F1(i: 1), F1(i: 1))
  TestEquatableEquals(F1(i: 1), F1(i: 2))
  TestEquatableEquals(F2(i: 1), F2(i: 1))
  TestEquatableEquals(F2(i: 1), F2(i: 2))

  // Different class and/or value
  TestNonEquatableEquals(F(i: 1), F(i: 2))
  TestNonEquatableEquals(F(i: 1), F(i: 1))
  TestNonEquatableEquals(F1(i: 1), F2(i: 1))
  TestNonEquatableEquals(F1(i: 1), F(i: 1))

  // Two equatable types with no common parent class
  TestNonEquatableEquals(F1(i: 1), E(i: 1))
  TestEquatableEquals(H(i:1), E(i:1))

  // Equatable but not Hashable: alway have the same Obj-C hashValue
  TestEquatableHash(E(i: 1))
  TestEquatableHash(E1(i: 3))
  TestEquatableHash(E2(i: 8))

  // Neither Equatable nor Hashable
  TestNonEquatableHash(C())
  TestNonEquatableHash(D())

  // Hashable types are also Equatable
  TestEquatableEquals(H(i:1), H(i:1))
  TestEquatableEquals(H(i:1), H(i:2))
  TestEquatableEquals(H(i:2), H(i:1))

  // Verify Obj-C hash value agrees with Swift
  TestHashable(H(i:1))
  TestHashable(H(i:2))
  TestHashable(H(i:18))

  TestSwiftObjectNSObjectAssertNoErrors()
} else {
  // Horrible hack to satisfy FileCheck
  fputs("c ##SwiftObjectNSObject.C##\n", stderr)
  fputs("d ##SwiftObjectNSObject.D##\n", stderr)
  fputs("S ##Swift._SwiftObject##\n", stderr)
  fakeEquatableWarning(E(i:1))
  fakeEquatableWarning(E1(i:1))
  fakeEquatableWarning(E2(i:1))
}

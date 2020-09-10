// RUN: %empty-directory(%t)
// RUN: %target-build-swift  -swift-version 4 %s -o %t/a.out -enforce-exclusivity=checked -Onone
//
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test
// REQUIRES: swift_stable_abi

// Tests for traps at run time when enforcing exclusive access.

import StdlibUnittest
import SwiftPrivateThreadExtras

struct X {
  var i = 7
}

/// Calling this function will begin a read access to the variable referred to
/// in the first parameter that lasts for the duration of the call. Any
/// accesses in the closure will therefore be nested inside the outer read.
func readAndPerform<T>(_ _: UnsafePointer<T>, closure: () ->()) {
  closure()
}

/// Begin a modify access to the first parameter and call the closure inside it.
func modifyAndPerform<T>(_ _: UnsafeMutablePointer<T>, closure: () ->()) {
  closure()
}

/// Begin a modify access to the first parameter and call the escaping closure inside it.
func modifyAndPerformEscaping<T>(_ _: UnsafeMutablePointer<T>, closure: () ->()) {
  closure()
}

var globalX = X()

var ExclusiveAccessTestSuite = TestSuite("ExclusiveAccess")

ExclusiveAccessTestSuite.test("Read") {
  let l = globalX // no-trap
  _blackHole(l)
}

// It is safe for a read access to overlap with a read.
ExclusiveAccessTestSuite.test("ReadInsideRead") {
  readAndPerform(&globalX) {
    let l = globalX // no-trap
    _blackHole(l)
  }
}

ExclusiveAccessTestSuite.test("ModifyInsideRead")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a read) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  readAndPerform(&globalX) {
    expectCrashLater()
    globalX = X()
  }
}

ExclusiveAccessTestSuite.test("ReadInsideModify")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a read) started at")
  .code
{
  modifyAndPerform(&globalX) {
    expectCrashLater()
    let l = globalX
    _blackHole(l)
  }
}

ExclusiveAccessTestSuite.test("ModifyInsideModify")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  modifyAndPerform(&globalX) {
    expectCrashLater()
    globalX.i = 12
  }
}

var globalOtherX = X()

// It is safe for two modifications of different variables
// to overlap.
ExclusiveAccessTestSuite.test("ModifyInsideModifyOfOther") {
  modifyAndPerform(&globalOtherX) {
    globalX.i = 12 // no-trap
  }
}

// The access durations for these two modifications do not overlap
ExclusiveAccessTestSuite.test("ModifyFollowedByModify") {
  globalX = X()
  _blackHole(())

  globalX = X() // no-trap
}

ExclusiveAccessTestSuite.test("ClosureCaptureReadRead") {
  var x = X()
  readAndPerform(&x) {
    _blackHole(x.i) // no-trap
  }
}

// Test for per-thread enforcement. Don't trap when two different threads
// have overlapping accesses
ExclusiveAccessTestSuite.test("PerThreadEnforcement") {
  modifyAndPerform(&globalX) {
    let (_, otherThread) = _stdlib_thread_create_block({ (_ : Void) -> () in
      globalX.i = 12 // no-trap
      return ()
    }, ())

    _ = _stdlib_thread_join(otherThread!, Void.self)
  }
}

// Helpers
func doOne(_ f: () -> ()) { f() }

func doTwo(_ f1: ()->(), _ f2: ()->()) { f1(); f2() }

// No crash.
ExclusiveAccessTestSuite.test("WriteNoescapeWrite") {
  var x = 3
  let c = { x = 7 }
  // Inside may-escape closure `c`: [read] [dynamic]
  // Inside never-escape closure: [modify] [dynamic]
  doTwo(c, { x = 42 })
  _blackHole(x)
}

// No crash.
ExclusiveAccessTestSuite.test("InoutReadEscapeRead") {
  var x = 3
  let c = { let y = x; _blackHole(y) }
  readAndPerform(&x, closure: c)
  _blackHole(x)
}

ExclusiveAccessTestSuite.test("InoutReadEscapeWrite")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a read) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  var x = 3
  let c = { x = 42 }
  expectCrashLater()
  readAndPerform(&x, closure: c) 
  _blackHole(x)
}

ExclusiveAccessTestSuite.test("InoutWriteEscapeRead")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a read) started at")
  .code
{
  var x = 3
  let c = { let y = x; _blackHole(y) }
  expectCrashLater()
  modifyAndPerform(&x, closure: c)
  _blackHole(x)
}

ExclusiveAccessTestSuite.test("InoutWriteEscapeWrite")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  var x = 3
  let c = { x = 42 }
  expectCrashLater()
  modifyAndPerform(&x, closure: c)
  _blackHole(x)
}

// No crash.
ExclusiveAccessTestSuite.test("InoutReadNoescapeRead") {
  var x = 3
  let c = { let y = x; _blackHole(y) }
  doOne { readAndPerform(&x, closure: c) }
}

ExclusiveAccessTestSuite.test("InoutReadNoescapeWrite")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a read) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  var x = 3
  let c = { x = 7 }
  expectCrashLater()
  doOne { readAndPerform(&x, closure: c) }
}

ExclusiveAccessTestSuite.test("InoutWriteEscapeReadClosure")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a read) started at")
  .code
{
  var x = 3
  let c = { let y = x; _blackHole(y) }
  expectCrashLater()
  doOne { modifyAndPerform(&x, closure: c) }
}

ExclusiveAccessTestSuite.test("InoutWriteEscapeWriteClosure")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  var x = 3
  let c = { x = 7 }
  expectCrashLater()
  doOne { modifyAndPerform(&x, closure: c) }
}

class ClassWithStoredProperty {
  final var f = 7
}

ExclusiveAccessTestSuite.test("KeyPathInoutDirectWriteClassStoredProp")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  expectCrashLater()
  modifyAndPerform(&c[keyPath: getF]) {
    c.f = 12
  }
}

ExclusiveAccessTestSuite.test("KeyPathInoutDirectReadClassStoredProp")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a read) started at")
  .code
{
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  expectCrashLater()
  modifyAndPerform(&c[keyPath: getF]) {
    let x = c.f
    _blackHole(x)
  }
}

// Unlike inout accesses, read-only inout-to-pointer conversions on key paths for
// final stored-properties do not perform a long-term read access. Instead, they
// materialize a location on the stack, perform an instantaneous read
// from the storage indicated by the key path and write the read value to the
// stack location. The stack location is then passed as the pointer for the
// inout-to-pointer conversion.
//
// This means that there is no conflict between a read-only inout-to-pointer
// conversion of the key path location for a call and an access to the
// the same location within the call.
ExclusiveAccessTestSuite.test("KeyPathReadOnlyInoutToPtrDirectWriteClassStoredProp") {
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  // This performs an instantaneous read
  readAndPerform(&c[keyPath: getF]) {
    c.f = 12 // no-trap
  }
}

ExclusiveAccessTestSuite.test("SequentialKeyPathWritesDontOverlap") {
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  c[keyPath: getF] = 7
  c[keyPath: getF] = 8 // no-trap
  c[keyPath: getF] += c[keyPath: getF] // no-trap
}

ExclusiveAccessTestSuite.test("KeyPathInoutKeyPathWriteClassStoredProp")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a modification) started at")
  .code
{
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  expectCrashLater()
  modifyAndPerform(&c[keyPath: getF]) {
    c[keyPath: getF] = 12
  }
}

ExclusiveAccessTestSuite.test("KeyPathInoutKeyPathReadClassStoredProp")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Previous access (a modification) started at")
  .crashOutputMatches("Current access (a read) started at")
  .code
{
  let getF = \ClassWithStoredProperty.f
  let c = ClassWithStoredProperty()

  expectCrashLater()
  modifyAndPerform(&c[keyPath: getF]) {
    let y = c[keyPath: getF]
    _blackHole(y)
  }
}

// <rdar://problem/43076947> [Exclusivity] improve diagnostics for
// withoutActuallyEscaping.
ExclusiveAccessTestSuite.test("withoutActuallyEscapingConflict") {
  var localVal = 0
  let nestedModify = { localVal = 3 }
  withoutActuallyEscaping(nestedModify) {
    expectCrashLater()
    modifyAndPerform(&localVal, closure: $0)
  }
}

ExclusiveAccessTestSuite.test("directlyAppliedConflict") {
  var localVal = 0
  let nestedModify = { localVal = 3 }
  expectCrashLater()
  _ = {
    modifyAndPerform(&localVal, closure: nestedModify)
  }()
}

// <rdar://problem/43122715> [Exclusivity] failure to diagnose
// escaping closures called within directly applied noescape closures.
ExclusiveAccessTestSuite.test("directlyAppliedEscapingConflict") {
  var localVal = 0
  let nestedModify = { localVal = 3 }
  expectCrashLater()
  _ = {
    modifyAndPerformEscaping(&localVal, closure: nestedModify)
  }()
}

runAllTests()

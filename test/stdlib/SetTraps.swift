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

var SetTraps = TestSuite("SetTraps" + testSuiteSuffix)

SetTraps.test("RemoveInvalidIndex1")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s = Set<Int>()
  let index = s.startIndex
  expectCrashLater()
  s.remove(at: index)
}

SetTraps.test("RemoveInvalidIndex2")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s = Set<Int>()
  let index = s.endIndex
  expectCrashLater()
  s.remove(at: index)
}

SetTraps.test("RemoveInvalidIndex3")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s: Set<Int> = [ 10, 20, 30 ]
  let index = s.endIndex
  expectCrashLater()
  s.remove(at: index)
}

SetTraps.test("RemoveInvalidIndex4")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s: Set<Int> = [ 10 ]
  let index = s.firstIndex(of: 10)!
  s.remove(at: index)
  expectFalse(s.contains(10))
  expectCrashLater()
  s.remove(at: index)
}

SetTraps.test("RemoveFirstFromEmpty")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(_isDebugAssertConfiguration() ?
    "Can't removeFirst from an empty Set" : "")
  .code {
  var s = Set<Int>()
  expectCrashLater()
  s.removeFirst()
}

runAllTests()


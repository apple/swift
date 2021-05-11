// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out_Debug -Onone
// RUN: %target-build-swift %s -o %t/a.out_Release -O
// RUN: %target-codesign %t/a.out_Debug
// RUN: %target-codesign %t/a.out_Release
//
// RUN: %target-run %t/a.out_Debug
// RUN: %target-run %t/a.out_Release
// REQUIRES: executable_test
// UNSUPPORTED: OS=wasi

import StdlibUnittest

let testSuiteSuffix = _isDebugAssertConfiguration() ? "_debug" : "_release"

var CharacterTraps = TestSuite("CharacterTraps" + testSuiteSuffix)

CharacterTraps.test("CharacterFromEmptyString")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s = ""
  expectCrashLater()
  _ = Character(s)
}

CharacterTraps.test("CharacterFromMoreThanOneGraphemeCluster")
  .skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "this trap is only guaranteed to happen in Debug builds"))
  .code {
  var s = "ab"
  expectCrashLater()
  _ = Character(s)
}

runAllTests()


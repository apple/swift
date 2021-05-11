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

var DictionaryTraps = TestSuite("DictionaryTraps" + testSuiteSuffix)

DictionaryTraps.test("DuplicateKeys1")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  expectCrashLater()
  let d = Dictionary(dictionaryLiteral:
    (10, 1010), (20, 1020), (30, 1030), (10, 0))
  _blackHole(d)
}

DictionaryTraps.test("DuplicateKeys2")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  expectCrashLater()
  let d = Dictionary(dictionaryLiteral:
    (10, 1010), (20, 1020), (30, 1030), (10, 1010))
  _blackHole(d)
}

DictionaryTraps.test("DuplicateKeys3")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  expectCrashLater()
  let d = [ 10: 1010, 10: 0 ]
  _blackHole(d)
}

DictionaryTraps.test("RemoveInvalidIndex1")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var d = Dictionary<Int, Int>()
  let index = d.startIndex
  expectCrashLater()
  d.remove(at: index)
}

DictionaryTraps.test("RemoveInvalidIndex2")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var d = Dictionary<Int, Int>()
  let index = d.endIndex
  expectCrashLater()
  d.remove(at: index)
}

DictionaryTraps.test("RemoveInvalidIndex3")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var d = [ 10: 1010, 20: 1020, 30: 1030 ]
  let index = d.endIndex
  expectCrashLater()
  d.remove(at: index)
}

DictionaryTraps.test("RemoveInvalidIndex4")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var d = [ 10: 1010 ]
  let index = d.index(forKey: 10)!
  d.remove(at: index)
  expectNil(d[10])
  expectCrashLater()
  d.remove(at: index)
}

runAllTests()

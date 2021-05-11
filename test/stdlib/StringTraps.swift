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

var StringTraps = TestSuite("StringTraps" + testSuiteSuffix)

StringTraps.test("startIndex/predecessor")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.startIndex
  i = s.index(after: i)
  i = s.index(before: i)
  expectCrashLater()
  i = s.index(before: i)
}

StringTraps.test("endIndex/successor")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.startIndex
  i = s.index(after: i)
  i = s.index(after: i)
  i = s.index(after: i)
  expectCrashLater()
  i = s.index(after: i)
}

StringTraps.test("subscript(_:)/endIndex")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.startIndex
  i = s.index(after: i)
  i = s.index(after: i)
  i = s.index(after: i)
  expectCrashLater()
  _ = s[i]
}

StringTraps.test("UTF8ViewSubscript/endIndexSuccessor")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.utf8.startIndex
  i = s.utf8.index(after: i)
  i = s.utf8.index(after: i)
  i = s.utf8.index(after: i)
  expectCrashLater()
  i = s.utf8.index(after: i)
  _ = s.utf8[i]
}

StringTraps.test("UTF8ViewSubscript/endIndex")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.utf8.startIndex
  i = s.utf8.index(after: i)
  i = s.utf8.index(after: i)
  i = s.utf8.index(after: i)
  expectCrashLater()
  _ = s.utf8[i]
}

StringTraps.test("UTF16ViewSubscript/DecrementedStartIndex")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s = "abc"
  var i = s.utf16.startIndex
  expectCrashLater()
  i = s.utf16.index(before: i)
  _ = s.utf16[i]
}

StringTraps.test("UTF16ViewSubscript/endIndex")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  var s = "abc"
  var i = s.utf16.startIndex
  i = s.utf16.index(after: i)
  i = s.utf16.index(after: i)
  i = s.utf16.index(after: i)
  expectCrashLater()
  _ = s.utf16[i]
}

StringTraps.test("UTF16ViewIndex/offsetLimited")
  .code {
  let sa = "foo"
  let u16a = sa.utf16
  let s16 = sa + "🤦🏻‍♀️"
  let u16 = s16.utf16

  let iaBegin = u16a.index(sa.startIndex, offsetBy: 99, limitedBy: sa.endIndex)
  expectNil(iaBegin)
  let iaEnd = u16a.index(sa.endIndex, offsetBy: 99, limitedBy: sa.endIndex)
  expectNil(iaEnd)
  let i16Begin = u16.index(u16.startIndex, offsetBy: 99, limitedBy: u16.endIndex)
  expectNil(i16Begin)
  let i16End = u16.index(u16.startIndex, offsetBy: 99, limitedBy: u16.endIndex)
  expectNil(i16End)
}

StringTraps.test("UTF16ViewIndex/offsetCrash")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s16 = "foo🤦🏻‍♀️"
  let u16 = s16.utf16
  expectCrashLater()
  let i = u16.index(u16.startIndex, offsetBy: 99)
  _ = s16.utf16[i]
}

StringTraps.test("UTF8ViewIndex/offsetLimited")
  .code {
  let sa = "foo"
  let u8a = sa.utf8
  let s8 = sa + "🤦🏻‍♀️"
  let u8 = s8.utf8

  let iaBegin = u8a.index(sa.startIndex, offsetBy: 99, limitedBy: sa.endIndex)
  expectNil(iaBegin)
  let iaEnd = u8a.index(sa.endIndex, offsetBy: 99, limitedBy: sa.endIndex)
  expectNil(iaEnd)
  let i8Begin = u8.index(u8.startIndex, offsetBy: 99, limitedBy: u8.endIndex)
  expectNil(i8Begin)
  let i8End = u8.index(u8.startIndex, offsetBy: 99, limitedBy: u8.endIndex)
  expectNil(i8End)
}

StringTraps.test("UTF8ViewIndex/offsetCrash")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let s8 = "foo🤦🏻‍♀️"
  let u8 = s8.utf8
  expectCrashLater()
  let i = u8.index(u8.startIndex, offsetBy: 99)
  _ = s8.utf8[i]
}

runAllTests()


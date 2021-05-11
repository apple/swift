// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest


var StaticStringTestSuite = TestSuite("StaticString")

StaticStringTestSuite.test("PointerRepresentation/ASCII/Empty") {
  let str = StaticString()
  expectEqual(0x00, str.utf8Start[0])
  expectEqual(0, str.utf8CodeUnitCount)
  expectTrue(str.hasPointerRepresentation)
  expectTrue(str.isASCII)
  str.withUTF8Buffer {
    utf8 in
    expectEqual(0, utf8.count)
  }
  expectEqual("", str.description)

  expectPrinted("", str)
  expectDebugPrinted("\"\"", str)
}

StaticStringTestSuite.test("PointerRepresentation/ASCII") {
  let str: StaticString = "abc"
  expectEqual(0x61, str.utf8Start[0])
  expectEqual(0x62, str.utf8Start[1])
  expectEqual(0x63, str.utf8Start[2])
  expectEqual(0x00, str.utf8Start[3])
  expectEqual(3, str.utf8CodeUnitCount)
  expectTrue(str.hasPointerRepresentation)
  expectTrue(str.isASCII)
  str.withUTF8Buffer {
    (utf8) -> Void in
    expectEqual(3, utf8.count)
    expectEqual(0x61, utf8[0])
    expectEqual(0x62, utf8[1])
    expectEqual(0x63, utf8[2])
  }
  expectEqual("abc", str.description)

  expectPrinted("abc", str)
  expectDebugPrinted("\"abc\"", str)
}

StaticStringTestSuite.test("PointerRepresentation/NonASCII") {
  let str: StaticString = "абв"
  expectEqual(0xd0, str.utf8Start[0])
  expectEqual(0xb0, str.utf8Start[1])
  expectEqual(0xd0, str.utf8Start[2])
  expectEqual(0xb1, str.utf8Start[3])
  expectEqual(0xd0, str.utf8Start[4])
  expectEqual(0xb2, str.utf8Start[5])
  expectEqual(0x00, str.utf8Start[6])
  expectEqual(6, str.utf8CodeUnitCount)
  expectTrue(str.hasPointerRepresentation)
  expectFalse(str.isASCII)
  str.withUTF8Buffer {
    (utf8) -> Void in
    expectEqual(6, utf8.count)
    expectEqual(0xd0, utf8[0])
    expectEqual(0xb0, utf8[1])
    expectEqual(0xd0, utf8[2])
    expectEqual(0xb1, utf8[3])
    expectEqual(0xd0, utf8[4])
    expectEqual(0xb2, utf8[5])
  }
  expectEqual("абв", str.description)

  expectPrinted("абв", str)
  expectDebugPrinted("\"абв\"", str)
}

#if !os(WASI)
StaticStringTestSuite.test("PointerRepresentation/unicodeScalar")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(_isDebugAssertConfiguration() ?
    "StaticString should have Unicode scalar representation" : "")
  .code {
  let str: StaticString = "abc"
  let strOpaque = _opaqueIdentity(str)
  expectCrashLater()
  strOpaque.unicodeScalar
}
#endif

StaticStringTestSuite.test("UnicodeScalarRepresentation/ASCII") {
  // The type checker does not call the UnicodeScalar initializer even if
  // passed a literal that consists from a single Unicode scalar.

  // U+005A LATIN CAPITAL LETTER Z
  let str = StaticString(_builtinUnicodeScalarLiteral: UInt32(0x5a)._value)
  expectEqual("Z", str.unicodeScalar)
  expectFalse(str.hasPointerRepresentation)
  expectTrue(str.isASCII)
  str.withUTF8Buffer {
    (utf8) -> Void in
    expectEqual(1, utf8.count)
    expectEqual(0x5a, utf8[0])
  }
  expectEqual("Z", str.description)

  expectPrinted("Z", str)
  expectDebugPrinted("\"Z\"", str)
}

StaticStringTestSuite.test("UnicodeScalarRepresentation/NonASCII") {
  // U+042B CYRILLIC CAPITAL LETTER YERU
  let str = StaticString(_builtinUnicodeScalarLiteral: UInt32(0x042b)._value)
  expectEqual("Ы", str.unicodeScalar)
  expectFalse(str.hasPointerRepresentation)
  expectFalse(str.isASCII)
  str.withUTF8Buffer {
    (utf8) -> Void in
    expectEqual(0xd0, utf8[0])
    expectEqual(0xab, utf8[1])
  }
  expectEqual("Ы", str.description)

  expectPrinted("Ы", str)
  expectDebugPrinted("\"Ы\"", str)
}

#if !os(WASI)
StaticStringTestSuite.test("UnicodeScalarRepresentation/utf8Start")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(_isDebugAssertConfiguration() ?
    "StaticString should have pointer representation" : "")
  .code {
  let str = StaticString(_builtinUnicodeScalarLiteral: UInt32(0x5a)._value)
  let strOpaque = _opaqueIdentity(str)
  expectCrashLater()
  strOpaque.utf8Start
}

StaticStringTestSuite.test("UnicodeScalarRepresentation/utf8CodeUnitCount")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(_isDebugAssertConfiguration() ?
    "StaticString should have pointer representation" : "")
  .code {
  let str = StaticString(_builtinUnicodeScalarLiteral: UInt32(0x5a)._value)
  let strOpaque = _opaqueIdentity(str)
  expectCrashLater()
  strOpaque.utf8CodeUnitCount
}
#endif

runAllTests()


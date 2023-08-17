// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import StdlibUnittest
import CxxStdlib

var StdStringOverlayTestSuite = TestSuite("std::string overlay")

StdStringOverlayTestSuite.test("std::string <=> Swift.String") {
  let cxx1 = std.string()
  let swift1 = String(cxx1)
  expectEqual(swift1, "")

  let cxx2 = std.string("something123")
  let swift2 = String(cxx2)
  expectEqual(swift2, "something123")

  let cxx3: std.string = "literal"
  expectEqual(cxx3.size(), 7)

  // Non-ASCII characters are represented by more than one CChar.
  let cxx4: std.string = "тест"
  expectEqual(cxx4.size(), 8)
  let swift4 = String(cxx4)
  expectEqual(swift4, "тест")

  let cxx5: std.string = "emoji_🤖"
  expectEqual(cxx5.size(), 10)
  let swift5 = String(cxx5)
  expectEqual(swift5, "emoji_🤖")

  let cxx6 = std.string("xyz\0abc")
  expectEqual(cxx6.size(), 7)
  let swift6 = String(cxx6)
  expectEqual(swift6, "xyz\0abc")

  var cxx7 = std.string()
  let bytes: [UInt8] = [0xE1, 0xC1, 0xAC]
  for byte in bytes {
    cxx7.push_back(CChar(bitPattern: byte))
  }
  let swift7 = String(cxx7)
  expectEqual(swift7, "���")
}

StdStringOverlayTestSuite.test("std::string operators") {
  var s1 = std.string("something")
  let s2 = std.string("123")
  let sum = s1 + s2
  expectEqual(sum, std.string("something123"))

  expectFalse(s1 == s2)
  let s3 = std.string("something123")
  expectFalse(s1 == s3)
  expectFalse(s2 == s3)

  s1 += s2
  expectTrue(s1 == std.string("something123"))
  expectTrue(s1 == s3)

  // Make sure the operators work together with ExpressibleByStringLiteral conformance.
  s1 += "literal"
  expectTrue(s1 == "something123literal")
}

StdStringOverlayTestSuite.test("std::u16string operators") {
  var s1 = std.u16string("something")
  let s2 = std.u16string("123")
  let sum = s1 + s2
  expectEqual(sum, std.u16string("something123"))

  expectFalse(s1 == s2)
  let s3 = std.u16string("something123")
  expectFalse(s1 == s3)
  expectFalse(s2 == s3)

  s1 += s2
  expectTrue(s1 == std.u16string("something123"))
  expectTrue(s1 == s3)

  // Make sure the operators work together with ExpressibleByStringLiteral conformance.
  s1 += "literal"
  expectTrue(s1 == "something123literal")
}

StdStringOverlayTestSuite.test("std::string::append") {
  var s1 = std.string("0123")
  let s2 = std.string("abc")
  s1.append(s2)
  expectEqual(s1, std.string("0123abc"))
}

StdStringOverlayTestSuite.test("std::u16string::append") {
  var s1 = std.u16string("0123")
  let s2 = std.u16string("abc")
  s1.append(s2)
  expectEqual(s1, std.u16string("0123abc"))
}

StdStringOverlayTestSuite.test("std::string as Hashable") {
  let s0 = std.string()
  let h0 = s0.hashValue

  let s1 = std.string("something")
  let h1 = s1.hashValue

  let s2 = std.string("something123")
  let h2 = s2.hashValue

  let s3 = std.string("something")
  let h3 = s3.hashValue

  expectEqual(h1, h3)
  expectNotEqual(h0, h1)
  expectNotEqual(h0, h2)
  expectNotEqual(h0, h3)
  expectNotEqual(h1, h2)
  expectNotEqual(h2, h3)
}

StdStringOverlayTestSuite.test("std::u16string as Hashable") {
  let s0 = std.u16string()
  let h0 = s0.hashValue

  let s1 = std.u16string("something")
  let h1 = s1.hashValue

  let s2 = std.u16string("something123")
  let h2 = s2.hashValue

  let s3 = std.u16string("something")
  let h3 = s3.hashValue

  expectEqual(h1, h3)
  expectNotEqual(h0, h1)
  expectNotEqual(h0, h2)
  expectNotEqual(h0, h3)
  expectNotEqual(h1, h2)
  expectNotEqual(h2, h3)
}

StdStringOverlayTestSuite.test("std::u16string <=> Swift.String") {
  let cxx1 = std.u16string()
  let swift1 = String(cxx1)
  expectEqual(swift1, "")

  let cxx2 = std.u16string("something123")
  expectEqual(cxx2.size(), 12)
  let swift2 = String(cxx2)
  expectEqual(swift2, "something123")

  let cxx3: std.u16string = "literal"
  expectEqual(cxx3.size(), 7)

  let cxx4: std.u16string = "тест"
  expectEqual(cxx4.size(), 4)
  let swift4 = String(cxx4)
  expectEqual(swift4, "тест")

  // Emojis are represented by more than one CWideChar.
  let cxx5: std.u16string = "emoji_🤖"
  expectEqual(cxx5.size(), 8)
  let swift5 = String(cxx5)
  expectEqual(swift5, "emoji_🤖")

  let cxx6 = std.u16string("xyz\0abc")
  expectEqual(cxx6.size(), 7)
  let swift6 = String(cxx6)
  expectEqual(swift6, "xyz\0abc")
}

StdStringOverlayTestSuite.test("std::string as Swift.CustomDebugStringConvertible") {
  let cxx1 = std.string()
  expectEqual(cxx1.debugDescription, "std.string()")

  let cxx2 = std.string("something123")
  expectEqual(cxx2.debugDescription, "std.string(something123)")

  let bytes: [UInt8] = [0xE1, 0xC1, 0xAC]
  var cxx3 = std.string()
  for byte in bytes {
    cxx3.push_back(CChar(bitPattern: byte))
  }
  expectEqual(cxx3.debugDescription, "std.string(���)")
}

StdStringOverlayTestSuite.test("std::u16string as Swift.CustomDebugStringConvertible") {
  let cxx1 = std.u16string()
  expectEqual(cxx1.debugDescription, "std.u16string()")

  let cxx2 = std.u16string("something123")
  expectEqual(cxx2.debugDescription, "std.u16string(something123)")

  let scalars: [UInt16] = [97, 55296, 99]
  var cxx3 = std.u16string()
  for scalar in scalars {
    cxx3.push_back(scalar)
  }
  expectEqual(cxx3.debugDescription, "std.u16string(a�c)")
}

StdStringOverlayTestSuite.test("std::string as Swift.Sequence") {
  let cxx1 = std.string()
  var iterated = false
  for _ in cxx1 {
    iterated = true
  }
  expectFalse(iterated)

  let cxx2 = std.string("abc123")
  var chars = 0
  var sum = 0
  for it in cxx2 {
    chars += 1
    sum += Int(it)
  }
  expectEqual(6, chars)
  expectEqual(97 + 98 + 99 + 49 + 50 + 51, sum)
}

StdStringOverlayTestSuite.test("std::string as CustomStringConvertible") {
  let cxx1 = std.string()
  expectEqual(cxx1.description, "")

  let cxx2 = std.string("something123")
  expectEqual(cxx2.description, "something123")

  let bytes: [UInt8] = [0xE1, 0xC1, 0xAC]
  var cxx3 = std.string()
  for byte in bytes {
    cxx3.push_back(CChar(bitPattern: byte))
  }
  expectEqual(cxx3.description, "���")
}

StdStringOverlayTestSuite.test("std::u16string as Swift.CustomStringConvertible") {
  let cxx1 = std.u16string()
  expectEqual(cxx1.description, "")

  let cxx2 = std.u16string("something123")
  expectEqual(cxx2.description, "something123")

  let scalars: [UInt16] = [97, 55296, 99]
  var cxx3 = std.u16string()
  for scalar in scalars {
    cxx3.push_back(scalar)
  }
  expectEqual(cxx3.description, "a�c")
}

StdStringOverlayTestSuite.test("std::string from C string") {
  let str = "abc".withCString { ptr in
    std.string(ptr)
  }
  expectEqual(str, std.string("abc"))
}

runAllTests()

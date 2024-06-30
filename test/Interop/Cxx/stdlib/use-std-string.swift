// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -D USE_CUSTOM_STRING_API)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=swift-6 -D USE_CUSTOM_STRING_API)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -D USE_CUSTOM_STRING_API)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -D USE_CUSTOM_STRING_API -Xcc -std=c++14)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -D USE_CUSTOM_STRING_API -Xcc -std=c++17)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -D USE_CUSTOM_STRING_API -Xcc -std=c++20)
//
// REQUIRES: executable_test

import StdlibUnittest
import CxxStdlib
#if USE_CUSTOM_STRING_API
import StdString
#endif

var StdStringTestSuite = TestSuite("StdString")

StdStringTestSuite.test("init") {
    let s = std.string()
    expectEqual(s.size(), 0)
    expectTrue(s.empty())
}

StdStringTestSuite.test("push back") {
    var s = std.string()
    s.push_back(42)
    expectEqual(s.size(), 1)
    expectFalse(s.empty())
    expectEqual(s[0], 42)
}

StdStringTestSuite.test("std::string <=> Swift.String") {
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

StdStringTestSuite.test("std::string operators") {
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

StdStringTestSuite.test("std::u16string operators") {
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

StdStringTestSuite.test("std::u32string operators") {
    var s1 = std.u32string("something")
    let s2 = std.u32string("123")
    let sum = s1 + s2
    expectEqual(sum, std.u32string("something123"))

    expectFalse(s1 == s2)
    let s3 = std.u32string("something123")
    expectFalse(s1 == s3)
    expectFalse(s2 == s3)

    s1 += s2
    expectTrue(s1 == std.u32string("something123"))
    expectTrue(s1 == s3)

    // Make sure the operators work together with ExpressibleByStringLiteral conformance.
    s1 += "literal"
    expectTrue(s1 == "something123literal")
}

StdStringTestSuite.test("std::string::append") {
    var s1 = std.string("0123")
    let s2 = std.string("abc")
    s1.append(s2)
    expectEqual(s1, std.string("0123abc"))
}

StdStringTestSuite.test("std::u16string::append") {
    var s1 = std.u16string("0123")
    let s2 = std.u16string("abc")
    s1.append(s2)
    expectEqual(s1, std.u16string("0123abc"))
}

StdStringTestSuite.test("std::u32string::append") {
    var s1 = std.u32string("0123")
    let s2 = std.u32string("abc")
    s1.append(s2)
    expectEqual(s1, std.u32string("0123abc"))
}

StdStringTestSuite.test("std::string as Hashable") {
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

StdStringTestSuite.test("std::u16string as Hashable") {
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

StdStringTestSuite.test("std::u32string as Hashable") {
    let s0 = std.u32string()
    let h0 = s0.hashValue

    let s1 = std.u32string("something")
    let h1 = s1.hashValue

    let s2 = std.u32string("something123")
    let h2 = s2.hashValue

    let s3 = std.u32string("something")
    let h3 = s3.hashValue

    expectEqual(h1, h3)
    expectNotEqual(h0, h1)
    expectNotEqual(h0, h2)
    expectNotEqual(h0, h3)
    expectNotEqual(h1, h2)
    expectNotEqual(h2, h3)
}

StdStringTestSuite.test("std::u16string <=> Swift.String") {
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

StdStringTestSuite.test("std::u32string <=> Swift.String") {
    let cxx1 = std.u32string()
    let swift1 = String(cxx1)
    expectEqual(swift1, "")

    let cxx2 = std.u32string("something123")
    expectEqual(cxx2.size(), 12)
    let swift2 = String(cxx2)
    expectEqual(swift2, "something123")

    let cxx3: std.u32string = "literal"
    expectEqual(cxx3.size(), 7)

    let cxx4: std.u32string = "тест"
    expectEqual(cxx4.size(), 4)
    let swift4 = String(cxx4)
    expectEqual(swift4, "тест")

    // Emojis are represented by more than one CWideChar.
    let cxx5: std.u32string = "emoji_🤖"
    expectEqual(cxx5.size(), 7)
    let swift5 = String(cxx5)
    expectEqual(swift5, "emoji_🤖")

    let cxx6 = std.u32string("xyz\0abc")
    expectEqual(cxx6.size(), 7)
    let swift6 = String(cxx6)
    expectEqual(swift6, "xyz\0abc")
}

StdStringTestSuite.test("std::string as Swift.CustomDebugStringConvertible") {
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

StdStringTestSuite.test("std::u16string as Swift.CustomDebugStringConvertible") {
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

StdStringTestSuite.test("std::u32string as Swift.CustomDebugStringConvertible") {
    let cxx1 = std.u32string()
    expectEqual(cxx1.debugDescription, "std.u32string()")

    let cxx2 = std.u32string("something123")
    expectEqual(cxx2.debugDescription, "std.u32string(something123)")

    // Since std::u32string does not support pushing back UInt32 directly, we utilize UInt16 instead.
    let scalars: [UInt16] = [97, 55296, 99]
    var cxx3_16 = std.u16string()
    for scalar: UInt16 in scalars {
        cxx3_16.push_back(scalar)
    }
    let cxx3 = std.u32string(String(cxx3_16))
    expectEqual(cxx3.debugDescription, "std.u32string(a�c)")
}

StdStringTestSuite.test("std::string as Swift.Sequence") {
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

StdStringTestSuite.test("std::string as CustomStringConvertible") {
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

StdStringTestSuite.test("std::u16string as Swift.CustomStringConvertible") {
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

StdStringTestSuite.test("std::u32string as Swift.CustomStringConvertible") {
    let cxx1 = std.u32string()
    expectEqual(cxx1.description, "")

    let cxx2 = std.u32string("something123")
    expectEqual(cxx2.description, "something123")

    // Since std::u32string does not support pushing back UInt32 directly, we utilize UInt16 instead.
    let scalars: [UInt16] = [97, 55296, 99]
    var cxx3_16 = std.u16string()
    for scalar: UInt16 in scalars {
        cxx3_16.push_back(scalar)
    }
    let cxx3 = std.u32string(String(cxx3_16))
    expectEqual(cxx3.description, "a�c")

    // For `push_back`
    let scalars2: [Unicode.Scalar] = [0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x2C, 0x20, 0x4E16, 0x754C]
      .compactMap { Unicode.Scalar($0) }
    var cxx4 = std.u32string()
    for scalar: Unicode.Scalar in scalars2 {
        cxx4.push_back(scalar)
    }
    expectEqual(cxx4.description, "Hello, 世界")
}

StdStringTestSuite.test("std::string from C string") {
    let str = "abc".withCString { ptr in
        std.string(ptr)
    }
    expectEqual(str, std.string("abc"))
}


StdStringTestSuite.test("std::string to UTF-8") {
    std.string().withUTF8 { ptr in
        expectEqual(ptr.count, 0)
    }
    std.string("abc").withUTF8 { ptr in
        expectEqual(ptr.count, 3)
        expectEqual(ptr.baseAddress?.pointee, 97)
        expectEqual(ptr.baseAddress?.successor().pointee, 98)
        expectEqual(ptr.baseAddress?.successor().successor().pointee, 99)
    }

    let bytes: [UInt8] = [0xE1, 0xC1, 0xAC]
    var str = std.string()
    for byte in bytes {
        str.push_back(CChar(bitPattern: byte))
    }
    str.withUTF8 { ptr in
        expectEqual(ptr.count, 3)
        expectEqual(ptr.baseAddress?.pointee, 0xE1)
        expectEqual(ptr.baseAddress?.successor().pointee, 0xC1)
        expectEqual(ptr.baseAddress?.successor().successor().pointee, 0xAC)
    }
}

#if USE_CUSTOM_STRING_API
StdStringTestSuite.test("get from a method") {
    let box = HasMethodThatReturnsString()
    let str = box.getString()
    expectEqual(str.size(), 3)
    expectEqual(str, std.string("111"))
}

StdStringTestSuite.test("pass as an argument") {
    let s = std.string("a")
    let res = takesStringWithDefaultArg(s)
    expectEqual(res.size(), 1)
    expectEqual(res[0], 97)
}

StdStringTestSuite.test("pass as a default argument") {
    let res = takesStringWithDefaultArg()
    expectEqual(res.size(), 3)
    expectEqual(res[0], 97)
    expectEqual(res[1], 98)
    expectEqual(res[2], 99)
}
#endif

runAllTests()

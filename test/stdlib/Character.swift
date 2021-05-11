// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

import StdlibUnittest
import Swift
import SwiftPrivate


//===---
// Utilities.
//===---

// Single Unicode scalars that occupy a variety of bits in UTF-8.
//
// These scalars should be "base characters" with regards to their position in
// a grapheme cluster.
let baseScalars: [UnicodeScalar] = [
  // U+0065 LATIN SMALL LETTER E
  "\u{0065}",

  // U+006F LATIN SMALL LETTER O
  "\u{006f}",

  // U+00A9 COPYRIGHT SIGN
  "\u{00a9}",

  // U+0122 LATIN CAPITAL LETTER G WITH CEDILLA
  "\u{0122}",

  // U+0521 CYRILLIC SMALL LETTER EL WITH MIDDLE HOOK
  "\u{0521}",

  // U+0ED2 LAO DIGIT TWO
  "\u{0ed2}",

  // U+4587 CJK UNIFIED IDEOGRAPH-4587
  "\u{4587}",

  // U+B977 HANGUL SYLLABLE REUGS
  "\u{b977}",

  // U+BF01 HANGUL SYLLABLE BBENG
  "\u{bf01}",

  // U+1D452 MATHEMATICAL ITALIC SMALL E
  "\u{1d452}",

  // U+1E825 MENDE KIKAKUI SYLLABLE M163 EE
  "\u{1e825}",

  // U+10B9C4 (private use)
  "\u{10b9c4}",
]

// Single Unicode scalars that are "continuing characters" with regards to
// their position in a grapheme cluster.
let continuingScalars: [UnicodeScalar] = [
  // U+0300 COMBINING GRAVE ACCENT
  "\u{0300}",

  // U+0308 COMBINING DIAERESIS
  "\u{0308}",

  // U+0903 DEVANAGARI SIGN VISARGA
  "\u{0903}",

  // U+200D ZERO WIDTH JOINER
  "\u{200D}",
]

var testCharacters = [
  // U+000D CARRIAGE RETURN (CR)
  // U+000A LINE FEED (LF)
  "\u{000d}\u{000a}",

  // Grapheme clusters that have UTF-8 representations of length 1..10 bytes.

  // U+0061 LATIN SMALL LETTER A
  // U+0300 COMBINING GRAVE ACCENT
  "\u{0061}", // UTF-8: 1 byte
  "\u{0061}\u{0300}", // UTF-8: 3 bytes
  "\u{0061}\u{0300}\u{0300}", // UTF-8: 5 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}", // UTF-8: 7 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 9 bytes
]

// Only run it on ObjC platforms. Supported Linux versions do not have a
// recent enough ICU
#if _runtime(_ObjC)
testCharacters += [
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 11 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 13 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 15 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 17 bytes
]
#endif

testCharacters += [
  // U+00A9 COPYRIGHT SIGN
  // U+0300 COMBINING GRAVE ACCENT
  "\u{00a9}", // UTF-8: 2 bytes
  "\u{00a9}\u{0300}", // UTF-8: 4 bytes
  "\u{00a9}\u{0300}\u{0300}", // UTF-8: 6 bytes
  "\u{00a9}\u{0300}\u{0300}\u{0300}", // UTF-8: 8 bytes
  "\u{00a9}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 10 bytes
]

// Only run it on recent enough versions of ICU
#if _runtime(_ObjC)
if #available(iOS 11.0, macOS 10.13, tvOS 11.0, watchOS 4.0, *) {
  testCharacters += [
    "\u{00a9}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 12 bytes
    "\u{00a9}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 14 bytes
    "\u{00a9}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 16 bytes

    "👩🏽‍💼", // UTF-8: 15 bytes
    "👩‍👩‍👦‍👦", // UTF-8: 25 bytes
  ]
}
#endif

func randomGraphemeCluster(_ minSize: Int, _ maxSize: Int) -> String {
  let n = Int.random(in: (minSize + 1) ..< maxSize)
  var result = String(baseScalars.randomElement()!)
  for _ in 0..<n {
    result += String(continuingScalars.randomElement()!)
  }
  return result
}

//===---
// Tests.
//===---

var CharacterTests = TestSuite("Character")

CharacterTests.test("literal") {
  do {
    // U+0041 LATIN CAPITAL LETTER A
    let ch: Character = "A"
    expectEqual("\u{0041}", String(ch))
  }

  do {
    // U+3042 HIRAGANA LETTER A
    let ch: Character = "あ"
    expectEqual("\u{3042}", String(ch))
  }

  do {
    // U+4F8B CJK UNIFIED IDEOGRAPH-4F8B
    let ch: Character = "例"
    expectEqual("\u{4F8B}", String(ch))
  }

  do {
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let ch: Character = "\u{304b}\u{3099}"
    expectEqual("\u{304b}\u{3099}", String(ch))
  }
}

CharacterTests.test("sizeof") {
  // FIXME(ABI)#164: should be 8.
  // <rdar://problem/16754935> MemoryLayout<Character>.size is 9, should be 8

  let size1 = MemoryLayout<Character>.size
  expectTrue(size1 == MemoryLayout<String>.size)

  let a: Character = "a"
  let size2 = MemoryLayout.size(ofValue: a)
  expectEqual(size1, size2)
}

CharacterTests.test("Hashable") {
  for characters in [
    baseScalars.map { String($0) },
    continuingScalars.map { String($0) },
    testCharacters
  ] {
    checkHashable(characters, equalityOracle: { $0 == $1 })
  }
}

CharacterTests.test("CR-LF") {
  let asciiString = "qwerty\r\n"
  let asciiString_rev = "\r\nytrewq"
  expectEqual(asciiString.count, asciiString_rev.count)
  expectEqualSequence(asciiString.reversed(), asciiString_rev)

  // Mixed form
  let utf16String = "a\u{03B2}c\r\nd\u{03B5}f"
  let utf16String_rev = "f\u{03B5}d\r\nc\u{03B2}a"
  expectEqual(utf16String.count, utf16String_rev.count)
  expectEqualSequence(utf16String.reversed(), utf16String_rev)

  // Substrings
  let asciiString_sub = asciiString[asciiString.index(after: asciiString.startIndex)..<asciiString.endIndex]
  let asciiString_rev_sub = asciiString_rev[asciiString_rev.startIndex..<asciiString_rev.index(before:asciiString_rev.endIndex)]
  expectEqual(asciiString_sub.count, asciiString_rev_sub.count)
  expectEqual(asciiString_sub.count, asciiString.count-1)
  expectEqualSequence(asciiString_sub.reversed(), asciiString_rev_sub)

  let utf16String_sub = utf16String[utf16String.index(after: utf16String.startIndex)..<utf16String.endIndex]
  let utf16String_rev_sub = utf16String_rev[utf16String_rev.startIndex..<utf16String_rev.index(before: utf16String_rev.endIndex)]
  expectEqual(utf16String_sub.count, utf16String_rev_sub.count)
  expectEqual(utf16String_sub.count, utf16String.count-1)
  expectEqualSequence(utf16String_sub.reversed(), utf16String_rev_sub)

  // Character view slices where the indices are invalid as subsequence-relative offsets
  let asciiString_final = "ty\r\n"
  let asciiString_final_rev = "\r\nyt"
  let finalASCIICharacters = asciiString[asciiString.index(asciiString.endIndex, offsetBy: -3)..<asciiString.endIndex]
  expectEqualSequence(finalASCIICharacters, asciiString_final)
  expectEqualSequence(finalASCIICharacters.reversed(), asciiString_final_rev)

  let unicodeAlphabetString = "abcdefgあいうえおαβγ\r\n"
  let unicodeAlphabetString_final = "βγ\r\n"
  let unicodeAlphabetString_final_rev = "\r\nγβ"
  let finalAlphaCharacters = unicodeAlphabetString[unicodeAlphabetString.index(unicodeAlphabetString.endIndex, offsetBy: -3)..<unicodeAlphabetString.endIndex]
  expectEqualSequence(finalAlphaCharacters, unicodeAlphabetString_final)
  expectEqualSequence(finalAlphaCharacters.reversed(), unicodeAlphabetString_final_rev)
}

CharacterTests.test("Unicode 9 grapheme breaking") {
  // Only run it on ObjC platforms. Supported Linux versions do not have a
  // recent enough ICU for Unicode 9 support.
#if _runtime(_ObjC)
  // Check for Unicode 9 or later
  guard #available(iOS 10.0, macOS 10.12, *) else { return }

  let flags = "🇺🇸🇨🇦🇩🇰🏳️‍🌈"
  expectEqual(4, flags.count)
  expectEqual(flags.reversed().count, flags.count)

  let family = "👪👨‍👧‍👧👩‍👩‍👧‍👦👨‍👨‍👦‍👦👨‍👧👩‍👦‍👦"
  expectEqual(6, family.count)
  expectEqual(family.reversed().count, family.count)

  let skinTone = "👋👋🏻👋🏼👋🏽👋🏾👋🏿"
  expectEqual(6, skinTone.count)
  expectEqual(skinTone.reversed().count, skinTone.count)
#endif
}

/// Test that a given `String` can be transformed into a `Character` and back
/// without loss of information.
func checkRoundTripThroughCharacter(_ s: String) {
  let c = Character(s)
  var s2 = String(c)
  expectEqual(
    Array(s.unicodeScalars), Array(s2.unicodeScalars),
    "round-tripping error: \"\(s)\" != \"\(s2)\""
  )
}

func isSmallRepresentation(_ s: String) -> Bool {
  return Character(s)._isSmall
}

func checkUnicodeScalars(_ s: String) {
  let c = s.first!
  expectEqualSequence(s.unicodeScalars, c.unicodeScalars)

  expectEqualSequence(
    s.unicodeScalars, c.unicodeScalars.indices.map { c.unicodeScalars[$0] })

  expectEqualSequence(
    s.unicodeScalars.reversed(), c.unicodeScalars.reversed())

  expectEqualSequence(
    s.unicodeScalars.reversed(), c.unicodeScalars.indices.reversed().map {
      c.unicodeScalars[$0]
    })
}

func checkRepresentation(_ s: String) {
  let expectSmall = s.utf8.count <= _SmallString.capacity
  let isSmall = isSmallRepresentation(s)

  let expectedSize = expectSmall ? "small" : "large"
  expectEqual(
    expectSmall, isSmall,
    "expected \"\(s)\" to use the \(expectedSize) representation")
}

CharacterTests.test("RoundTripping") {
  // Single Unicode Scalar Value tests
  for s in baseScalars.lazy.map(String.init) {
    checkUnicodeScalars(s)
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }

  // Edge case tests
  for s in testCharacters {
    checkUnicodeScalars(s)
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }
}

CharacterTests.test("RoundTripping/Random") {
  // Random tests
  for _ in 0..<500 {
    // Character's small representation variant has 63 bits. Making
    // the maximum length 9 scalars tests both sides of the limit.
    let s = randomGraphemeCluster(1, 9)
    checkUnicodeScalars(s)
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }
}

CharacterTests.test("forall x: ASCII . String(Character(x)) == String(x)") {
  // For all ASCII chars, constructing a Character then a String should be the
  // same as constructing a String directly.
  let asciiDomain = (0..<128).map({ UnicodeScalar(Int($0))! })
  expectEqualFunctionsForDomain(asciiDomain,
    { String($0) },
    { String(Character($0)) })
}

CharacterTests.test(
  "forall x: (0..ASCII_MAX-1) . String(x) < String(y+1) == String(Character(x)) < String(Character(y+1))") {
  // For all ASCII chars, constructing a Character then a String should ordered
  // the same as constructing a String directly.
  let asciiDomain = Array(0..<127)
  let ascii0to126 = asciiDomain.map({ UnicodeScalar(Int($0))! })
  let ascii1to127 = asciiDomain.map({ UnicodeScalar(Int($0 + 1))! })
  expectEqualMethodsForDomain(
    ascii0to126,
    ascii1to127,
    { x in { String(x) < String($0) } },
    { x in { String(Character(x)) < String(Character($0)) } })
}

CharacterTests.test("String.append(_: Character)") {
  for test in testCharacters {
    let character = Character(test)
    var result = ""
    result.append(character)
    expectEqualSequence(
      test.unicodeScalars,
      result.unicodeScalars)
  }
}

CharacterTests.test("utf6/16/unicodescalar views") {
  for c in testCharacters {
    expectEqualSequence(String(c).unicodeScalars, c.unicodeScalars)
    expectEqualSequence(String(c).utf8, c.utf8)
    expectEqualSequence(String(c).utf16, c.utf16)

    expectEqualSequence(
      String(c).unicodeScalars.reversed(), c.unicodeScalars.reversed())
    expectEqualSequence(String(c).utf8.reversed(), c.utf8.reversed())
    expectEqualSequence(String(c).utf16.reversed(), c.utf16.reversed())
  }
}

var UnicodeScalarTests = TestSuite("UnicodeScalar")

UnicodeScalarTests.test("UInt8(ascii: UnicodeScalar)") {
  for i in 0..<0x7f {
    let us = UnicodeScalar(i)!
    expectEqual(UInt8(i), UInt8(ascii: us))
  }
}

#if !os(WASI)
UnicodeScalarTests.test("UInt8(ascii: UnicodeScalar)/non-ASCII should trap")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  let us: UnicodeScalar = "\u{E5}"
  expectCrashLater()
  _blackHole(UInt8(ascii: us))
}
#endif

UnicodeScalarTests.test("UInt32(_: UnicodeScalar),UInt64(_: UnicodeScalar)") {
  for us in baseScalars {
    expectEqual(us.value, UInt32(us))
    expectEqual(UInt64(us.value), UInt64(us))
  }
}

UnicodeScalarTests.test("isASCII()") {
  expectTrue(UnicodeScalar(0)!.isASCII)
  expectTrue(("A" as UnicodeScalar).isASCII)
  expectTrue(UnicodeScalar(127)!.isASCII)
  expectFalse(UnicodeScalar(128)!.isASCII)
  expectFalse(UnicodeScalar(256)!.isASCII)
}

UnicodeScalarTests.test("Comparable") {
  // FIXME: these tests are insufficient.

  let CharA: UnicodeScalar = "A"

  expectTrue(CharA == "A")
  expectTrue("A" == CharA)
  expectTrue(CharA != "B")
  expectTrue("B" != CharA)

  expectTrue(CharA < "B")
  expectTrue("B" > CharA)
  expectTrue(CharA <= "B")
  expectTrue("B" >= CharA)
}

UnicodeScalarTests.test("LosslessStringConvertible") {
  // FIXME: these tests are insufficient.

  checkLosslessStringConvertible((0xE000...0xF000).map { UnicodeScalar(Int($0))! })
  checkLosslessStringConvertible((0...127).map { UnicodeScalar(Int($0))! })
}

if #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) {
  UnicodeScalarTests.test("Views") {
    let scalars = baseScalars + continuingScalars
    for scalar in scalars {
      expectEqual(scalar, String(scalar).unicodeScalars.first!)
      expectEqualSequence(String(scalar).utf8, scalar.utf8)
      expectEqualSequence(String(scalar).utf16, scalar.utf16)

      expectEqualSequence(String(scalar).utf8.reversed(), scalar.utf8.reversed())
      expectEqualSequence(
        String(scalar).utf16.reversed(), scalar.utf16.reversed())
    }
  }
}

runAllTests()


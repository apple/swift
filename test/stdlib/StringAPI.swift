// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: reflection

//
// Tests for the non-Foundation API of String
//

import StdlibUnittest

#if _runtime(_ObjC)
import Foundation
#endif


var StringTests = TestSuite("StringTests")

struct ComparisonTest {
  let expectedUnicodeCollation: ExpectedComparisonResult
  let lhs: String
  let rhs: String
  let loc: SourceLoc
  let xfail: TestRunPredicate

  init(
    _ expectedUnicodeCollation: ExpectedComparisonResult,
    _ lhs: String, _ rhs: String,
    xfail: TestRunPredicate = .never,
    file: String = #file, line: UInt = #line
  ) {
    self.expectedUnicodeCollation = expectedUnicodeCollation
    self.lhs = lhs
    self.rhs = rhs
    self.loc = SourceLoc(file, line, comment: "test data")
    self.xfail = xfail
  }

  func replacingPredicate(_ xfail: TestRunPredicate) -> ComparisonTest {
    return ComparisonTest(expectedUnicodeCollation, lhs, rhs,
      xfail: xfail, file: loc.file, line: loc.line)
  }
}

// List test cases for comparisons and prefix/suffix. Ideally none fail.

let tests = [
  ComparisonTest(.eq, "", ""),
  ComparisonTest(.lt, "", "a"),

  // ASCII cases
  ComparisonTest(.lt, "t", "tt"),
  ComparisonTest(.gt, "t", "Tt"),
  ComparisonTest(.gt, "\u{0}", ""),
  ComparisonTest(.eq, "\u{0}", "\u{0}"),

  ComparisonTest(.lt, "\r\n", "t"),
  ComparisonTest(.gt, "\r\n", "\n"),
  ComparisonTest(.lt, "\u{0}", "\u{0}\u{0}"),

  // Whitespace
  // U+000A LINE FEED (LF)
  // U+000B LINE TABULATION
  // U+000C FORM FEED (FF)
  // U+0085 NEXT LINE (NEL)
  // U+2028 LINE SEPARATOR
  // U+2029 PARAGRAPH SEPARATOR
  ComparisonTest(.gt, "\u{0085}", "\n"),
  ComparisonTest(.gt, "\u{000b}", "\n"),
  ComparisonTest(.gt, "\u{000c}", "\n"),
  ComparisonTest(.gt, "\u{2028}", "\n"),
  ComparisonTest(.gt, "\u{2029}", "\n"),
  ComparisonTest(.gt, "\r\n\r\n", "\r\n"),

  // U+0301 COMBINING ACUTE ACCENT
  // U+00E1 LATIN SMALL LETTER A WITH ACUTE
  ComparisonTest(.eq, "a\u{301}", "\u{e1}"),
  ComparisonTest(.lt, "a", "a\u{301}"),
  ComparisonTest(.lt, "a", "\u{e1}"),

  // U+304B HIRAGANA LETTER KA
  // U+304C HIRAGANA LETTER GA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  ComparisonTest(.eq, "\u{304b}", "\u{304b}"),
  ComparisonTest(.eq, "\u{304c}", "\u{304c}"),
  ComparisonTest(.lt, "\u{304b}", "\u{304c}"),
  ComparisonTest(.lt, "\u{304b}", "\u{304c}\u{3099}"),
  ComparisonTest(.eq, "\u{304c}", "\u{304b}\u{3099}"),
  ComparisonTest(.lt, "\u{304c}", "\u{304c}\u{3099}"),

  // U+212B ANGSTROM SIGN
  // U+030A COMBINING RING ABOVE
  // U+00C5 LATIN CAPITAL LETTER A WITH RING ABOVE
  ComparisonTest(.eq, "\u{212b}", "A\u{30a}"),
  ComparisonTest(.eq, "\u{212b}", "\u{c5}"),
  ComparisonTest(.eq, "A\u{30a}", "\u{c5}"),
  ComparisonTest(.gt, "A\u{30a}", "a"),
  ComparisonTest(.lt, "A", "A\u{30a}"),

  // U+2126 OHM SIGN
  // U+03A9 GREEK CAPITAL LETTER OMEGA
  ComparisonTest(.eq, "\u{2126}", "\u{03a9}"),

  // U+0323 COMBINING DOT BELOW
  // U+0307 COMBINING DOT ABOVE
  // U+1E63 LATIN SMALL LETTER S WITH DOT BELOW
  // U+1E69 LATIN SMALL LETTER S WITH DOT BELOW AND DOT ABOVE
  ComparisonTest(.eq, "\u{1e69}", "s\u{323}\u{307}"),
  ComparisonTest(.eq, "\u{1e69}", "s\u{307}\u{323}"),
  ComparisonTest(.eq, "\u{1e69}", "\u{1e63}\u{307}"),
  ComparisonTest(.eq, "\u{1e63}", "s\u{323}"),
  ComparisonTest(.eq, "\u{1e63}\u{307}", "s\u{323}\u{307}"),
  ComparisonTest(.eq, "\u{1e63}\u{307}", "s\u{307}\u{323}"),
  ComparisonTest(.lt, "s\u{323}", "\u{1e69}"),

  // U+FB01 LATIN SMALL LIGATURE FI
  ComparisonTest(.eq, "\u{fb01}", "\u{fb01}"),
  ComparisonTest(.lt, "fi", "\u{fb01}"),

  // U+1F1E7 REGIONAL INDICATOR SYMBOL LETTER B
  // \u{1F1E7}\u{1F1E7} Flag of Barbados
  ComparisonTest(.lt, "\u{1F1E7}", "\u{1F1E7}\u{1F1E7}"),

  // Test that Unicode collation is performed in deterministic mode.
  //
  // U+0301 COMBINING ACUTE ACCENT
  // U+0341 COMBINING ACUTE TONE MARK
  // U+0954 DEVANAGARI ACUTE ACCENT
  //
  // Collation elements from DUCET:
  // 0301  ; [.0000.0024.0002] # COMBINING ACUTE ACCENT
  // 0341  ; [.0000.0024.0002] # COMBINING ACUTE TONE MARK
  // 0954  ; [.0000.0024.0002] # DEVANAGARI ACUTE ACCENT
  //
  // U+0301 and U+0954 don't decompose in the canonical decomposition mapping.
  // U+0341 has a canonical decomposition mapping of U+0301.
  ComparisonTest(.eq, "\u{0301}", "\u{0341}"),
  ComparisonTest(.lt, "\u{0301}", "\u{0954}"),
  ComparisonTest(.lt, "\u{0341}", "\u{0954}"),

  // (U+212A KELVIN SIGN) normalizes to ASCII "K"
  ComparisonTest(.eq, "K", "\u{212A}"),
]

func checkStringComparison(
  _ expected: ExpectedComparisonResult,
  _ lhs: String, _ rhs: String, _ stackTrace: SourceLocStack
) {
  // String / String
  expectEqual(expected.isEQ(), lhs == rhs, stackTrace: stackTrace)
  expectEqual(expected.isNE(), lhs != rhs, stackTrace: stackTrace)
  checkHashable(
    expectedEqual: expected.isEQ(),
    lhs, rhs, stackTrace: stackTrace.withCurrentLoc())

  expectEqual(expected.isLT(), lhs < rhs, stackTrace: stackTrace)
  expectEqual(expected.isLE(), lhs <= rhs, stackTrace: stackTrace)
  expectEqual(expected.isGE(), lhs >= rhs, stackTrace: stackTrace)
  expectEqual(expected.isGT(), lhs > rhs, stackTrace: stackTrace)
  checkComparable(expected, lhs, rhs, stackTrace: stackTrace.withCurrentLoc())
  
  // Substring / Substring
  // Matching slices of != Strings may still be ==, but not vice versa
  if expected.isEQ() {
    for i in 0 ..< Swift.min(lhs.count, rhs.count) {
      let lhsSub = lhs.dropFirst(i)
      let rhsSub = rhs.dropFirst(i)
      
      expectEqual(expected.isEQ(), lhsSub == rhsSub, stackTrace: stackTrace)
      expectEqual(expected.isNE(), lhsSub != rhsSub, stackTrace: stackTrace)
      checkHashable(
        expectedEqual: expected.isEQ(),
        lhs, rhs, stackTrace: stackTrace.withCurrentLoc())
      
      expectEqual(expected.isLT(), lhsSub < rhsSub, stackTrace: stackTrace)
      expectEqual(expected.isLE(), lhsSub <= rhsSub, stackTrace: stackTrace)
      expectEqual(expected.isGE(), lhsSub >= rhsSub, stackTrace: stackTrace)
      expectEqual(expected.isGT(), lhsSub > rhsSub, stackTrace: stackTrace)
      checkComparable(
        expected, lhsSub, rhsSub, stackTrace: stackTrace.withCurrentLoc())
    }
  }

#if _runtime(_ObjC)
  // NSString / NSString
  let lhsNSString = lhs as NSString
  let rhsNSString = rhs as NSString
  let expectedEqualUnicodeScalars =
    Array(lhs.unicodeScalars) == Array(rhs.unicodeScalars)
  // FIXME: Swift String and NSString comparison may not be equal.
  expectEqual(
    expectedEqualUnicodeScalars, lhsNSString == rhsNSString,
    stackTrace: stackTrace)
  expectEqual(
    !expectedEqualUnicodeScalars, lhsNSString != rhsNSString,
    stackTrace: stackTrace)
  checkHashable(
    expectedEqual: expectedEqualUnicodeScalars,
    lhsNSString, rhsNSString,
    stackTrace: stackTrace.withCurrentLoc())
#endif
}

// Mark the test cases that are expected to fail in checkStringComparison

let comparisonTests = tests

for test in comparisonTests {
  StringTests.test("String.{Equatable,Hashable,Comparable}: line \(test.loc.line)")
  .xfail(test.xfail)
  .code {
    checkStringComparison(
      test.expectedUnicodeCollation, test.lhs, test.rhs,
      test.loc.withCurrentLoc())
    checkStringComparison(
      test.expectedUnicodeCollation.flip(), test.rhs, test.lhs,
      test.loc.withCurrentLoc())
  }
}

func checkCharacterComparison(
  _ expected: ExpectedComparisonResult,
  _ lhs: Character, _ rhs: Character, _ stackTrace: SourceLocStack
) {
  // Character / Character
  expectEqual(expected.isEQ(), lhs == rhs, stackTrace: stackTrace)
  expectEqual(expected.isNE(), lhs != rhs, stackTrace: stackTrace)
  checkHashable(
    expectedEqual: expected.isEQ(),
    lhs, rhs, stackTrace: stackTrace.withCurrentLoc())

  expectEqual(expected.isLT(), lhs < rhs, stackTrace: stackTrace)
  expectEqual(expected.isLE(), lhs <= rhs, stackTrace: stackTrace)
  expectEqual(expected.isGE(), lhs >= rhs, stackTrace: stackTrace)
  expectEqual(expected.isGT(), lhs > rhs, stackTrace: stackTrace)
  checkComparable(expected, lhs, rhs, stackTrace: stackTrace.withCurrentLoc())
}

for test in comparisonTests {
  if test.lhs.count == 1 && test.rhs.count == 1 {
    StringTests.test("Character.{Equatable,Hashable,Comparable}: line \(test.loc.line)")
    .xfail(test.xfail)
    .code {
      let lhsCharacter = Character(test.lhs)
      let rhsCharacter = Character(test.rhs)
      checkCharacterComparison(
        test.expectedUnicodeCollation, lhsCharacter, rhsCharacter,
        test.loc.withCurrentLoc())
      checkCharacterComparison(
        test.expectedUnicodeCollation.flip(), rhsCharacter, lhsCharacter,
        test.loc.withCurrentLoc())
    }
  }
}

func checkHasPrefixHasSuffix(
  _ lhs: String, _ rhs: String, _ stackTrace: SourceLocStack
) {
#if _runtime(_ObjC)
  if rhs == "" {
    expectTrue(lhs.hasPrefix(rhs), stackTrace: stackTrace)
    expectTrue(lhs.hasSuffix(rhs), stackTrace: stackTrace)
    return
  }
  if lhs == "" {
    expectFalse(lhs.hasPrefix(rhs), stackTrace: stackTrace)
    expectFalse(lhs.hasSuffix(rhs), stackTrace: stackTrace)
    return
  }

  // To determine the expected results, compare grapheme clusters,
  // scalar-to-scalar, of the NFD form of the strings.
  let lhsNFDGraphemeClusters =
    lhs.decomposedStringWithCanonicalMapping.map {
      Array(String($0).unicodeScalars)
    }
  let rhsNFDGraphemeClusters =
    rhs.decomposedStringWithCanonicalMapping.map {
      Array(String($0).unicodeScalars)
    }
  let expectHasPrefix = lhsNFDGraphemeClusters.starts(
    with: rhsNFDGraphemeClusters, by: (==))

  let expectHasSuffix = lhsNFDGraphemeClusters.lazy.reversed()
    .starts(with: rhsNFDGraphemeClusters.lazy.reversed(), by: (==))

  expectEqual(expectHasPrefix, lhs.hasPrefix(rhs), stackTrace: stackTrace)
  expectEqual(expectHasSuffix, lhs.hasSuffix(rhs), stackTrace: stackTrace)
#endif
}

StringTests.test("LosslessStringConvertible") {
  checkLosslessStringConvertible(comparisonTests.map { $0.lhs })
  checkLosslessStringConvertible(comparisonTests.map { $0.rhs })
}

// Mark the test cases that are expected to fail in checkHasPrefixHasSuffix

let substringTests = tests.map {
  (test: ComparisonTest) -> ComparisonTest in
  switch (test.expectedUnicodeCollation, test.lhs, test.rhs) {

  case (.gt, "\r\n", "\n"):
    return test.replacingPredicate(.objCRuntime(
      "blocked on rdar://problem/19036555"))

  default:
    return test
  }
}

for test in substringTests {
  StringTests.test("hasPrefix,hasSuffix: line \(test.loc.line)")
    .skip(.nativeRuntime(
        "String.has{Prefix,Suffix} defined when _runtime(_ObjC)"))
    .xfail(test.xfail)
    .code {
    checkHasPrefixHasSuffix(test.lhs, test.rhs, test.loc.withCurrentLoc())
    checkHasPrefixHasSuffix(test.rhs, test.lhs, test.loc.withCurrentLoc())

    let fragment = "abc"
    let combiner = "\u{0301}" // combining acute accent

    checkHasPrefixHasSuffix(test.lhs + fragment, test.rhs, test.loc.withCurrentLoc())
    checkHasPrefixHasSuffix(fragment + test.lhs, test.rhs, test.loc.withCurrentLoc())
    checkHasPrefixHasSuffix(test.lhs + combiner, test.rhs, test.loc.withCurrentLoc())
    checkHasPrefixHasSuffix(combiner + test.lhs, test.rhs, test.loc.withCurrentLoc())
  }
}

StringTests.test("SameTypeComparisons") {
  // U+0323 COMBINING DOT BELOW
  // U+0307 COMBINING DOT ABOVE
  // U+1E63 LATIN SMALL LETTER S WITH DOT BELOW
  let xs = "\u{1e69}"
  expectTrue(xs == "s\u{323}\u{307}")
  expectFalse(xs != "s\u{323}\u{307}")
  expectTrue("s\u{323}\u{307}" == xs)
  expectFalse("s\u{323}\u{307}" != xs)
  expectTrue("\u{1e69}" == "s\u{323}\u{307}")
  expectFalse("\u{1e69}" != "s\u{323}\u{307}")
  expectTrue(xs == xs)
  expectFalse(xs != xs)
}

#if !os(WASI)
StringTests.test("CompareStringsWithUnpairedSurrogates")
  .xfail(
    .always("<rdar://problem/18029104> Strings referring to underlying " +
      "storage with unpaired surrogates compare unequal"))
  .code {
  let donor = "abcdef"
  let acceptor = "\u{1f601}\u{1f602}\u{1f603}"

  expectEqual("\u{fffd}\u{1f602}\u{fffd}",
    acceptor[
      donor.index(donor.startIndex, offsetBy: 1) ..<
      donor.index(donor.startIndex, offsetBy: 5)
    ]
  )
}
#endif

StringTests.test("[String].joined() -> String") {
  let s = ["hello", "world"].joined()
  _ = s == "" // should compile without error
}

StringTests.test("UnicodeScalarView.Iterator.Lifetime") {
  // https://github.com/apple/swift/issues/47975
  //
  // Tests that 'String.UnicodeScalarView.Iterator' is maintaining the lifetime
  // of an underlying 'String' buffer.
  //
  // WARNING: it is very easy to write this test so it produces false negatives
  // (i.e. passes even when the code is broken).  The array, for example, seems
  // to be a requirement.  So perturb this test with care!
  let sources = ["𝓣his 𝓘s 𝓜uch 𝓛onger 𝓣han 𝓐ny 𝓢mall 𝓢tring 𝓑uffer"]
  for s in sources {
    // Append something to s so that it creates a dynamically-allocated buffer.
    let i = (s + "X").unicodeScalars.makeIterator()
    expectEqualSequence(s.unicodeScalars, IteratorSequence(i).dropLast(),
      "Actual Contents: \(Array(IteratorSequence(i)))")
  }
}

StringTests.test("Regression/rdar-33276845") {
  // These two cases fail slightly differently when the code is broken
  // See rdar://33276845
  do {
    let s = String(repeating: "x", count: 0xffff)
    let a = Array(s.utf8)
    expectNotEqual(0, a.count)
  }
  do {
    let s = String(repeating: "x", count: 0x1_0010)
    let a = Array(s.utf8)
    expectNotEqual(0, a.count)
  }
}

StringTests.test("Regression/corelibs-foundation") {
  struct NSRange { var location, length: Int }

  func NSFakeRange(_ location: Int, _ length: Int) -> NSRange {
    return NSRange(location: location, length: length)
  }

  func substring(of _storage: String, with range: NSRange) -> String {
    let start = _storage.utf16.startIndex
    let min = _storage.utf16.index(start, offsetBy: range.location)
    let max = _storage.utf16.index(
      start, offsetBy: range.location + range.length)

    if let substr = String(_storage.utf16[min..<max]) {
      return substr
    }
    //If we come here, then the range has created unpaired surrogates on either end.
    //An unpaired surrogate is replaced by OXFFFD - the Unicode Replacement Character.
    //The CRLF ("\r\n") sequence is also treated like a surrogate pair, but its constituent
    //characters "\r" and "\n" can exist outside the pair!

    let replacementCharacter = String(describing: UnicodeScalar(0xFFFD)!)
    let CR: UInt16 = 13  //carriage return
    let LF: UInt16 = 10  //new line

    //make sure the range is of non-zero length
    guard range.length > 0 else { return "" }

    //if the range is pointing to a single unpaired surrogate
    if range.length == 1 {
      switch _storage.utf16[min] {
      case CR: return "\r"
      case LF: return "\n"
      default: return replacementCharacter
      }
    }

    //set the prefix and suffix characters
    let prefix = _storage.utf16[min] == LF ? "\n" : replacementCharacter
    let suffix = _storage.utf16[_storage.utf16.index(before: max)] == CR
      ? "\r" : replacementCharacter

    let postMin = _storage.utf16.index(after: min)

    //if the range breaks a surrogate pair at the beginning of the string
    if let substrSuffix = String(
      _storage.utf16[postMin..<max]) {
      return prefix + substrSuffix
    }

    let preMax = _storage.utf16.index(before: max)
    //if the range breaks a surrogate pair at the end of the string
    if let substrPrefix = String(_storage.utf16[min..<preMax]) {
      return substrPrefix + suffix
    }

    //the range probably breaks surrogate pairs at both the ends
    guard postMin <= preMax else { return prefix + suffix }

    let substr =  String(_storage.utf16[postMin..<preMax])!
    return prefix + substr + suffix
  }

  let trivial = "swift.org"
  expectEqual(substring(of: trivial, with: NSFakeRange(0, 5)), "swift")

  let surrogatePairSuffix = "Hurray🎉"
  expectEqual(substring(of: surrogatePairSuffix, with: NSFakeRange(0, 7)), "Hurray�")

  let surrogatePairPrefix = "🐱Cat"
  expectEqual(substring(of: surrogatePairPrefix, with: NSFakeRange(1, 4)), "�Cat")

  let singleChar = "😹"
  expectEqual(substring(of: singleChar, with: NSFakeRange(0,1)), "�")

  let crlf = "\r\n"
  expectEqual(substring(of: crlf, with: NSFakeRange(0,1)), "\r")
  expectEqual(substring(of: crlf, with: NSFakeRange(1,1)), "\n")
  expectEqual(substring(of: crlf, with: NSFakeRange(1,0)), "")

  let bothEnds1 = "😺😺"
  expectEqual(substring(of: bothEnds1, with: NSFakeRange(1,2)), "��")

  let s1 = "😺\r\n"
  expectEqual(substring(of: s1, with: NSFakeRange(1,2)), "�\r")

  let s2 = "\r\n😺"
  expectEqual(substring(of: s2, with: NSFakeRange(1,2)), "\n�")

  let s3 = "😺cats😺"
  expectEqual(substring(of: s3, with: NSFakeRange(1,6)), "�cats�")

  let s4 = "😺cats\r\n"
  expectEqual(substring(of: s4, with: NSFakeRange(1,6)), "�cats\r")

  let s5 = "\r\ncats😺"
  expectEqual(substring(of: s5, with: NSFakeRange(1,6)), "\ncats�")
}

StringTests.test("Regression/radar-87371813") {
  let s1 = "what♕/".dropFirst(5)
  let s2 = "/"[...]
  let s3 = "/⚅".dropLast()
  expectEqual(s1, s2)
  expectEqual(s1, s3)
  expectEqual(s1, s3)
  expectEqual(s1.hashValue, s2.hashValue)
  expectEqual(s2.hashValue, s3.hashValue)
}

StringTests.test("_isIdentical(to:)") {
  let a = "Hello"
  let b = "Hello"
  expectTrue(a._isIdentical(to: a))
  expectTrue(b._isIdentical(to: b))
  expectTrue(a._isIdentical(to: b)) // Both small ASCII strings
  expectTrue(b._isIdentical(to: a))

  let c = "Cafe\u{301}"
  let d = "Cafe\u{301}"
  let e = "Café"
  expectTrue(c._isIdentical(to: d))
  expectTrue(d._isIdentical(to: c))
  expectFalse(c._isIdentical(to: e))
  expectFalse(d._isIdentical(to: e))

  let f = String(repeating: "foo", count: 1000)
  let g = String(repeating: "foo", count: 1000)
  expectEqual(f, g)
  expectFalse(f._isIdentical(to: g)) // Two large, distinct native strings
  expectTrue(f._isIdentical(to: f))
  expectTrue(g._isIdentical(to: g))
}

runAllTests()

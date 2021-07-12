// RUN: %target-run-simple-swift %t
// REQUIRES: executable_test

import StdlibUnittest

var UnicodeScalarPropertiesTests = TestSuite("UnicodeScalarPropertiesTests")

func us(_ scalar: Unicode.Scalar) -> Unicode.Scalar { return scalar }

UnicodeScalarPropertiesTests.test("properties.booleanProperties") {
  func expectBooleanProperty(
    _ keyPath: KeyPath<Unicode.Scalar.Properties, Bool>,
    trueFor trueScalar: Unicode.Scalar,
    falseFor falseScalar: Unicode.Scalar,
    _ message: String = "",
    file: String = #file,
    line: UInt = #line
  ) {
    expectTrue(
      trueScalar.properties[keyPath: keyPath], message, file: file, line: line)
    expectFalse(
      falseScalar.properties[keyPath: keyPath], message, file: file, line: line)
  }

  // Some sanity checks for basic properties. Not intended to be comprehensive.
  expectBooleanProperty(\.isAlphabetic, trueFor: "A", falseFor: "5")
  expectBooleanProperty(\.isASCIIHexDigit, trueFor: "F", falseFor: "G")
  // U+200E LEFT-TO-RIGHT MARK
  expectBooleanProperty(\.isBidiControl, trueFor: "\u{200E}", falseFor: "A")
  expectBooleanProperty(\.isBidiMirrored, trueFor: "(", falseFor: "A")
  expectBooleanProperty(\.isDash, trueFor: "-", falseFor: "A")
  // U+00AD SOFT HYPHEN
  expectBooleanProperty(\.isDefaultIgnorableCodePoint, trueFor: "\u{00AD}", falseFor: "A")
  // U+0149 LATIN SMALL LETTER N PRECEDED BY APOSTROPHE
  expectBooleanProperty(\.isDeprecated, trueFor: "\u{0149}", falseFor: "A")
  expectBooleanProperty(\.isDiacritic, trueFor: "^", falseFor: "A")
  // U+00B7 MIDDLE DOT
  expectBooleanProperty(\.isExtender, trueFor: "\u{00B7}", falseFor: "A")
  // U+0340 COMBINING GRAVE TONE MARK
  expectBooleanProperty(\.isFullCompositionExclusion, trueFor: "\u{0340}", falseFor: "A")
  // U+0300 COMBINING GRAVE ACCENT
  expectBooleanProperty(\.isGraphemeBase, trueFor: "A", falseFor: "\u{0300}")
  expectBooleanProperty(\.isGraphemeExtend, trueFor: "\u{0300}", falseFor: "A")
  // U+FF21 FULLWIDTH LATIN CAPITAL LETTER A
  expectBooleanProperty(\.isHexDigit, trueFor: "\u{FF21}", falseFor: "G")
  expectBooleanProperty(\.isIDContinue, trueFor: "0", falseFor: "!")
  expectBooleanProperty(\.isIDStart, trueFor: "A", falseFor: "!")
  // U+3400 CJK UNIFIED IDEOGRAPH-3400
  expectBooleanProperty(\.isIdeographic, trueFor: "\u{3400}", falseFor: "A")
  // U+2FF0 IDEOGRAPHIC DESCRIPTION CHARACTER LEFT TO RIGHT
  expectBooleanProperty(\.isIDSBinaryOperator, trueFor: "\u{2FF0}", falseFor: "A")
  // U+2FF2 IDEOGRAPHIC DESCRIPTION CHARACTER LEFT TO MIDDLE AND RIGHT
  expectBooleanProperty(\.isIDSTrinaryOperator, trueFor: "\u{2FF2}", falseFor: "A")
  // U+200C ZERO WIDTH NON-JOINER
  expectBooleanProperty(\.isJoinControl, trueFor: "\u{200C}", falseFor: "A")
  // U+0E40 THAI CHARACTER SARA E
  expectBooleanProperty(\.isLogicalOrderException, trueFor: "\u{0E40}", falseFor: "A")
  expectBooleanProperty(\.isLowercase, trueFor: "a", falseFor: "A")
  expectBooleanProperty(\.isMath, trueFor: "+", falseFor: "A")
  expectBooleanProperty(\.isNoncharacterCodePoint, trueFor: Unicode.Scalar(0xFDD0)!, falseFor: "A")
  expectBooleanProperty(\.isQuotationMark, trueFor: "'", falseFor: "A")
  // U+2E80 CJK RADICAL REPEAT
  expectBooleanProperty(\.isRadical, trueFor: "\u{2E80}", falseFor: "A")
  expectBooleanProperty(\.isSoftDotted, trueFor: "i", falseFor: "A")
  expectBooleanProperty(\.isTerminalPunctuation, trueFor: "!", falseFor: "A")
  expectBooleanProperty(\.isUnifiedIdeograph, trueFor: "\u{3400}", falseFor: "A")
  expectBooleanProperty(\.isUppercase, trueFor: "A", falseFor: "a")
  expectBooleanProperty(\.isWhitespace, trueFor: " ", falseFor: "A")
  expectBooleanProperty(\.isXIDContinue, trueFor: "0", falseFor: "!")
  expectBooleanProperty(\.isXIDStart, trueFor: "A", falseFor: "!")
  expectBooleanProperty(\.isSentenceTerminal, trueFor: "!", falseFor: "A")
  // U+FE00 VARIATION SELECTOR-1
  expectBooleanProperty(\.isVariationSelector, trueFor: "\u{FE00}", falseFor: "A")
  expectBooleanProperty(\.isPatternSyntax, trueFor: "!", falseFor: "A")
  expectBooleanProperty(\.isPatternWhitespace, trueFor: " ", falseFor: "A")
  expectBooleanProperty(\.isCased, trueFor: "A", falseFor: "!")
  expectBooleanProperty(\.isCaseIgnorable, trueFor: "'", falseFor: "A")
  expectBooleanProperty(\.changesWhenLowercased, trueFor: "A", falseFor: "a")
  expectBooleanProperty(\.changesWhenUppercased, trueFor: "a", falseFor: "A")
  expectBooleanProperty(\.changesWhenTitlecased, trueFor: "a", falseFor: "A")
  expectBooleanProperty(\.changesWhenCaseFolded, trueFor: "A", falseFor: "!")
  expectBooleanProperty(\.changesWhenCaseMapped, trueFor: "A", falseFor: "!")
  expectBooleanProperty(\.changesWhenNFKCCaseFolded, trueFor: "A", falseFor: "!")

  if #available(macOS 10.12.2, iOS 10.2, tvOS 10.1, watchOS 3.1.1, *) {
    // U+2708 AIRPLANE
    expectBooleanProperty(\.isEmoji, trueFor: "\u{2708}", falseFor: "A")
    // U+231A WATCH
    expectBooleanProperty(
      \.isEmojiPresentation, trueFor: "\u{231A}", falseFor: "A")
    // U+1F3FD EMOJI MODIFIER FITZPATRICK TYPE-4
    expectBooleanProperty(
      \.isEmojiModifier, trueFor: "\u{1F3FD}", falseFor: "A")
    // U+270B RAISED HAND
    expectBooleanProperty(
      \.isEmojiModifierBase, trueFor: "\u{270B}", falseFor: "A")
  }
}

UnicodeScalarPropertiesTests.test("properties.lowercaseMapping") {
  expectEqual("2", us("2").properties.lowercaseMapping)
  expectEqual("i", us("I").properties.lowercaseMapping)
  expectEqual("i\u{0307}", us("\u{0130}").properties.lowercaseMapping)
  // There are currently no lowercase mappings that produce multiple graphemes.
}

UnicodeScalarPropertiesTests.test("uppercaseMapping") {
  expectEqual("2", us("2").properties.uppercaseMapping)
  expectEqual("I", us("i").properties.uppercaseMapping)
  expectEqual("\u{02BC}N", us("\u{0149}").properties.uppercaseMapping)  // multiple scalars
  expectEqual("SS", us("ß").properties.uppercaseMapping)  // multiple graphemes
  expectEqual("FFL", us("\u{FB04}").properties.uppercaseMapping)  // multiple graphemes
}

UnicodeScalarPropertiesTests.test("titlecaseMapping") {
  expectEqual("2", us("2").properties.titlecaseMapping)
  expectEqual("I", us("i").properties.titlecaseMapping)
  expectEqual("\u{02BC}N", us("\u{0149}").properties.titlecaseMapping)  // multiple scalars
  expectEqual("Ff", us("\u{FB00}").properties.titlecaseMapping)  // multiple graphemes
  expectEqual("Ffl", us("\u{FB04}").properties.titlecaseMapping)  // multiple graphemes
}

UnicodeScalarPropertiesTests.test("properties.name") {
  // A scalar with no assigned name returns nil.
  expectNil(us("\u{0000}").properties.name)

  // Try some results that should fit in small strings.
  expectEqual("CARE OF", us("\u{2105}").properties.name)
  expectEqual("ACCOUNT OF", us("\u{2100}").properties.name)

  // Try some results that need heap-allocated strings.
  expectEqual("LATIN SMALL LETTER A", us("\u{0061}").properties.name)
  expectEqual(
    "COMBINING LEFTWARDS HARPOON WITH BARB DOWNWARDS",
    us("\u{20ED}").properties.name)
  expectEqual(
    "PRESENTATION FORM FOR VERTICAL RIGHT WHITE LENTICULAR BRAKCET",  // [sic]
    us("\u{FE18}").properties.name)

  // Try some boundary cases around the length limit of a SmallUTF8String.
  expectEqual("COMBINING HORN", us("\u{031B}").properties.name)  // 14
  expectEqual("COMBINING TILDE", us("\u{0303}").properties.name)  // 15
  expectEqual("COMBINING MACRON", us("\u{0304}").properties.name)  // 16
}

UnicodeScalarPropertiesTests.test("properties.nameAlias") {
  // A scalar with no assigned alias returns nil.
  expectNil(us("\u{0000}").properties.nameAlias)
  expectNil(us("\u{0040}").properties.nameAlias)

  // Try some aliases of varying lengths, getting some small and large string
  // coverage.
  expectEqual("LAO LETTER RO", us("\u{0EA3}").properties.nameAlias)
  expectEqual("MICR DASH SYMBOL", us("\u{2449}").properties.nameAlias)
  expectEqual(
    "PRESENTATION FORM FOR VERTICAL RIGHT WHITE LENTICULAR BRACKET",
    us("\u{FE18}").properties.nameAlias)
}

UnicodeScalarPropertiesTests.test("properties.age") {
  func expectAgeEqual(
    _ expected: (Int, Int),
    _ scalar: Unicode.Scalar,
    _ message: String = "",
    file: String = #file,
    line: UInt = #line
  ) {
    expectNotNil(scalar.properties.age, message, file: file, line: line)
    expectEqual(
      expected, scalar.properties.age!, message, file: file, line: line)
  }

  expectNil(us("\u{0378}").properties.age)
  expectAgeEqual((1, 1), "\u{0040}")
  expectAgeEqual((3, 0), "\u{3500}")
  expectAgeEqual((3, 2), "\u{FE00}")
  expectAgeEqual((6, 1), "\u{11180}")
}

UnicodeScalarPropertiesTests.test("properties.generalCategory") {
  expectEqual(.uppercaseLetter, us("A").properties.generalCategory)
  expectEqual(.lowercaseLetter, us("a").properties.generalCategory)
  // U+01C8 LATIN CAPITAL LETTER L WITH SMALL LETTER J
  expectEqual(.titlecaseLetter, us("\u{01C8}").properties.generalCategory)
  // U+02B0 MODIFIER LETTER SMALL H
  expectEqual(.modifierLetter, us("\u{02B0}").properties.generalCategory)
  // U+00AA FEMININE ORDINAL INDICATOR
  expectEqual(.otherLetter, us("\u{00AA}").properties.generalCategory)

  // U+0300 COMBINING GRAVE ACCENT
  expectEqual(.nonspacingMark, us("\u{0300}").properties.generalCategory)
  // U+20DD COMBINING ENCLOSING CIRCLE
  expectEqual(.enclosingMark, us("\u{20DD}").properties.generalCategory)

  expectEqual(.decimalNumber, us("0").properties.generalCategory)
  // U+2160 ROMAN NUMERAL ONE
  expectEqual(.letterNumber, us("\u{2160}").properties.generalCategory)
  // U+00B2 SUPERSCRIPT TWO
  expectEqual(.otherNumber, us("\u{00B2}").properties.generalCategory)

  expectEqual(.connectorPunctuation, us("_").properties.generalCategory)
  expectEqual(.dashPunctuation, us("-").properties.generalCategory)
  expectEqual(.openPunctuation, us("(").properties.generalCategory)
  expectEqual(.closePunctuation, us(")").properties.generalCategory)
  // U+201C LEFT DOUBLE QUOTATION MARK
  expectEqual(.initialPunctuation, us("\u{201C}").properties.generalCategory)
  // U+201D RIGHT DOUBLE QUOTATION MARK
  expectEqual(.finalPunctuation, us("\u{201D}").properties.generalCategory)
  expectEqual(.otherPunctuation, us("!").properties.generalCategory)

  expectEqual(.mathSymbol, us("+").properties.generalCategory)
  expectEqual(.currencySymbol, us("$").properties.generalCategory)
  expectEqual(.modifierSymbol, us("^").properties.generalCategory)
  // U+00AE REGISTERED SIGN
  expectEqual(.otherSymbol, us("\u{00AE}").properties.generalCategory)

  expectEqual(.spaceSeparator, us(" ").properties.generalCategory)
  // U+2028 LINE SEPARATOR
  expectEqual(.lineSeparator, us("\u{2028}").properties.generalCategory)
  // U+2029 PARAGRAPH SEPARATOR
  expectEqual(.paragraphSeparator, us("\u{2029}").properties.generalCategory)

  expectEqual(.control, us("\u{0000}").properties.generalCategory)
  // U+200B ZERO WIDTH SPACE
  expectEqual(.format, us("\u{200B}").properties.generalCategory)
  expectEqual(.privateUse, us("\u{E000}").properties.generalCategory)
  expectEqual(.unassigned, us("\u{FFFE}").properties.generalCategory)
  // .surrogate cannot be tested because Swift does not permit creating
  // Unicode.Scalars for code points U+D800-DFFF.
}

UnicodeScalarPropertiesTests.test("properties.canonicalCombiningClass") {
  expectEqual(.notReordered, us("A").properties.canonicalCombiningClass)
  expectEqual(.above, us("\u{0301}").properties.canonicalCombiningClass)
  expectEqual(.below, us("\u{0316}").properties.canonicalCombiningClass)
}

UnicodeScalarPropertiesTests.test("properties.numericTypeAndValue") {
  expectNil(us("X").properties.numericType)
  expectNil(us("X").properties.numericValue)

  expectEqual(.decimal, us("3").properties.numericType)
  expectEqual(3, us("3").properties.numericValue)

  // U+00BC VULGAR FRACTION ONE QUARTER
  expectEqual(.numeric, us("\u{00BC}").properties.numericType)
  expectEqual(0.25, us("\u{00BC}").properties.numericValue)

  // U+2463 CIRCLED DIGIT FOUR
  expectEqual(.digit, us("\u{2463}").properties.numericType)
  expectEqual(4, us("\u{2463}").properties.numericValue)
}

runAllTests()

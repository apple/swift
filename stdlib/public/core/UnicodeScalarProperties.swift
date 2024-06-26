//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// Exposes advanced properties of Unicode.Scalar defined by the Unicode
// Standard.
//===----------------------------------------------------------------------===//

import SwiftShims

extension Unicode.Scalar {

  /// A value that provides access to properties of a Unicode scalar that are
  /// defined by the Unicode standard.
  @frozen
  public struct Properties: Sendable {
    @usableFromInline
    internal var _scalar: Unicode.Scalar

    @inlinable
    internal init(_ scalar: Unicode.Scalar) {
      self._scalar = scalar
    }
  }

  /// Properties of this scalar defined by the Unicode standard.
  ///
  /// Use this property to access the Unicode properties of a Unicode scalar
  /// value. The following code tests whether a string contains any math
  /// symbols:
  ///
  ///     let question = "Which is larger, 3 * 3 * 3 or 10 + 10 + 10?"
  ///     let hasMathSymbols = question.unicodeScalars.contains(where: {
  ///         $0.properties.isMath
  ///     })
  ///     // hasMathSymbols == true
  @inlinable
  public var properties: Properties {
    return Properties(self)
  }
}

extension Unicode.Scalar.Properties {
  // This OptionSet represents the 64 bit integer value returned when asking
  // '_swift_stdlib_getBinaryProperties' where each bit indicates a unique
  // Unicode defined binary property of a scalar. For example, bit 8 represents
  // the 'isAlphabetic' property for scalars.
  //
  // WARNING: The values below must be kept in-sync with the generation script.
  // If one should ever update this list below, be it reordering bits, adding
  // new properties, etc., please update the generation script found at:
  // 'utils/gen-unicode-data/Sources/GenScalarProps/BinProps.swift'.
  fileprivate struct _BinaryProperties: OptionSet {
    let rawValue: UInt64

    private init(_ rawValue: UInt64) {
      self.rawValue = rawValue
    }

    // Because we defined the labelless init, we lose the memberwise one
    // generated, so define that here to satisfy the 'OptionSet' requirement.
    init(rawValue: UInt64) {
      self.rawValue = rawValue
    }

    static var changesWhenCaseFolded       : Self { Self(1 &<<  0) }
    static var changesWhenCaseMapped       : Self { Self(1 &<<  1) }
    static var changesWhenLowercased       : Self { Self(1 &<<  2) }
    static var changesWhenNFKCCaseFolded   : Self { Self(1 &<<  3) }
    static var changesWhenTitlecased       : Self { Self(1 &<<  4) }
    static var changesWhenUppercased       : Self { Self(1 &<<  5) }
    static var isASCIIHexDigit             : Self { Self(1 &<<  6) }
    static var isAlphabetic                : Self { Self(1 &<<  7) }
    static var isBidiControl               : Self { Self(1 &<<  8) }
    static var isBidiMirrored              : Self { Self(1 &<<  9) }
    static var isCaseIgnorable             : Self { Self(1 &<< 10) }
    static var isCased                     : Self { Self(1 &<< 11) }
    static var isDash                      : Self { Self(1 &<< 12) }
    static var isDefaultIgnorableCodePoint : Self { Self(1 &<< 13) }
    static var isDeprecated                : Self { Self(1 &<< 14) }
    static var isDiacritic                 : Self { Self(1 &<< 15) }
    static var isEmoji                     : Self { Self(1 &<< 16) }
    static var isEmojiModifier             : Self { Self(1 &<< 17) }
    static var isEmojiModifierBase         : Self { Self(1 &<< 18) }
    static var isEmojiPresentation         : Self { Self(1 &<< 19) }
    static var isExtender                  : Self { Self(1 &<< 20) }
    static var isFullCompositionExclusion  : Self { Self(1 &<< 21) }
    static var isGraphemeBase              : Self { Self(1 &<< 22) }
    static var isGraphemeExtend            : Self { Self(1 &<< 23) }
    static var isHexDigit                  : Self { Self(1 &<< 24) }
    static var isIDContinue                : Self { Self(1 &<< 25) }
    static var isIDSBinaryOperator         : Self { Self(1 &<< 26) }
    static var isIDSTrinaryOperator        : Self { Self(1 &<< 27) }
    static var isIDStart                   : Self { Self(1 &<< 28) }
    static var isIdeographic               : Self { Self(1 &<< 29) }
    static var isJoinControl               : Self { Self(1 &<< 30) }
    static var isLogicalOrderException     : Self { Self(1 &<< 31) }
    static var isLowercase                 : Self { Self(1 &<< 32) }
    static var isMath                      : Self { Self(1 &<< 33) }
    static var isNoncharacterCodePoint     : Self { Self(1 &<< 34) }
    static var isPatternSyntax             : Self { Self(1 &<< 35) }
    static var isPatternWhitespace         : Self { Self(1 &<< 36) }
    static var isQuotationMark             : Self { Self(1 &<< 37) }
    static var isRadical                   : Self { Self(1 &<< 38) }
    static var isSentenceTerminal          : Self { Self(1 &<< 39) }
    static var isSoftDotted                : Self { Self(1 &<< 40) }
    static var isTerminalPunctuation       : Self { Self(1 &<< 41) }
    static var isUnifiedIdeograph          : Self { Self(1 &<< 42) }
    static var isUppercase                 : Self { Self(1 &<< 43) }
    static var isVariationSelector         : Self { Self(1 &<< 44) }
    static var isWhitespace                : Self { Self(1 &<< 45) }
    static var isXIDContinue               : Self { Self(1 &<< 46) }
    static var isXIDStart                  : Self { Self(1 &<< 47) }
  }
}

/// Boolean properties that are defined by the Unicode Standard.
extension Unicode.Scalar.Properties {
  fileprivate var _binaryProperties: _BinaryProperties {
    _BinaryProperties(
      rawValue: _swift_stdlib_getBinaryProperties(_scalar.value)
    )
  }

  /// A Boolean value indicating whether the scalar is alphabetic.
  ///
  /// Alphabetic scalars are the primary units of alphabets and/or syllabaries.
  ///
  /// This property corresponds to the "Alphabetic" and the "Other_Alphabetic"
  /// properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isAlphabetic: Bool {
    _binaryProperties.contains(.isAlphabetic)
  }

  /// A Boolean value indicating whether the scalar is an ASCII character
  /// commonly used for the representation of hexadecimal numbers.
  ///
  /// The only scalars for which this property is `true` are:
  ///
  /// * U+0030...U+0039: DIGIT ZERO...DIGIT NINE
  /// * U+0041...U+0046: LATIN CAPITAL LETTER A...LATIN CAPITAL LETTER F
  /// * U+0061...U+0066: LATIN SMALL LETTER A...LATIN SMALL LETTER F
  ///
  /// This property corresponds to the "ASCII_Hex_Digit" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isASCIIHexDigit: Bool {
    _binaryProperties.contains(.isASCIIHexDigit)
  }

  /// A Boolean value indicating whether the scalar is a format control
  /// character that has a specific function in the Unicode Bidirectional
  /// Algorithm.
  ///
  /// This property corresponds to the "Bidi_Control" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isBidiControl: Bool {
    _binaryProperties.contains(.isBidiControl)
  }

  /// A Boolean value indicating whether the scalar is mirrored in
  /// bidirectional text.
  ///
  /// This property corresponds to the "Bidi_Mirrored" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isBidiMirrored: Bool {
    _binaryProperties.contains(.isBidiMirrored)
  }

  /// A Boolean value indicating whether the scalar is a punctuation
  /// symbol explicitly called out as a dash in the Unicode Standard or a
  /// compatibility equivalent.
  ///
  /// This property corresponds to the "Dash" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isDash: Bool {
    _binaryProperties.contains(.isDash)
  }

  /// A Boolean value indicating whether the scalar is a default-ignorable
  /// code point.
  ///
  /// Default-ignorable code points are those that should be ignored by default
  /// in rendering (unless explicitly supported). They have no visible glyph or
  /// advance width in and of themselves, although they may affect the display,
  /// positioning, or adornment of adjacent or surrounding characters.
  ///
  /// This property corresponds to the "Default_Ignorable_Code_Point" and the
  /// "Other_Default_Ignorable_Code_point" properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isDefaultIgnorableCodePoint: Bool {
    _binaryProperties.contains(.isDefaultIgnorableCodePoint)
  }

  /// A Boolean value indicating whether the scalar is deprecated.
  ///
  /// Scalars are never removed from the Unicode Standard, but the usage of
  /// deprecated scalars is strongly discouraged.
  ///
  /// This property corresponds to the "Deprecated" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isDeprecated: Bool {
    _binaryProperties.contains(.isDeprecated)
  }

  /// A Boolean value indicating whether the scalar is a diacritic.
  ///
  /// Diacritics are scalars that linguistically modify the meaning of another
  /// scalar to which they apply. Scalars for which this property is `true` are
  /// frequently, but not always, combining marks or modifiers.
  ///
  /// This property corresponds to the "Diacritic" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isDiacritic: Bool {
    _binaryProperties.contains(.isDiacritic)
  }

  /// A Boolean value indicating whether the scalar's principal function is
  /// to extend the value or shape of a preceding alphabetic scalar.
  ///
  /// Typical extenders are length and iteration marks.
  ///
  /// This property corresponds to the "Extender" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isExtender: Bool {
    _binaryProperties.contains(.isExtender)
  }

  /// A Boolean value indicating whether the scalar is excluded from
  /// composition when performing Unicode normalization.
  ///
  /// This property corresponds to the "Full_Composition_Exclusion" property in
  /// the [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isFullCompositionExclusion: Bool {
    _binaryProperties.contains(.isFullCompositionExclusion)
  }

  /// A Boolean value indicating whether the scalar is a grapheme base.
  ///
  /// A grapheme base can be thought of as a space-occupying glyph above or
  /// below which other non-spacing modifying glyphs can be applied. For
  /// example, when the character `é` is represented in its decomposed form,
  /// the grapheme base is "e" (U+0065 LATIN SMALL LETTER E) and it is followed
  /// by a single grapheme extender, U+0301 COMBINING ACUTE ACCENT.
  ///
  /// The set of scalars for which `isGraphemeBase` is `true` is disjoint by
  /// definition from the set for which `isGraphemeExtend` is `true`.
  ///
  /// This property corresponds to the "Grapheme_Base" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isGraphemeBase: Bool {
    _binaryProperties.contains(.isGraphemeBase)
  }

  /// A Boolean value indicating whether the scalar is a grapheme extender.
  ///
  /// A grapheme extender can be thought of primarily as a non-spacing glyph
  /// that is applied above or below another glyph. For example, when the
  /// character `é` is represented in its decomposed form, the grapheme base is
  /// "e" (U+0065 LATIN SMALL LETTER E) and it is followed by a single grapheme
  /// extender, U+0301 COMBINING ACUTE ACCENT.
  ///
  /// The set of scalars for which `isGraphemeExtend` is `true` is disjoint by
  /// definition from the set for which `isGraphemeBase` is `true`.
  ///
  /// This property corresponds to the "Grapheme_Extend" and the
  /// "Other_Grapheme_Extend" properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isGraphemeExtend: Bool {
    _binaryProperties.contains(.isGraphemeExtend)
  }

  /// A Boolean value indicating whether the scalar is one that is commonly
  /// used for the representation of hexadecimal numbers or a compatibility
  /// equivalent.
  ///
  /// This property is `true` for all scalars for which `isASCIIHexDigit` is
  /// `true` as well as for their CJK halfwidth and fullwidth variants.
  ///
  /// This property corresponds to the "Hex_Digit" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isHexDigit: Bool {
    _binaryProperties.contains(.isHexDigit)
  }

  /// A Boolean value indicating whether the scalar is one which is
  /// recommended to be allowed to appear in a non-starting position in a
  /// programming language identifier.
  ///
  /// Applications that store identifiers in NFKC normalized form should instead
  /// use `isXIDContinue` to check whether a scalar is a valid identifier
  /// character.
  ///
  /// This property corresponds to the "ID_Continue" and the "Other_ID_Continue"
  /// properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isIDContinue: Bool {
    _binaryProperties.contains(.isIDContinue)
  }

  /// A Boolean value indicating whether the scalar is one which is
  /// recommended to be allowed to appear in a starting position in a
  /// programming language identifier.
  ///
  /// Applications that store identifiers in NFKC normalized form should instead
  /// use `isXIDStart` to check whether a scalar is a valid identifier
  /// character.
  ///
  /// This property corresponds to the "ID_Start" and the "Other_ID_Start"
  /// properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isIDStart: Bool {
    _binaryProperties.contains(.isIDStart)
  }

  /// A Boolean value indicating whether the scalar is considered to be a
  /// CJKV (Chinese, Japanese, Korean, and Vietnamese) or other siniform
  /// (Chinese writing-related) ideograph.
  ///
  /// This property roughly defines the class of "Chinese characters" and does
  /// not include characters of other logographic scripts such as Cuneiform or
  /// Egyptian Hieroglyphs.
  ///
  /// This property corresponds to the "Ideographic" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isIdeographic: Bool {
    _binaryProperties.contains(.isIdeographic)
  }

  /// A Boolean value indicating whether the scalar is an ideographic
  /// description character that determines how the two ideographic characters
  /// or ideographic description sequences that follow it are to be combined to
  /// form a single character.
  ///
  /// Ideographic description characters are technically printable characters,
  /// but advanced rendering engines may use them to approximate ideographs that
  /// are otherwise unrepresentable.
  ///
  /// This property corresponds to the "IDS_Binary_Operator" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isIDSBinaryOperator: Bool {
    _binaryProperties.contains(.isIDSBinaryOperator)
  }

  /// A Boolean value indicating whether the scalar is an ideographic
  /// description character that determines how the three ideographic characters
  /// or ideographic description sequences that follow it are to be combined to
  /// form a single character.
  ///
  /// Ideographic description characters are technically printable characters,
  /// but advanced rendering engines may use them to approximate ideographs that
  /// are otherwise unrepresentable.
  ///
  /// This property corresponds to the "IDS_Trinary_Operator" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isIDSTrinaryOperator: Bool {
    _binaryProperties.contains(.isIDSTrinaryOperator)
  }

  /// A Boolean value indicating whether the scalar is a format control
  /// character that has a specific function in controlling cursive joining and
  /// ligation.
  ///
  /// There are two scalars for which this property is `true`:
  ///
  /// * When U+200C ZERO WIDTH NON-JOINER is inserted between two characters, it
  ///   directs the rendering engine to render them separately/disconnected when
  ///   it might otherwise render them as a ligature. For example, a rendering
  ///   engine might display "fl" in English as a connected glyph; inserting the
  ///   zero width non-joiner would force them to be rendered as disconnected
  ///   glyphs.
  ///
  /// * When U+200D ZERO WIDTH JOINER is inserted between two characters, it
  ///   directs the rendering engine to render them as a connected glyph when it
  ///   would otherwise render them independently. The zero width joiner is also
  ///   used to construct complex emoji from sequences of base emoji characters.
  ///   For example, the various "family" emoji are encoded as sequences of man,
  ///   woman, or child emoji that are interleaved with zero width joiners.
  ///
  /// This property corresponds to the "Join_Control" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isJoinControl: Bool {
    _binaryProperties.contains(.isJoinControl)
  }

  /// A Boolean value indicating whether the scalar requires special handling
  /// for operations involving ordering, such as sorting and searching.
  ///
  /// This property applies to a small number of spacing vowel letters occurring
  /// in some Southeast Asian scripts like Thai and Lao, which use a visual
  /// order display model. Such letters are stored in text ahead of
  /// syllable-initial consonants.
  ///
  /// This property corresponds to the "Logical_Order_Exception" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isLogicalOrderException: Bool {
    _binaryProperties.contains(.isLogicalOrderException)
  }

  /// A Boolean value indicating whether the scalar's letterform is
  /// considered lowercase.
  ///
  /// This property corresponds to the "Lowercase" and the "Other_Lowercase"
  /// properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isLowercase: Bool {
    _binaryProperties.contains(.isLowercase)
  }

  /// A Boolean value indicating whether the scalar is one that naturally
  /// appears in mathematical contexts.
  ///
  /// The set of scalars for which this property is `true` includes mathematical
  /// operators and symbols as well as specific Greek and Hebrew letter
  /// variants that are categorized as symbols. Notably, it does _not_ contain
  /// the standard digits or Latin/Greek letter blocks; instead, it contains the
  /// mathematical Latin, Greek, and Arabic letters and numbers defined in the
  /// Supplemental Multilingual Plane.
  ///
  /// This property corresponds to the "Math" and the "Other_Math" properties in
  /// the [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isMath: Bool {
    _binaryProperties.contains(.isMath)
  }

  /// A Boolean value indicating whether the scalar is permanently reserved
  /// for internal use.
  ///
  /// This property corresponds to the "Noncharacter_Code_Point" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isNoncharacterCodePoint: Bool {
    _binaryProperties.contains(.isNoncharacterCodePoint)
  }

  /// A Boolean value indicating whether the scalar is one that is used in
  /// writing to surround quoted text.
  ///
  /// This property corresponds to the "Quotation_Mark" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isQuotationMark: Bool {
    _binaryProperties.contains(.isQuotationMark)
  }

  /// A Boolean value indicating whether the scalar is a radical component of
  /// CJK characters, Tangut characters, or Yi syllables.
  ///
  /// These scalars are often the components of ideographic description
  /// sequences, as defined by the `isIDSBinaryOperator` and
  /// `isIDSTrinaryOperator` properties.
  ///
  /// This property corresponds to the "Radical" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isRadical: Bool {
    _binaryProperties.contains(.isRadical)
  }

  /// A Boolean value indicating whether the scalar has a "soft dot" that
  /// disappears when a diacritic is placed over the scalar.
  ///
  /// For example, "i" is soft dotted because the dot disappears when adding an
  /// accent mark, as in "í".
  ///
  /// This property corresponds to the "Soft_Dotted" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isSoftDotted: Bool {
    _binaryProperties.contains(.isSoftDotted)
  }

  /// A Boolean value indicating whether the scalar is a punctuation symbol
  /// that typically marks the end of a textual unit.
  ///
  /// This property corresponds to the "Terminal_Punctuation" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isTerminalPunctuation: Bool {
    _binaryProperties.contains(.isTerminalPunctuation)
  }

  /// A Boolean value indicating whether the scalar is one of the unified
  /// CJK ideographs in the Unicode Standard.
  ///
  /// This property is false for CJK punctuation and symbols, as well as for
  /// compatibility ideographs (which canonically decompose to unified
  /// ideographs).
  ///
  /// This property corresponds to the "Unified_Ideograph" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isUnifiedIdeograph: Bool {
    _binaryProperties.contains(.isUnifiedIdeograph)
  }

  /// A Boolean value indicating whether the scalar's letterform is
  /// considered uppercase.
  ///
  /// This property corresponds to the "Uppercase" and the "Other_Uppercase"
  /// properties in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isUppercase: Bool {
    _binaryProperties.contains(.isUppercase)
  }

  /// A Boolean value indicating whether the scalar is a whitespace
  /// character.
  ///
  /// This property is `true` for scalars that are spaces, separator characters,
  /// and other control characters that should be treated as whitespace for the
  /// purposes of parsing text elements.
  ///
  /// This property corresponds to the "White_Space" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isWhitespace: Bool {
    _binaryProperties.contains(.isWhitespace)
  }

  /// A Boolean value indicating whether the scalar is one which is
  /// recommended to be allowed to appear in a non-starting position in a
  /// programming language identifier, with adjustments made for NFKC normalized
  /// form.
  ///
  /// The set of scalars `[:XID_Continue:]` closes the set `[:ID_Continue:]`
  /// under NFKC normalization by removing any scalars whose normalized form is
  /// not of the form `[:ID_Continue:]*`.
  ///
  /// This property corresponds to the "XID_Continue" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isXIDContinue: Bool {
    _binaryProperties.contains(.isXIDContinue)
  }

  /// A Boolean value indicating whether the scalar is one which is
  /// recommended to be allowed to appear in a starting position in a
  /// programming language identifier, with adjustments made for NFKC normalized
  /// form.
  ///
  /// The set of scalars `[:XID_Start:]` closes the set `[:ID_Start:]` under
  /// NFKC normalization by removing any scalars whose normalized form is not of
  /// the form `[:ID_Start:] [:ID_Continue:]*`.
  ///
  /// This property corresponds to the "XID_Start" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isXIDStart: Bool {
    _binaryProperties.contains(.isXIDStart)
  }

  /// A Boolean value indicating whether the scalar is a punctuation mark
  /// that generally marks the end of a sentence.
  ///
  /// This property corresponds to the "Sentence_Terminal" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isSentenceTerminal: Bool {
    _binaryProperties.contains(.isSentenceTerminal)
  }

  /// A Boolean value indicating whether the scalar is a variation selector.
  ///
  /// Variation selectors allow rendering engines that support them to choose
  /// different glyphs to display for a particular code point.
  ///
  /// This property corresponds to the "Variation_Selector" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isVariationSelector: Bool {
    _binaryProperties.contains(.isVariationSelector)
  }

  /// A Boolean value indicating whether the scalar is recommended to have
  /// syntactic usage in patterns represented in source code.
  ///
  /// This property corresponds to the "Pattern_Syntax" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isPatternSyntax: Bool {
    _binaryProperties.contains(.isPatternSyntax)
  }

  /// A Boolean value indicating whether the scalar is recommended to be
  /// treated as whitespace when parsing patterns represented in source code.
  ///
  /// This property corresponds to the "Pattern_White_Space" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isPatternWhitespace: Bool {
    _binaryProperties.contains(.isPatternWhitespace)
  }

  /// A Boolean value indicating whether the scalar is considered to be
  /// either lowercase, uppercase, or titlecase.
  ///
  /// Though similar in name, this property is *not* equivalent to
  /// `changesWhenCaseMapped`. The set of scalars for which `isCased` is `true`
  /// is a superset of those for which `changesWhenCaseMapped` is `true`. For
  /// example, the Latin small capitals that are used by the International
  /// Phonetic Alphabet have a case, but do not change when they are mapped to
  /// any of the other cases.
  ///
  /// This property corresponds to the "Cased" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isCased: Bool {
    _binaryProperties.contains(.isCased)
  }

  /// A Boolean value indicating whether the scalar is ignored for casing
  /// purposes.
  ///
  /// This property corresponds to the "Case_Ignorable" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var isCaseIgnorable: Bool {
    _binaryProperties.contains(.isCaseIgnorable)
  }

  /// A Boolean value indicating whether the scalar's normalized form differs
  /// from the `lowercaseMapping` of each constituent scalar.
  ///
  /// This property corresponds to the "Changes_When_Lowercased" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenLowercased: Bool {
    _binaryProperties.contains(.changesWhenLowercased)
  }

  /// A Boolean value indicating whether the scalar's normalized form differs
  /// from the `uppercaseMapping` of each constituent scalar.
  ///
  /// This property corresponds to the "Changes_When_Uppercased" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenUppercased: Bool {
    _binaryProperties.contains(.changesWhenUppercased)
  }

  /// A Boolean value indicating whether the scalar's normalized form differs
  /// from the `titlecaseMapping` of each constituent scalar.
  ///
  /// This property corresponds to the "Changes_When_Titlecased" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenTitlecased: Bool {
    _binaryProperties.contains(.changesWhenTitlecased)
  }

  /// A Boolean value indicating whether the scalar's normalized form differs
  /// from the case-fold mapping of each constituent scalar.
  ///
  /// This property corresponds to the "Changes_When_Casefolded" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenCaseFolded: Bool {
    _binaryProperties.contains(.changesWhenCaseFolded)
  }

  /// A Boolean value indicating whether the scalar may change when it
  /// undergoes case mapping.
  ///
  /// This property is `true` whenever one or more of `changesWhenLowercased`,
  /// `changesWhenUppercased`, or `changesWhenTitlecased` are `true`.
  ///
  /// This property corresponds to the "Changes_When_Casemapped" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenCaseMapped: Bool {
    _binaryProperties.contains(.changesWhenCaseMapped)
  }

  /// A Boolean value indicating whether the scalar is one that is not
  /// identical to its NFKC case-fold mapping.
  ///
  /// This property corresponds to the "Changes_When_NFKC_Casefolded" property
  /// in the [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var changesWhenNFKCCaseFolded: Bool {
    _binaryProperties.contains(.changesWhenNFKCCaseFolded)
  }

  /// A Boolean value indicating whether the scalar has an emoji
  /// presentation, whether or not it is the default.
  ///
  /// This property is true for scalars that are rendered as emoji by default
  /// and also for scalars that have a non-default emoji rendering when followed
  /// by U+FE0F VARIATION SELECTOR-16. This includes some scalars that are not
  /// typically considered to be emoji:
  ///
  ///     let scalars: [Unicode.Scalar] = ["😎", "$", "0"]
  ///     for s in scalars {
  ///         print(s, "-->", s.properties.isEmoji)
  ///     }
  ///     // 😎 --> true
  ///     // $ --> false
  ///     // 0 --> true
  ///
  /// The final result is true because the ASCII digits have non-default emoji
  /// presentations; some platforms render these with an alternate appearance.
  ///
  /// Because of this behavior, testing `isEmoji` alone on a single scalar is
  /// insufficient to determine if a unit of text is rendered as an emoji; a
  /// correct test requires inspecting multiple scalars in a `Character`. In
  /// addition to checking whether the base scalar has `isEmoji == true`, you
  /// must also check its default presentation (see `isEmojiPresentation`) and
  /// determine whether it is followed by a variation selector that would modify
  /// the presentation.
  ///
  /// This property corresponds to the "Emoji" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  @available(macOS 10.12.2, iOS 10.2, tvOS 10.1, watchOS 3.1.1, *)
  public var isEmoji: Bool {
    _binaryProperties.contains(.isEmoji)
  }

  /// A Boolean value indicating whether the scalar is one that should be
  /// rendered with an emoji presentation, rather than a text presentation, by
  /// default.
  ///
  /// Scalars that have default to emoji presentation can be followed by
  /// U+FE0E VARIATION SELECTOR-15 to request the text presentation of the
  /// scalar instead. Likewise, scalars that default to text presentation can
  /// be followed by U+FE0F VARIATION SELECTOR-16 to request the emoji
  /// presentation.
  ///
  /// This property corresponds to the "Emoji_Presentation" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  @available(macOS 10.12.2, iOS 10.2, tvOS 10.1, watchOS 3.1.1, *)
  public var isEmojiPresentation: Bool {
    _binaryProperties.contains(.isEmojiPresentation)
  }

  /// A Boolean value indicating whether the scalar is one that can modify
  /// a base emoji that precedes it.
  ///
  /// The Fitzpatrick skin types are examples of emoji modifiers; they change
  /// the appearance of the preceding emoji base (that is, a scalar for which
  /// `isEmojiModifierBase` is true) by rendering it with a different skin tone.
  ///
  /// This property corresponds to the "Emoji_Modifier" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  @available(macOS 10.12.2, iOS 10.2, tvOS 10.1, watchOS 3.1.1, *)
  public var isEmojiModifier: Bool {
    _binaryProperties.contains(.isEmojiModifier)
  }

  /// A Boolean value indicating whether the scalar is one whose appearance
  /// can be changed by an emoji modifier that follows it.
  ///
  /// This property corresponds to the "Emoji_Modifier_Base" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  @available(macOS 10.12.2, iOS 10.2, tvOS 10.1, watchOS 3.1.1, *)
  public var isEmojiModifierBase: Bool {
    _binaryProperties.contains(.isEmojiModifierBase)
  }
}

/// Case mapping properties.
extension Unicode.Scalar.Properties {
  fileprivate struct _CaseMapping {
    let rawValue: UInt8

    static let uppercase = _CaseMapping(rawValue: 0)
    static let lowercase = _CaseMapping(rawValue: 1)
    static let titlecase = _CaseMapping(rawValue: 2)
  }

  fileprivate func _getMapping(_ mapping: _CaseMapping) -> String {
    // First, check if our scalar has a special mapping where it's mapped to
    // more than 1 scalar.
    var specialMappingLength = 0

    let specialMappingPtr = _swift_stdlib_getSpecialMapping(
      _scalar.value,
      mapping.rawValue,
      &specialMappingLength
    )

    if let specialMapping = specialMappingPtr, specialMappingLength != 0 {
      let buffer = UnsafeBufferPointer<UInt8>(
        start: specialMapping,
        count: specialMappingLength
      )

      return String._uncheckedFromUTF8(buffer, isASCII: false)
    }

    // If we did not have a special mapping, check if we have a direct scalar
    // to scalar mapping.
    let mappingDistance = _swift_stdlib_getMapping(
      _scalar.value,
      mapping.rawValue
    )

    if mappingDistance != 0 {
      let scalar = Unicode.Scalar(
        _unchecked: UInt32(Int(_scalar.value) &+ Int(mappingDistance))
      )
      return String(scalar)
    }

    // We did not have any mapping. Return the scalar as is.
    return String(_scalar)
  }

  /// The lowercase mapping of the scalar.
  ///
  /// This property is a `String`, not a `Unicode.Scalar` or `Character`,
  /// because some mappings may transform a scalar into multiple scalars or
  /// graphemes. For example, the character "İ" (U+0130 LATIN CAPITAL LETTER I
  /// WITH DOT ABOVE) becomes two scalars (U+0069 LATIN SMALL LETTER I, U+0307
  /// COMBINING DOT ABOVE) when converted to lowercase.
  ///
  /// This property corresponds to the "Lowercase_Mapping" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var lowercaseMapping: String {
    _getMapping(.lowercase)
  }

  /// The titlecase mapping of the scalar.
  ///
  /// This property is a `String`, not a `Unicode.Scalar` or `Character`,
  /// because some mappings may transform a scalar into multiple scalars or
  /// graphemes. For example, the ligature "ﬁ" (U+FB01 LATIN SMALL LIGATURE FI)
  /// becomes "Fi" (U+0046 LATIN CAPITAL LETTER F, U+0069 LATIN SMALL LETTER I)
  /// when converted to titlecase.
  ///
  /// This property corresponds to the "Titlecase_Mapping" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var titlecaseMapping: String {
    _getMapping(.titlecase)
  }

  /// The uppercase mapping of the scalar.
  ///
  /// This property is a `String`, not a `Unicode.Scalar` or `Character`,
  /// because some mappings may transform a scalar into multiple scalars or
  /// graphemes. For example, the German letter "ß" (U+00DF LATIN SMALL LETTER
  /// SHARP S) becomes "SS" (U+0053 LATIN CAPITAL LETTER S, U+0053 LATIN CAPITAL
  /// LETTER S) when converted to uppercase.
  ///
  /// This property corresponds to the "Uppercase_Mapping" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var uppercaseMapping: String {
    _getMapping(.uppercase)
  }
}

extension Unicode {

  /// A version of the Unicode Standard represented by its major and minor
  /// components.
  public typealias Version = (major: Int, minor: Int)
}

extension Unicode.Scalar.Properties {

  /// The earliest version of the Unicode Standard in which the scalar was
  /// assigned.
  ///
  /// This value is `nil` for code points that have not yet been assigned.
  ///
  /// This property corresponds to the "Age" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var age: Unicode.Version? {
    let age: UInt16 = _swift_stdlib_getAge(_scalar.value)

    if age == .max {
      return nil
    }

    let major = age & 0xFF
    let minor = (age & 0xFF00) >> 8

    return (major: Int(major), minor: Int(minor))
  }
}

extension Unicode {

  /// The most general classification of a Unicode scalar.
  ///
  /// The general category of a scalar is its "first-order, most usual
  /// categorization". It does not attempt to cover multiple uses of some
  /// scalars, such as the use of letters to represent Roman numerals.
  public enum GeneralCategory: Sendable {

    /// An uppercase letter.
    ///
    /// This value corresponds to the category `Uppercase_Letter` (abbreviated
    /// `Lu`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case uppercaseLetter

    /// A lowercase letter.
    ///
    /// This value corresponds to the category `Lowercase_Letter` (abbreviated
    /// `Ll`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case lowercaseLetter

    /// A digraph character whose first part is uppercase.
    ///
    /// This value corresponds to the category `Titlecase_Letter` (abbreviated
    /// `Lt`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case titlecaseLetter

    /// A modifier letter.
    ///
    /// This value corresponds to the category `Modifier_Letter` (abbreviated
    /// `Lm`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case modifierLetter

    /// Other letters, including syllables and ideographs.
    ///
    /// This value corresponds to the category `Other_Letter` (abbreviated
    /// `Lo`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case otherLetter

    /// A non-spacing combining mark with zero advance width (abbreviated Mn).
    ///
    /// This value corresponds to the category `Nonspacing_Mark` (abbreviated
    /// `Mn`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case nonspacingMark

    /// A spacing combining mark with positive advance width.
    ///
    /// This value corresponds to the category `Spacing_Mark` (abbreviated `Mc`)
    /// in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case spacingMark

    /// An enclosing combining mark.
    ///
    /// This value corresponds to the category `Enclosing_Mark` (abbreviated
    /// `Me`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case enclosingMark

    /// A decimal digit.
    ///
    /// This value corresponds to the category `Decimal_Number` (abbreviated
    /// `Nd`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case decimalNumber

    /// A letter-like numeric character.
    ///
    /// This value corresponds to the category `Letter_Number` (abbreviated
    /// `Nl`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case letterNumber

    /// A numeric character of another type.
    ///
    /// This value corresponds to the category `Other_Number` (abbreviated `No`)
    /// in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case otherNumber

    /// A connecting punctuation mark, like a tie.
    ///
    /// This value corresponds to the category `Connector_Punctuation`
    /// (abbreviated `Pc`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case connectorPunctuation

    /// A dash or hyphen punctuation mark.
    ///
    /// This value corresponds to the category `Dash_Punctuation` (abbreviated
    /// `Pd`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case dashPunctuation

    /// An opening punctuation mark of a pair.
    ///
    /// This value corresponds to the category `Open_Punctuation` (abbreviated
    /// `Ps`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case openPunctuation

    /// A closing punctuation mark of a pair.
    ///
    /// This value corresponds to the category `Close_Punctuation` (abbreviated
    /// `Pe`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case closePunctuation

    /// An initial quotation mark.
    ///
    /// This value corresponds to the category `Initial_Punctuation`
    /// (abbreviated `Pi`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case initialPunctuation

    /// A final quotation mark.
    ///
    /// This value corresponds to the category `Final_Punctuation` (abbreviated
    /// `Pf`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case finalPunctuation

    /// A punctuation mark of another type.
    ///
    /// This value corresponds to the category `Other_Punctuation` (abbreviated
    /// `Po`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case otherPunctuation

    /// A symbol of mathematical use.
    ///
    /// This value corresponds to the category `Math_Symbol` (abbreviated `Sm`)
    /// in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case mathSymbol

    /// A currency sign.
    ///
    /// This value corresponds to the category `Currency_Symbol` (abbreviated
    /// `Sc`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case currencySymbol

    /// A non-letterlike modifier symbol.
    ///
    /// This value corresponds to the category `Modifier_Symbol` (abbreviated
    /// `Sk`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case modifierSymbol

    /// A symbol of another type.
    ///
    /// This value corresponds to the category `Other_Symbol` (abbreviated
    /// `So`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case otherSymbol

    /// A space character of non-zero width.
    ///
    /// This value corresponds to the category `Space_Separator` (abbreviated
    /// `Zs`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case spaceSeparator

    /// A line separator, which is specifically (and only) U+2028 LINE
    /// SEPARATOR.
    ///
    /// This value corresponds to the category `Line_Separator` (abbreviated
    /// `Zl`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case lineSeparator

    /// A paragraph separator, which is specifically (and only) U+2029 PARAGRAPH
    /// SEPARATOR.
    ///
    /// This value corresponds to the category `Paragraph_Separator`
    /// (abbreviated `Zp`) in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case paragraphSeparator

    /// A C0 or C1 control code.
    ///
    /// This value corresponds to the category `Control` (abbreviated `Cc`) in
    /// the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case control

    /// A format control character.
    ///
    /// This value corresponds to the category `Format` (abbreviated `Cf`) in
    /// the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case format

    /// A surrogate code point.
    ///
    /// This value corresponds to the category `Surrogate` (abbreviated `Cs`) in
    /// the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case surrogate

    /// A private-use character.
    ///
    /// This value corresponds to the category `Private_Use` (abbreviated `Co`)
    /// in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case privateUse

    /// A reserved unassigned code point or a non-character.
    ///
    /// This value corresponds to the category `Unassigned` (abbreviated `Cn`)
    /// in the
    /// [Unicode Standard](https://unicode.org/reports/tr44/#General_Category_Values).
    case unassigned

    internal init(rawValue: UInt8) {
      switch rawValue {
      case 0: self = .uppercaseLetter
      case 1: self = .lowercaseLetter
      case 2: self = .titlecaseLetter
      case 3: self = .modifierLetter
      case 4: self = .otherLetter
      case 5: self = .nonspacingMark
      case 6: self = .spacingMark
      case 7: self = .enclosingMark
      case 8: self = .decimalNumber
      case 9: self = .letterNumber
      case 10: self = .otherNumber
      case 11: self = .connectorPunctuation
      case 12: self = .dashPunctuation
      case 13: self = .openPunctuation
      case 14: self = .closePunctuation
      case 15: self = .initialPunctuation
      case 16: self = .finalPunctuation
      case 17: self = .otherPunctuation
      case 18: self = .mathSymbol
      case 19: self = .currencySymbol
      case 20: self = .modifierSymbol
      case 21: self = .otherSymbol
      case 22: self = .spaceSeparator
      case 23: self = .lineSeparator
      case 24: self = .paragraphSeparator
      case 25: self = .control
      case 26: self = .format
      case 27: self = .surrogate
      case 28: self = .privateUse
      case 29: self = .unassigned
      default: fatalError("Unknown general category")
      }
    }
  }
}

// Internal helpers
extension Unicode.GeneralCategory {
  internal var _isSymbol: Bool {
    switch self {
      case .mathSymbol, .currencySymbol, .modifierSymbol, .otherSymbol:
        return true
      default: return false
    }
  }
  internal var _isPunctuation: Bool {
    switch self {
      case .connectorPunctuation, .dashPunctuation, .openPunctuation,
           .closePunctuation, .initialPunctuation, .finalPunctuation,
           .otherPunctuation:
        return true
      default: return false
    }
  }
}

extension Unicode.Scalar.Properties {

  /// The general category (most usual classification) of the scalar.
  ///
  /// This property corresponds to the "General_Category" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var generalCategory: Unicode.GeneralCategory {
    let rawValue = _swift_stdlib_getGeneralCategory(_scalar.value)
    return Unicode.GeneralCategory(rawValue: rawValue)
  }
}

extension Unicode.Scalar.Properties {
  internal func _hangulName() -> String {
    // T = Hangul tail consonants
    let T: (base: UInt32, count: UInt32) = (base: 0x11A7, count: 28)
    // N = Number of precomposed Hangul syllables that start with the same
    //     leading consonant. (There is no base for N).
    let N: (base: UInt32, count: UInt32) = (base: 0x0, count: 588)
    // S = Hangul precomposed syllables
    let S: (base: UInt32, count: UInt32) = (base: 0xAC00, count: 11172)

    let hangulLTable = ["G", "GG", "N", "D", "DD", "R", "M", "B", "BB", "S",
                        "SS", "", "J", "JJ", "C", "K", "T", "P", "H"]

    let hangulVTable = ["A", "AE", "YA", "YAE", "EO", "E", "YEO", "YE", "O",
                        "WA", "WAE", "OE", "YO", "U", "WEO", "WE", "WI", "YU",
                        "EU", "YI", "I"]

    let hangulTTable = ["", "G", "GG", "GS", "N", "NJ", "NH", "D", "L", "LG",
                        "LM", "LB", "LS", "LT", "LP", "LH", "M", "B", "BS", "S",
                        "SS", "NG", "J", "C", "K", "T", "P", "H"]

    let sIdx = _scalar.value &- S.base
    let lIdx = Int(sIdx / N.count)
    let vIdx = Int((sIdx % N.count) / T.count)
    let tIdx = Int(sIdx % T.count)

    let scalarName = hangulLTable[lIdx] + hangulVTable[vIdx] + hangulTTable[tIdx]
    return "HANGUL SYLLABLE \(scalarName)"
  }

  // Used to potentially return a name who can either be represented in a large
  // range or algorithmically. A good example are the Hangul names. Instead of
  // storing those names, we can define an algorithm to generate the name.
  internal func _fastScalarName() -> String? {
    // Define a couple algorithmic names below.

    let scalarName = String(_scalar.value, radix: 16, uppercase: true)

    switch _scalar.value {
    // Hangul Syllable *
    case (0xAC00 ... 0xD7A3):
      return _hangulName()

    // Variation Selector-17 through Variation Selector-256
    case (0xE0100 ... 0xE01EF):
      return "VARIATION SELECTOR-\(_scalar.value - 0xE0100 + 17)"

    case (0x3400 ... 0x4DBF),
         (0x4E00 ... 0x9FFF),
         (0x20000 ... 0x2A6DF),
         (0x2A700 ... 0x2B739),
         (0x2B740 ... 0x2B81D),
         (0x2B820 ... 0x2CEA1),
         (0x2CEB0 ... 0x2EBE0),
         (0x30000 ... 0x3134A),
         (0x31350 ... 0x323AF):
      return "CJK UNIFIED IDEOGRAPH-\(scalarName)"

    case (0xF900 ... 0xFA6D),
         (0xFA70 ... 0xFAD9),
         (0x2F800 ... 0x2FA1D):
      return "CJK COMPATIBILITY IDEOGRAPH-\(scalarName)"

    case (0x17000 ... 0x187F7),
         (0x18D00 ... 0x18D08):
      return "TANGUT IDEOGRAPH-\(scalarName)"

    case (0x18B00 ... 0x18CD5):
      return "KHITAN SMALL SCRIPT CHARACTER-\(scalarName)"

    case (0x1B170 ... 0x1B2FB):
      return "NUSHU CHARACTER-\(scalarName)"

    // Otherwise, go look it up.
    default:
      return nil
    }
  }

  /// The published name of the scalar.
  ///
  /// Some scalars, such as control characters, do not have a value for this
  /// property in the Unicode Character Database. For such scalars, this
  /// property is `nil`.
  ///
  /// This property corresponds to the "Name" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var name: String? {
    if let fastName = _fastScalarName() {
      return fastName
    }

    // The longest name that Unicode defines is 88 characters long.
    let largestCount = Int(SWIFT_STDLIB_LARGEST_NAME_COUNT)

    let name = String(_uninitializedCapacity: largestCount) { buffer in
      _swift_stdlib_getScalarName(
        _scalar.value,
        buffer.baseAddress,
        buffer.count
      )
    }

    return name.isEmpty ? nil : name
  }

  /// The normative formal alias of the scalar.
  ///
  /// The name of a scalar is immutable and never changed in future versions of
  /// the Unicode Standard. The `nameAlias` property is provided to issue
  /// corrections if a name was issued erroneously. For example, the `name` of
  /// U+FE18 is "PRESENTATION FORM FOR VERTICAL RIGHT WHITE LENTICULAR BRAKCET"
  /// (note that "BRAKCET" is misspelled). The `nameAlias` property then
  /// contains the corrected name.
  ///
  /// If a scalar has no alias, this property is `nil`.
  ///
  /// This property corresponds to the "Name_Alias" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var nameAlias: String? {
    guard let nameAliasPtr = _swift_stdlib_getNameAlias(_scalar.value) else {
      return nil
    }

    return String(cString: nameAliasPtr)
  }
}

extension Unicode {

  /// The classification of a scalar used in the Canonical Ordering Algorithm
  /// defined by the Unicode Standard.
  ///
  /// Canonical combining classes are used by the ordering algorithm to
  /// determine if two sequences of combining marks should be considered
  /// canonically equivalent (that is, identical in interpretation). Two
  /// sequences are canonically equivalent if they are equal when sorting the
  /// scalars in ascending order by their combining class.
  ///
  /// For example, consider the sequence `"\u{0041}\u{0301}\u{0316}"` (LATIN
  /// CAPITAL LETTER A, COMBINING ACUTE ACCENT, COMBINING GRAVE ACCENT BELOW).
  /// The combining classes of these scalars have the numeric values 0, 230, and
  /// 220, respectively. Sorting these scalars by their combining classes yields
  /// `"\u{0041}\u{0316}\u{0301}"`, so two strings that differ only by the
  /// ordering of those scalars would compare as equal:
  ///
  ///     let aboveBeforeBelow = "\u{0041}\u{0301}\u{0316}"
  ///     let belowBeforeAbove = "\u{0041}\u{0316}\u{0301}"
  ///     print(aboveBeforeBelow == belowBeforeAbove)
  ///     // Prints "true"
  ///
  /// Named and Unnamed Combining Classes
  /// ===================================
  ///
  /// Canonical combining classes are defined in the Unicode Standard as
  /// integers in the range `0...254`. For convenience, the standard assigns
  /// symbolic names to a subset of these combining classes.
  ///
  /// The `CanonicalCombiningClass` type conforms to `RawRepresentable` with a
  /// raw value of type `UInt8`. You can create instances of the type by using
  /// the static members named after the symbolic names, or by using the
  /// `init(rawValue:)` initializer.
  ///
  ///     let overlayClass = Unicode.CanonicalCombiningClass(rawValue: 1)
  ///     let overlayClassIsOverlay = overlayClass == .overlay
  ///     // overlayClassIsOverlay == true
  public struct CanonicalCombiningClass:
    Comparable, Hashable, RawRepresentable, Sendable
  {
    /// Base glyphs that occupy their own space and do not combine with others.
    public static let notReordered = CanonicalCombiningClass(rawValue: 0)

    /// Marks that overlay a base letter or symbol.
    public static let overlay = CanonicalCombiningClass(rawValue: 1)

    /// Diacritic nukta marks in Brahmi-derived scripts.
    public static let nukta = CanonicalCombiningClass(rawValue: 7)

    /// Combining marks that are attached to hiragana and katakana to indicate
    /// voicing changes.
    public static let kanaVoicing = CanonicalCombiningClass(rawValue: 8)

    /// Diacritic virama marks in Brahmi-derived scripts.
    public static let virama = CanonicalCombiningClass(rawValue: 9)

    /// Marks attached at the bottom left.
    public static let attachedBelowLeft = CanonicalCombiningClass(rawValue: 200)

    /// Marks attached directly below.
    public static let attachedBelow = CanonicalCombiningClass(rawValue: 202)

    /// Marks attached directly above.
    public static let attachedAbove = CanonicalCombiningClass(rawValue: 214)

    /// Marks attached at the top right.
    public static let attachedAboveRight =
      CanonicalCombiningClass(rawValue: 216)

    /// Distinct marks at the bottom left.
    public static let belowLeft = CanonicalCombiningClass(rawValue: 218)

    /// Distinct marks directly below.
    public static let below = CanonicalCombiningClass(rawValue: 220)

    /// Distinct marks at the bottom right.
    public static let belowRight = CanonicalCombiningClass(rawValue: 222)

    /// Distinct marks to the left.
    public static let left = CanonicalCombiningClass(rawValue: 224)

    /// Distinct marks to the right.
    public static let right = CanonicalCombiningClass(rawValue: 226)

    /// Distinct marks at the top left.
    public static let aboveLeft = CanonicalCombiningClass(rawValue: 228)

    /// Distinct marks directly above.
    public static let above = CanonicalCombiningClass(rawValue: 230)

    /// Distinct marks at the top right.
    public static let aboveRight = CanonicalCombiningClass(rawValue: 232)

    /// Distinct marks subtending two bases.
    public static let doubleBelow = CanonicalCombiningClass(rawValue: 233)

    /// Distinct marks extending above two bases.
    public static let doubleAbove = CanonicalCombiningClass(rawValue: 234)

    /// Greek iota subscript only (U+0345 COMBINING GREEK YPOGEGRAMMENI).
    public static let iotaSubscript = CanonicalCombiningClass(rawValue: 240)

    /// The raw integer value of the canonical combining class.
    public let rawValue: UInt8

    /// Creates a new canonical combining class with the given raw integer
    /// value.
    ///
    /// - Parameter rawValue: The raw integer value of the canonical combining
    ///   class.
    public init(rawValue: UInt8) {
      self.rawValue = rawValue
    }

    public static func == (
      lhs: CanonicalCombiningClass,
      rhs: CanonicalCombiningClass
    ) -> Bool {
      return lhs.rawValue == rhs.rawValue
    }

    public static func < (
      lhs: CanonicalCombiningClass,
      rhs: CanonicalCombiningClass
    ) -> Bool {
      return lhs.rawValue < rhs.rawValue
    }

    public var hashValue: Int {
      return rawValue.hashValue
    }

    public func hash(into hasher: inout Hasher) {
      hasher.combine(rawValue)
    }
  }
}

extension Unicode.Scalar.Properties {

  /// The canonical combining class of the scalar.
  ///
  /// This property corresponds to the "Canonical_Combining_Class" property in
  /// the [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var canonicalCombiningClass: Unicode.CanonicalCombiningClass {
    let normData = Unicode._NormData(_scalar)
    return Unicode.CanonicalCombiningClass(rawValue: normData.ccc)
  }
}

extension Unicode {

  /// The numeric type of a scalar.
  ///
  /// Scalars with a non-nil numeric type include numbers, fractions, numeric
  /// superscripts and subscripts, and circled or otherwise decorated number
  /// glyphs.
  ///
  /// Some letterlike scalars used in numeric systems, such as Greek or Latin
  /// letters, do not have a non-nil numeric type, in order to prevent programs
  /// from incorrectly interpreting them as numbers in non-numeric contexts.
  public enum NumericType: Sendable {

    /// A digit that is commonly understood to form base-10 numbers.
    ///
    /// Specifically, scalars have this numeric type if they occupy a contiguous
    /// range of code points representing numeric values `0...9`.
    case decimal

    /// A digit that does not meet the requirements of the `decimal` numeric
    /// type.
    ///
    /// Scalars with this numeric type are often those that represent a decimal
    /// digit but would not typically be used to write a base-10 number, such
    /// as "④" (U+2463 CIRCLED DIGIT FOUR).
    ///
    /// As of Unicode 6.3, any new scalars that represent numbers but do not
    /// meet the requirements of `decimal` will have numeric type `numeric`,
    /// and programs can treat `digit` and `numeric` equivalently.
    case digit

    /// A digit that does not meet the requirements of the `decimal` numeric
    /// type or a non-digit numeric value.
    ///
    /// This numeric type includes fractions such as "⅕" (U+2155 VULGAR
    /// FRACTION ONE FIFTH), numerical CJK ideographs like "兆" (U+5146 CJK
    /// UNIFIED IDEOGRAPH-5146), and other scalars that are not decimal digits
    /// used positionally in the writing of base-10 numbers.
    ///
    /// As of Unicode 6.3, any new scalars that represent numbers but do not
    /// meet the requirements of `decimal` will have numeric type `numeric`,
    /// and programs can treat `digit` and `numeric` equivalently.
    case numeric

    internal init(rawValue: UInt8) {
      switch rawValue {
      case 0:
        self = .numeric
      case 1:
        self = .digit
      case 2:
        self = .decimal
      default:
        fatalError("Unknown numeric type")
      }
    }
  }
}

/// Numeric properties of scalars.
extension Unicode.Scalar.Properties {

  /// The numeric type of the scalar.
  ///
  /// For scalars that represent a number, `numericType` is the numeric type
  /// of the scalar. For all other scalars, this property is `nil`.
  ///
  ///     let scalars: [Unicode.Scalar] = ["4", "④", "⅕", "X"]
  ///     for scalar in scalars {
  ///         print(scalar, "-->", scalar.properties.numericType)
  ///     }
  ///     // 4 --> Optional(Swift.Unicode.NumericType.decimal)
  ///     // ④ --> Optional(Swift.Unicode.NumericType.digit)
  ///     // ⅕ --> Optional(Swift.Unicode.NumericType.numeric)
  ///     // X --> nil
  ///
  /// This property corresponds to the "Numeric_Type" property in the
  /// [Unicode Standard](http://www.unicode.org/versions/latest/).
  public var numericType: Unicode.NumericType? {
    let rawValue = _swift_stdlib_getNumericType(_scalar.value)

    guard rawValue != .max else {
      return nil
    }

    return Unicode.NumericType(rawValue: rawValue)
  }

  /// The numeric value of the scalar.
  ///
  /// For scalars that represent a numeric value, `numericValue` is the whole
  /// or fractional value. For all other scalars, this property is `nil`.
  ///
  ///     let scalars: [Unicode.Scalar] = ["4", "④", "⅕", "X"]
  ///     for scalar in scalars {
  ///         print(scalar, "-->", scalar.properties.numericValue)
  ///     }
  ///     // 4 --> Optional(4.0)
  ///     // ④ --> Optional(4.0)
  ///     // ⅕ --> Optional(0.2)
  ///     // X --> nil
  ///
  /// This property corresponds to the "Numeric_Value" property in the [Unicode
  /// Standard](http://www.unicode.org/versions/latest/).
  public var numericValue: Double? {
    guard numericType != nil else {
      return nil
    }

    return _swift_stdlib_getNumericValue(_scalar.value)
  }
}

//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A single extended grapheme cluster that approximates a user-perceived
/// character.
///
/// The `Character` type represents a character made up of one or more Unicode
/// scalar values, grouped by a Unicode boundary algorithm. Generally, a
/// `Character` instance matches what the reader of a string will perceive as
/// a single character. Strings are collections of `Character` instances, so
/// the number of visible characters is generally the most natural way to
/// count the length of a string.
///
///     let greeting = "Hello! 🐥"
///     print("Length: \(greeting.count)")
///     // Prints "Length: 8"
///
/// Because each character in a string can be made up of one or more Unicode
/// scalar values, the number of characters in a string may not match the
/// length of the Unicode scalar value representation or the length of the
/// string in a particular binary representation.
///
///     print("Unicode scalar value count: \(greeting.unicodeScalars.count)")
///     // Prints "Unicode scalar value count: 8"
///
///     print("UTF-8 representation count: \(greeting.utf8.count)")
///     // Prints "UTF-8 representation count: 11"
///
/// Every `Character` instance is composed of one or more Unicode scalar values
/// that are grouped together as an *extended grapheme cluster*. The way these
/// scalar values are grouped is defined by a canonical, localized, or
/// otherwise tailored Unicode segmentation algorithm.
///
/// For example, a country's Unicode flag character is made up of two regional
/// indicator scalar values that correspond to that country's ISO 3166-1
/// alpha-2 code. The alpha-2 code for The United States is "US", so its flag
/// character is made up of the Unicode scalar values `"\u{1F1FA}"` (REGIONAL
/// INDICATOR SYMBOL LETTER U) and `"\u{1F1F8}"` (REGIONAL INDICATOR SYMBOL
/// LETTER S). When placed next to each other in a string literal, these two
/// scalar values are combined into a single grapheme cluster, represented by
/// a `Character` instance in Swift.
///
///     let usFlag: Character = "\u{1F1FA}\u{1F1F8}"
///     print(usFlag)
///     // Prints "🇺🇸"
///
/// For more information about the Unicode terms used in this discussion, see
/// the [Unicode.org glossary][glossary]. In particular, this discussion
/// mentions [extended grapheme clusters][clusters] and [Unicode scalar
/// values][scalars].
///
/// [glossary]: http://www.unicode.org/glossary/
/// [clusters]: http://www.unicode.org/glossary/#extended_grapheme_cluster
/// [scalars]: http://www.unicode.org/glossary/#unicode_scalar_value
@frozen
public struct Character: Sendable {
  @usableFromInline
  internal var _str: String

  @inlinable @inline(__always)
  internal init(unchecked str: String) {
    self._str = str
    _invariantCheck()
  }
}

extension Character {
  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    _internalInvariant(_str.count == 1)
    _internalInvariant(_str._guts.isFastUTF8)
    _internalInvariant(_str._guts._object.isPreferredRepresentation)
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension Character {
  /// A view of a character's contents as a collection of UTF-8 code units. See
  /// String.UTF8View for more information
  public typealias UTF8View = String.UTF8View

  /// A UTF-8 encoding of `self`.
  @inlinable
  public var utf8: UTF8View { return _str.utf8 }

  /// A view of a character's contents as a collection of UTF-16 code units. See
  /// String.UTF16View for more information
  public typealias UTF16View = String.UTF16View

  /// A UTF-16 encoding of `self`.
  @inlinable
  public var utf16: UTF16View { return _str.utf16 }

  public typealias UnicodeScalarView = String.UnicodeScalarView

  @inlinable
  public var unicodeScalars: UnicodeScalarView { return _str.unicodeScalars }
}

extension Character :
  _ExpressibleByBuiltinExtendedGraphemeClusterLiteral,
  ExpressibleByExtendedGraphemeClusterLiteral
{
  /// Creates a character containing the given Unicode scalar value.
  ///
  /// - Parameter content: The Unicode scalar value to convert into a character.
  @inlinable @inline(__always)
  public init(_ content: Unicode.Scalar) {
    self.init(unchecked: String(content))
  }

  @inlinable @inline(__always)
  @_effects(readonly)
  public init(_builtinUnicodeScalarLiteral value: Builtin.Int32) {
    self.init(Unicode.Scalar(_builtinUnicodeScalarLiteral: value))
  }

  // Inlining ensures that the whole constructor can be folded away to a single
  // integer constant in case of small character literals.
  @inlinable @inline(__always)
  @_effects(readonly)
  public init(
    _builtinExtendedGraphemeClusterLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
  ) {
    self.init(unchecked: String(
      _builtinExtendedGraphemeClusterLiteral: start,
      utf8CodeUnitCount: utf8CodeUnitCount,
      isASCII: isASCII))
  }

  /// Creates a character with the specified value.
  ///
  /// Do not call this initializer directly. It is used by the compiler when
  /// you use a string literal to initialize a `Character` instance. For
  /// example:
  ///
  ///     let oBreve: Character = "o\u{306}"
  ///     print(oBreve)
  ///     // Prints "ŏ"
  ///
  /// The assignment to the `oBreve` constant calls this initializer behind the
  /// scenes.
  @inlinable @inline(__always)
  public init(extendedGraphemeClusterLiteral value: Character) {
    self.init(unchecked: value._str)
  }

  /// Creates a character from a single-character string.
  ///
  /// The following example creates a new character from the uppercase version
  /// of a string that only holds one character.
  ///
  ///     let a = "a"
  ///     let capitalA = Character(a.uppercased())
  ///
  /// - Parameter s: The single-character string to convert to a `Character`
  ///   instance. `s` must contain exactly one extended grapheme cluster.
  @inlinable @inline(__always)
  public init(_ s: String) {
    _precondition(!s.isEmpty,
      "Can't form a Character from an empty String")
    _debugPrecondition(s.index(after: s.startIndex) == s.endIndex,
      "Can't form a Character from a String containing more than one extended grapheme cluster")

    if _fastPath(s._guts._object.isPreferredRepresentation) {
      self.init(unchecked: s)
      return
    }
    self.init(unchecked: String._copying(s))
  }
}

extension Character: CustomStringConvertible {
 @inlinable
 public var description: String {
   return _str
 }
}

extension Character: LosslessStringConvertible { }

extension Character: CustomDebugStringConvertible {
 /// A textual representation of the character, suitable for debugging.
 public var debugDescription: String {
   return _str.debugDescription
 }
}

extension String {
  /// Creates a string containing the given character.
  ///
  /// - Parameter c: The character to convert to a string.
  @inlinable @inline(__always)
  public init(_ c: Character) {
    self.init(c._str._guts)
  }
}

extension Character: Equatable {
  @inlinable @inline(__always)
  @_effects(readonly)
  public static func == (lhs: Character, rhs: Character) -> Bool {
    return lhs._str == rhs._str
  }
}

extension Character: Comparable {
  @inlinable @inline(__always)
  @_effects(readonly)
  public static func < (lhs: Character, rhs: Character) -> Bool {
    return lhs._str < rhs._str
  }
}

extension Character: Hashable {
  // not @inlinable (performance)
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @_effects(releasenone)
  public func hash(into hasher: inout Hasher) {
    _str.hash(into: &hasher)
  }
}

extension Character {
  @usableFromInline // @testable
  internal var _isSmall: Bool {
    return _str._guts.isSmall
  }
}

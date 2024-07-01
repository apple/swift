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
// Unicode.Scalar Type
//===----------------------------------------------------------------------===//

extension Unicode {
  /// A Unicode scalar value.
  ///
  /// The `Unicode.Scalar` type, representing a single Unicode scalar value, is
  /// the element type of a string's `unicodeScalars` collection.
  ///
  /// You can create a `Unicode.Scalar` instance by using a string literal that
  /// contains a single character representing exactly one Unicode scalar value.
  ///
  ///     let letterK: Unicode.Scalar = "K"
  ///     let kim: Unicode.Scalar = "김"
  ///     print(letterK, kim)
  ///     // Prints "K 김"
  ///
  /// You can also create Unicode scalar values directly from their numeric
  /// representation.
  ///
  ///     let airplane = Unicode.Scalar(9992)!
  ///     print(airplane)
  ///     // Prints "✈︎"
  @frozen
  public struct Scalar: Sendable {
    @usableFromInline
    internal var _value: UInt32

    @inlinable
    internal init(_value: UInt32) {
      self._value = _value
    }
  }
}

extension Unicode.Scalar :
    _ExpressibleByBuiltinUnicodeScalarLiteral,
    ExpressibleByUnicodeScalarLiteral {
  /// A numeric representation of the Unicode scalar.
  @inlinable
  public var value: UInt32 { return _value }

  @_transparent
  public init(_builtinUnicodeScalarLiteral value: Builtin.Int32) {
    self._value = UInt32(value)
  }

  /// Creates a Unicode scalar with the specified value.
  ///
  /// Do not call this initializer directly. It may be used by the compiler
  /// when you use a string literal to initialize a `Unicode.Scalar` instance.
  ///
  ///     let letterK: Unicode.Scalar = "K"
  ///     print(letterK)
  ///     // Prints "K"
  ///
  /// In this example, the assignment to the `letterK` constant is handled by
  /// this initializer behind the scenes.
  @_transparent
  public init(unicodeScalarLiteral value: Unicode.Scalar) {
    self = value
  }

  /// Creates a Unicode scalar with the specified numeric value.
  ///
  /// For example, the following code sample creates a `Unicode.Scalar`
  /// instance with a value of an emoji character:
  ///
  ///     let codepoint: UInt32 = 127881
  ///     let emoji = Unicode.Scalar(codepoint)
  ///     print(emoji!)
  ///     // Prints "🎉"
  ///
  /// In case of an invalid input value, nil is returned.
  ///
  ///     let codepoint: UInt32 = extValue   // This might be an invalid value
  ///     if let emoji = Unicode.Scalar(codepoint) {
  ///       print(emoji)
  ///     } else {
  ///       // Do something else
  ///     }
  ///
  /// - Parameter v: The Unicode code point to use for the scalar. The
  ///   initializer succeeds if `v` is a valid Unicode scalar value---that is,
  ///   if `v` is in the range `0...0xD7FF` or `0xE000...0x10FFFF`. If `v` is
  ///   an invalid Unicode scalar value, the result is `nil`.
  @inlinable
  public init?(_ v: UInt32) {
    // Unicode 6.3.0:
    //
    //     D9.  Unicode codespace: A range of integers from 0 to 10FFFF.
    //
    //     D76. Unicode scalar value: Any Unicode code point except
    //     high-surrogate and low-surrogate code points.
    //
    //     * As a result of this definition, the set of Unicode scalar values
    //     consists of the ranges 0 to D7FF and E000 to 10FFFF, inclusive.
    if (v < 0xD800 || v > 0xDFFF) && v <= 0x10FFFF {
      self._value = v
      return
    }
    // Return nil in case of an invalid unicode scalar value.
    return nil
  }

  /// Creates a Unicode scalar with the specified numeric value.
  ///
  /// For example, the following code sample creates a `Unicode.Scalar`
  /// instance with a value of `"밥"`, the Korean word for rice:
  ///
  ///     let codepoint: UInt16 = 48165
  ///     let bap = Unicode.Scalar(codepoint)
  ///     print(bap!)
  ///     // Prints "밥"
  ///
  /// In case of an invalid input value, the result is `nil`.
  ///
  ///     let codepoint: UInt16 = extValue   // This might be an invalid value
  ///     if let bap = Unicode.Scalar(codepoint) {
  ///         print(bap)
  ///     } else {
  ///         // Do something else
  ///     }
  ///
  /// - Parameter v: The Unicode code point to use for the scalar. The
  ///   initializer succeeds if `v` is a valid Unicode scalar value, in the
  ///   range `0...0xD7FF` or `0xE000...0x10FFFF`. If `v` is an invalid
  ///   unicode scalar value, the result is `nil`.
  @inlinable
  public init?(_ v: UInt16) {
    self.init(UInt32(v))
  }

  /// Creates a Unicode scalar with the specified numeric value.
  ///
  /// For example, the following code sample creates a `Unicode.Scalar`
  /// instance with a value of `"7"`:
  ///
  ///     let codepoint: UInt8 = 55
  ///     let seven = Unicode.Scalar(codepoint)
  ///     print(seven)
  ///     // Prints "7"
  ///
  /// - Parameter v: The code point to use for the scalar.
  @inlinable
  public init(_ v: UInt8) {
    self._value = UInt32(v)
  }

  /// Creates a duplicate of the given Unicode scalar.
  @inlinable
  public init(_ v: Unicode.Scalar) {
    // This constructor allows one to provide necessary type context to
    // disambiguate between function overloads on 'String' and 'Unicode.Scalar'.
    self = v
  }

  /// Returns a string representation of the Unicode scalar.
  ///
  /// Scalar values representing characters that are normally unprintable or
  /// that otherwise require escaping are escaped with a backslash.
  ///
  ///     let tab = Unicode.Scalar(9)!
  ///     print(tab)
  ///     // Prints " "
  ///     print(tab.escaped(asASCII: false))
  ///     // Prints "\t"
  ///
  /// When the `forceASCII` parameter is `true`, a `Unicode.Scalar` instance
  /// with a value greater than 127 is represented using an escaped numeric
  /// value; otherwise, non-ASCII characters are represented using their
  /// typical string value.
  ///
  ///     let bap = Unicode.Scalar(48165)!
  ///     print(bap.escaped(asASCII: false))
  ///     // Prints "밥"
  ///     print(bap.escaped(asASCII: true))
  ///     // Prints "\u{BC25}"
  ///
  /// - Parameter forceASCII: Pass `true` if you need the result to use only
  ///   ASCII characters; otherwise, pass `false`.
  /// - Returns: A string representation of the scalar.
  public func escaped(asASCII forceASCII: Bool) -> String {
    _escaped(asASCII: forceASCII) ?? String(self)
  }

  internal func _escaped(asASCII forceASCII: Bool) -> String? {
    func lowNibbleAsHex(_ v: UInt32) -> String {
      let nibble = v & 15
      if nibble < 10 {
        return String(Unicode.Scalar(nibble+48)!)    // 48 = '0'
      } else {
        return String(Unicode.Scalar(nibble-10+65)!) // 65 = 'A'
      }
    }

    if self == "\\" {
      return "\\\\"
    } else if self == "\'" {
      return "\\\'"
    } else if self == "\"" {
      return "\\\""
    } else if _isPrintableASCII {
      return nil
    } else if self == "\0" {
      return "\\0"
    } else if self == "\n" {
      return "\\n"
    } else if self == "\r" {
      return "\\r"
    } else if self == "\t" {
      return "\\t"
    } else if UInt32(self) < 128 {
      return "\\u{"
        + lowNibbleAsHex(UInt32(self) >> 4)
        + lowNibbleAsHex(UInt32(self)) + "}"
    } else if !forceASCII {
      return nil
    } else if UInt32(self) <= 0xFFFF {
      var result = "\\u{"
      result += lowNibbleAsHex(UInt32(self) >> 12)
      result += lowNibbleAsHex(UInt32(self) >> 8)
      result += lowNibbleAsHex(UInt32(self) >> 4)
      result += lowNibbleAsHex(UInt32(self))
      result += "}"
      return result
    } else {
      // FIXME: Type checker performance prohibits this from being a
      // single chained "+".
      var result = "\\u{"
      result += lowNibbleAsHex(UInt32(self) >> 28)
      result += lowNibbleAsHex(UInt32(self) >> 24)
      result += lowNibbleAsHex(UInt32(self) >> 20)
      result += lowNibbleAsHex(UInt32(self) >> 16)
      result += lowNibbleAsHex(UInt32(self) >> 12)
      result += lowNibbleAsHex(UInt32(self) >> 8)
      result += lowNibbleAsHex(UInt32(self) >> 4)
      result += lowNibbleAsHex(UInt32(self))
      result += "}"
      return result
    }
  }

  /// A Boolean value indicating whether the Unicode scalar is an ASCII
  /// character.
  ///
  /// ASCII characters have a scalar value between 0 and 127, inclusive. For
  /// example:
  ///
  ///     let canyon = "Cañón"
  ///     for scalar in canyon.unicodeScalars {
  ///         print(scalar, scalar.isASCII, scalar.value)
  ///     }
  ///     // Prints "C true 67"
  ///     // Prints "a true 97"
  ///     // Prints "ñ false 241"
  ///     // Prints "ó false 243"
  ///     // Prints "n true 110"
  @inlinable
  public var isASCII: Bool {
    return value <= 127
  }

  // FIXME: Unicode makes this interesting.
  internal var _isPrintableASCII: Bool {
    return (self >= Unicode.Scalar(0o040) && self <= Unicode.Scalar(0o176))
  }
}

extension Unicode.Scalar: CustomStringConvertible, CustomDebugStringConvertible {
  /// A textual representation of the Unicode scalar.
  @inlinable
  public var description: String {
    return String(self)
  }

  /// An escaped textual representation of the Unicode scalar, suitable for
  /// debugging.
  public var debugDescription: String {
    return "\"\(escaped(asASCII: true))\""
  }
}

extension Unicode.Scalar: LosslessStringConvertible {
  @inlinable
  public init?(_ description: String) {
    let scalars = description.unicodeScalars
    guard let v = scalars.first, scalars.count == 1 else {
      return nil
    }
    self = v
  }
}

extension Unicode.Scalar: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(self.value)
  }
}

extension Unicode.Scalar {
  /// Creates a Unicode scalar with the specified numeric value.
  ///
  /// - Parameter v: The Unicode code point to use for the scalar. `v` must be
  ///   a valid Unicode scalar value, in the ranges `0...0xD7FF` or
  ///   `0xE000...0x10FFFF`. In case of an invalid unicode scalar value, nil is
  ///   returned.
  ///
  /// For example, the following code sample creates a `Unicode.Scalar` instance
  /// with a value of an emoji character:
  ///
  ///     let codepoint = 127881
  ///     let emoji = Unicode.Scalar(codepoint)!
  ///     print(emoji)
  ///     // Prints "🎉"
  ///
  /// In case of an invalid input value, nil is returned.
  ///
  ///     let codepoint: UInt32 = extValue // This might be an invalid value.
  ///     if let emoji = Unicode.Scalar(codepoint) {
  ///       print(emoji)
  ///     } else {
  ///       // Do something else
  ///     }
  @inlinable
  public init?(_ v: Int) {
    if let exact = UInt32(exactly: v) {
      self.init(exact)
    } else {
      return nil
    }
  }
}

extension UInt8 {
  /// Construct with value `v.value`.
  ///
  /// - Precondition: `v.value` can be represented as ASCII (0..<128).
  @inlinable
  public init(ascii v: Unicode.Scalar) {
    _precondition(v.value < 128,
        "Code point value does not fit into ASCII")
    self = UInt8(v.value)
  }
}
extension UInt32 {
  /// Construct with value `v.value`.
  @inlinable
  public init(_ v: Unicode.Scalar) {
    self = v.value
  }
}
extension UInt64 {
  /// Construct with value `v.value`.
  @inlinable
  public init(_ v: Unicode.Scalar) {
    self = UInt64(v.value)
  }
}

/// Extends `UInt8` providing initialization from a Unicode scalar.
extension UInt8 {
  /// Converts a UInt8 value into a UnicodeScalar
  @_transparent @_alwaysEmitIntoClient
  var ascii: Unicode.Scalar? {
    guard self < 0x80 else { return nil }
    return Unicode.Scalar(self)
  }
  /// Initializes a FixedWidthInteger with the value of the provided Unicode scalar.
  ///
  /// - Parameter unicode: The Unicode scalar to initialize from.
  /// - Note: Construct with value `v.value`.
  @inlinable @_alwaysEmitIntoClient
  public init?(ifASCII v: Unicode.Scalar) {
    guard v.value < 0x80 else { return nil }
    self = Self(v.value)
  }
}

/// Extends `UInt8` to allow direct comparisons with double quoted literals.
extension UInt8 {
  /// Returns a Boolean indicating whether the `UInt8` is equal to the provided Unicode scalar.
  ///
  /// - Parameters:
  ///   - i: The `UInt8` value to compare.
  ///   - s: The Unicode scalar to compare against.
  /// - Returns: `true` when the `UInt8` is equal to the provided Unicode scalar; otherwise, `false`.
  @_transparent @_alwaysEmitIntoClient
  public static func == (i: Self, s: Unicode.Scalar) -> Bool {
    #if DEBUG
    return i == UInt8(ascii: s)
    #else
    return i == UInt8(ifASCII: s)
    #endif
  }

  /// Returns a Boolean indicating whether the `UInt8` is not equal to the provided Unicode scalar.
  @_transparent @_alwaysEmitIntoClient
  public static func != (i: Self, s: Unicode.Scalar) -> Bool {
    #if DEBUG
    return i != UInt8(ascii: s)
    #else
    return i != UInt8(ifASCII: s)
    #endif
  }

  /// Enables pattern matching of Unicode scalars in switch statements.
  @_transparent @_alwaysEmitIntoClient
  public static func ~= (s: Unicode.Scalar, i: Self) -> Bool {
    #if DEBUG
    return i == UInt8(ascii: s)
    #else
    return i == UInt8(ifASCII: s)
    #endif
  }
}

/// Extends `Optional<UInt8>` to allow direct comparisons with double quoted literals.
extension UInt8? {
  /// Returns a Boolean value indicating whether the optional `UInt8` is equal to the provided Unicode scalar.
  ///
  /// - Parameters:
  ///   - i: The optional `UInt8` value to compare.
  ///   - s: The Unicode scalar to compare against.
  /// - Returns: `true` if the optional `UInt8` is equal to the provided Unicode scalar; otherwise, `false`.
  @_transparent @_alwaysEmitIntoClient
  public static func == (i: Self, s: Unicode.Scalar) -> Bool {
    #if DEBUG
    return i == UInt8(ascii: s)
    #else
    return i == UInt8(ifASCII: s)
    #endif
  }

  /// Returns a Boolean value indicating whether the optional `UInt8` is not equal to the provided Unicode scalar.
  @_transparent @_alwaysEmitIntoClient
  public static func != (i: Self, s: Unicode.Scalar) -> Bool {
    #if DEBUG
    return i != UInt8(ascii: s)
    #else
    return i != UInt8(ifASCII: s)
    #endif
  }

  /// Allows pattern matching of Unicode scalars in switch statements.
  @_transparent @_alwaysEmitIntoClient
  public static func ~= (s: Unicode.Scalar, i: Self) -> Bool {
    #if DEBUG
    return i == UInt8(ascii: s)
    #else
    return i == UInt8(ifASCII: s)
    #endif
  }
}

/// Extends `FixedWidthInteger` providing initialization from a Unicode scalar.
extension FixedWidthInteger {
  /// Initializes a FixedWidthInteger with the value of the provided Unicode scalar.
  ///
  /// - Parameter unicode: The Unicode scalar to initialize from.
  /// - Note: Construct with value `v.value`.
  @inlinable @_alwaysEmitIntoClient
  public init?(unicode v: Unicode.Scalar) {
    guard v.value <= Self.max else { return nil }
    self = Self(v.value)
  }
}

extension Unicode.Scalar: Equatable {
  @inlinable
  public static func == (lhs: Unicode.Scalar, rhs: Unicode.Scalar) -> Bool {
    return lhs.value == rhs.value
  }
}

extension Unicode.Scalar: Comparable {
  @inlinable
  public static func < (lhs: Unicode.Scalar, rhs: Unicode.Scalar) -> Bool {
    return lhs.value < rhs.value
  }
}

extension Unicode.Scalar {
  @frozen
  public struct UTF16View: Sendable {
    @usableFromInline
    internal var value: Unicode.Scalar

    @inlinable
    internal init(value: Unicode.Scalar) {
      self.value = value
    }
  }

  @inlinable
  public var utf16: UTF16View {
    return UTF16View(value: self)
  }
}

extension Unicode.Scalar.UTF16View: RandomAccessCollection {

  public typealias Indices = Range<Int>

  /// The position of the first code unit.
  @inlinable
  public var startIndex: Int {
    return 0
  }

  /// The "past the end" position---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// If the collection is empty, `endIndex` is equal to `startIndex`.
  @inlinable
  public var endIndex: Int {
    return 0 + UTF16.width(value)
  }

  /// Accesses the code unit at the specified position.
  ///
  /// - Parameter position: The position of the element to access. `position`
  ///   must be a valid index of the collection that is not equal to the
  ///   `endIndex` property.
  @inlinable
  public subscript(position: Int) -> UTF16.CodeUnit {
    if position == 1 { return UTF16.trailSurrogate(value) }
    if endIndex == 1 { return UTF16.CodeUnit(value.value) }
    return UTF16.leadSurrogate(value)
  }
}

extension Unicode.Scalar {
  @available(SwiftStdlib 5.1, *)
  @frozen
  public struct UTF8View: Sendable {
    @usableFromInline
    internal var value: Unicode.Scalar

    @inlinable
    internal init(value: Unicode.Scalar) {
      self.value = value
    }
  }

  @available(SwiftStdlib 5.1, *)
  @inlinable
  public var utf8: UTF8View { return UTF8View(value: self) }
}

@available(SwiftStdlib 5.1, *)
extension Unicode.Scalar.UTF8View: RandomAccessCollection {
  public typealias Indices = Range<Int>

  /// The position of the first code unit.
  @inlinable
  public var startIndex: Int { return 0 }

  /// The "past the end" position---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// If the collection is empty, `endIndex` is equal to `startIndex`.
  @inlinable
  public var endIndex: Int { return 0 + UTF8.width(value) }

  /// Accesses the code unit at the specified position.
  ///
  /// - Parameter position: The position of the element to access. `position`
  ///   must be a valid index of the collection that is not equal to the
  ///   `endIndex` property.
  @inlinable
  public subscript(position: Int) -> UTF8.CodeUnit {
    _precondition(position >= startIndex && position < endIndex,
      "Unicode.Scalar.UTF8View index is out of bounds")
    return value.withUTF8CodeUnits { $0[position] }
  }
}

extension Unicode.Scalar {
  internal static var _replacementCharacter: Unicode.Scalar {
    return Unicode.Scalar(_value: UTF32._replacementCodeUnit)
  }
}

extension Unicode.Scalar {
  /// Creates an instance of the NUL scalar value.
  @available(*, unavailable, message: "use 'Unicode.Scalar(0)'")
  public init() {
    Builtin.unreachable()
  }
}

// Access the underlying code units
extension Unicode.Scalar {
  // Access the scalar as encoded in UTF-16
  internal func withUTF16CodeUnits<Result>(
    _ body: (UnsafeBufferPointer<UInt16>) throws -> Result
  ) rethrows -> Result {
    var codeUnits: (UInt16, UInt16) = (self.utf16[0], 0)
    let utf16Count = self.utf16.count
    if utf16Count > 1 {
      _internalInvariant(utf16Count == 2)
      codeUnits.1 = self.utf16[1]
    }
    return try Swift.withUnsafePointer(to: &codeUnits) {
      return try $0.withMemoryRebound(to: UInt16.self, capacity: 2) {
        return try body(UnsafeBufferPointer(start: $0, count: utf16Count))
      }
    }
  }

  // Access the scalar as encoded in UTF-8
  @inlinable
  internal func withUTF8CodeUnits<Result>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> Result
  ) rethrows -> Result {
    let encodedScalar = UTF8.encode(self)!
    var (codeUnits, utf8Count) = encodedScalar._bytes

    // The first code unit is in the least significant byte of codeUnits.
    codeUnits = codeUnits.littleEndian
#if $TypedThrows
    return try Swift._withUnprotectedUnsafePointer(to: &codeUnits) {
      return try $0.withMemoryRebound(to: UInt8.self, capacity: 4) {
        return try body(UnsafeBufferPointer(start: $0, count: utf8Count))
      }
    }
#else
    return try Swift.__abi_se0413_withUnsafePointer(to: &codeUnits) {
      return try $0.withMemoryRebound(to: UInt8.self, capacity: 4) {
        return try body(UnsafeBufferPointer(start: $0, count: utf8Count))
      }
    }
#endif
  }
}

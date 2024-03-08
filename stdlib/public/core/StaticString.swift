//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implementation Note: Because StaticString is used in the
// implementation of _precondition(), _fatalErrorMessage(), etc., we
// keep it extremely close to the bare metal.  In particular, because
// we store only Builtin types, we are guaranteed that no assertions
// are involved in its construction.  This feature is crucial for
// preventing infinite recursion even in non-asserting cases.
//
//===----------------------------------------------------------------------===//

/// A string type designed to represent text that is known at compile time.
///
/// Instances of the `StaticString` type are immutable.
///
/// `StaticString` provides only low-level access to its contents, unlike
/// Swift's more commonly used `String` type. A static string can use
/// either of the following as its storage:
///
/// * a pointer to a null-terminated sequence of UTF-8 code units:
///
///       let emoji: StaticString = "\u{1F600}"
///       emoji.hasPointerRepresentation  //-> true
///       emoji.isASCII                   //-> false
///       emoji.unicodeScalar             //-> Fatal error!
///       emoji.utf8CodeUnitCount         //-> 4
///       emoji.utf8Start[0]              //-> 0xF0
///       emoji.utf8Start[1]              //-> 0x9F
///       emoji.utf8Start[2]              //-> 0x98
///       emoji.utf8Start[3]              //-> 0x80
///       emoji.utf8Start[4]              //-> 0x00
///
/// * a single Unicode scalar value, under very limited circumstances:
///
///       struct MyStaticScalar: ExpressibleByUnicodeScalarLiteral {
///           typealias UnicodeScalarLiteralType = StaticString
///           let value: StaticString
///           init(unicodeScalarLiteral value: StaticString) {
///               self.value = value
///           }
///       }
///
///       let emoji: StaticString = MyStaticScalar("\u{1F600}").value
///       emoji.hasPointerRepresentation  //-> false
///       emoji.isASCII                   //-> false
///       emoji.unicodeScalar.value       //-> 0x1F600
///       emoji.utf8CodeUnitCount         //-> Fatal error!
///       emoji.utf8Start                 //-> Fatal error!
///
/// You can use the `withUTF8Buffer(_:)` method to access a static string's
/// contents, regardless of which representation the static string uses.
///
///     emoji.withUTF8Buffer { utf8 in
///         utf8.count  //-> 4
///         utf8[0]     //-> 0xF0
///         utf8[1]     //-> 0x9F
///         utf8[2]     //-> 0x98
///         utf8[3]     //-> 0x80
///         utf8[4]     //-> Fatal error!
///     }
@frozen
public struct StaticString: Sendable {

  /// Either a pointer to the start of UTF-8 data, represented as an integer,
  /// or an integer representation of a single Unicode scalar.
  @usableFromInline
  internal var _startPtrOrData: Builtin.Word

  /// If `_startPtrOrData` is a pointer, contains the length of the UTF-8 data
  /// in bytes (excluding the null terminator).
  @usableFromInline
  internal var _utf8CodeUnitCount: Builtin.Word

  /// Extra flags:
  ///
  /// - bit 0: set to 0 if `_startPtrOrData` is a pointer, or to 1 if it is a
  ///   Unicode scalar.
  ///
  /// - bit 1: set to 1 if `_startPtrOrData` either points to an ASCII code unit
  ///   sequence, or stores an ASCII scalar value.
  @usableFromInline
  internal var _flags: Builtin.Int8

  /// Creates an empty static string.
  @_transparent
  public init() {
    self = ""
  }

  @usableFromInline @_transparent
  internal init(
    _start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
  ) {
    // We don't go through UnsafePointer here to make things simpler for alias
    // analysis. A higher-level algorithm may be trying to make sure an
    // unrelated buffer is not accessed or freed.
    self._startPtrOrData = Builtin.ptrtoint_Word(_start)
    self._utf8CodeUnitCount = utf8CodeUnitCount
    self._flags = Bool(isASCII)
      ? (0x2 as UInt8)._value
      : (0x0 as UInt8)._value
  }

  @usableFromInline @_transparent
  internal init(
    unicodeScalar: Builtin.Int32
  ) {
    self._startPtrOrData = UInt(UInt32(unicodeScalar))._builtinWordValue
    self._utf8CodeUnitCount = 0._builtinWordValue
    self._flags = Unicode.Scalar(_builtinUnicodeScalarLiteral: unicodeScalar).isASCII
      ? (0x3 as UInt8)._value
      : (0x1 as UInt8)._value
  }

  /// A pointer to a null-terminated sequence of UTF-8 code units.
  ///
  /// - Important: Accessing this property when `hasPointerRepresentation` is
  ///   `false` triggers a runtime error.
  @_transparent
  public var utf8Start: UnsafePointer<UInt8> {
    _precondition(
      hasPointerRepresentation,
      "StaticString should have pointer representation")
    return UnsafePointer(bitPattern: UInt(_startPtrOrData))!
  }

  /// A single Unicode scalar value.
  ///
  /// - Important: Accessing this property when `hasPointerRepresentation` is
  ///   `true` triggers a runtime error.
  @_transparent
  public var unicodeScalar: Unicode.Scalar {
    _precondition(
      !hasPointerRepresentation,
      "StaticString should have Unicode scalar representation")
    return Unicode.Scalar(UInt32(UInt(_startPtrOrData)))!
  }

  /// The number of UTF-8 code units (excluding the null terminator).
  ///
  /// - Important: Accessing this property when `hasPointerRepresentation` is
  ///   `false` triggers a runtime error.
  @_transparent
  public var utf8CodeUnitCount: Int {
    _precondition(
      hasPointerRepresentation,
      "StaticString should have pointer representation")
    return Int(_utf8CodeUnitCount)
  }

  @_alwaysEmitIntoClient @_transparent
  internal var unsafeRawPointer: Builtin.RawPointer {
    return Builtin.inttoptr_Word(_startPtrOrData)
  }

  /// A Boolean value that indicates whether the static string stores a
  /// pointer to a null-terminated sequence of UTF-8 code units.
  ///
  /// If `hasPointerRepresentation` is `false`, the static string stores a
  /// single Unicode scalar value.
  @_transparent
  public var hasPointerRepresentation: Bool {
    return (UInt8(_flags) & 0x1) == 0
  }

  /// A Boolean value that indicates whether the static string represents only
  /// ASCII code units (or an ASCII scalar value).
  @_transparent
  public var isASCII: Bool {
    return (UInt8(_flags) & 0x2) != 0
  }

  /// Invokes the given closure with a buffer containing the static string's
  /// UTF-8 code unit sequence (excluding the null terminator).
  ///
  /// This method works regardless of whether the static string stores a
  /// pointer or a single Unicode scalar value.
  ///
  /// The pointer argument to `body` is valid only during the execution of
  /// `withUTF8Buffer(_:)`. Do not store or return the pointer for later use.
  ///
  /// - Parameter body: A closure that takes a buffer pointer to the static
  ///   string's UTF-8 code unit sequence as its sole argument. If the closure
  ///   has a return value, that value is also used as the return value of the
  ///   `withUTF8Buffer(_:)` method. The pointer argument is valid only for the
  ///   duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure.
  @_transparent
  public func withUTF8Buffer<R>(
    _ body: (UnsafeBufferPointer<UInt8>) -> R
  ) -> R {
    if hasPointerRepresentation {
      return body(UnsafeBufferPointer(
        start: utf8Start, count: utf8CodeUnitCount))
    } else {
      #if $Embedded
      fatalError("non-pointer representation not supported in embedded Swift")
      #else
      return unicodeScalar.withUTF8CodeUnits { body($0) }
      #endif
    }
  }
}

extension StaticString: _ExpressibleByBuiltinUnicodeScalarLiteral {

  @_effects(readonly)
  @_transparent
  public init(_builtinUnicodeScalarLiteral value: Builtin.Int32) {
    self = StaticString(unicodeScalar: value)
  }
}

extension StaticString: ExpressibleByUnicodeScalarLiteral {

  /// Creates an instance initialized to a single Unicode scalar.
  ///
  /// Do not call this initializer directly. It may be used by the compiler
  /// when you initialize a static string with a Unicode scalar.
  @_effects(readonly)
  @_transparent
  public init(unicodeScalarLiteral value: StaticString) {
    self = value
  }
}

extension StaticString: _ExpressibleByBuiltinExtendedGraphemeClusterLiteral {

  @_effects(readonly)
  @_transparent
  public init(
    _builtinExtendedGraphemeClusterLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
  ) {
    self = StaticString(
      _builtinStringLiteral: start,
      utf8CodeUnitCount: utf8CodeUnitCount,
      isASCII: isASCII
    )
  }
}

extension StaticString: ExpressibleByExtendedGraphemeClusterLiteral {

  /// Creates an instance initialized to a single character that is made up of
  /// one or more Unicode scalar values.
  ///
  /// Do not call this initializer directly. It may be used by the compiler
  /// when you initialize a static string using an extended grapheme cluster.
  @_effects(readonly)
  @_transparent
  public init(extendedGraphemeClusterLiteral value: StaticString) {
    self = value
  }
}

extension StaticString: _ExpressibleByBuiltinStringLiteral {

  @_effects(readonly)
  @_transparent
  public init(
    _builtinStringLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
  ) {
    self = StaticString(
      _start: start,
      utf8CodeUnitCount: utf8CodeUnitCount,
      isASCII: isASCII)
  }
}

extension StaticString: ExpressibleByStringLiteral {

  /// Creates an instance initialized to the value of a string literal.
  ///
  /// Do not call this initializer directly. It may be used by the compiler
  /// when you initialize a static string using a string literal.
  @_effects(readonly)
  @_transparent
  public init(stringLiteral value: StaticString) {
    self = value
  }
}

@_unavailableInEmbedded
extension StaticString: CustomStringConvertible {

  /// A textual representation of the static string.
  @_transparent
  public var description: String {
    guard hasPointerRepresentation else {
      return withUTF8Buffer { String._uncheckedFromUTF8($0) }
    }

    let buffer = UnsafeBufferPointer<UInt8>(
      start: utf8Start,
      count: utf8CodeUnitCount
    )

    return String(_StringGuts(buffer, isASCII: isASCII))
  }
}

@_unavailableInEmbedded
extension StaticString: CustomDebugStringConvertible {

  /// A textual representation of the static string, suitable for debugging.
  public var debugDescription: String {
    return self.description.debugDescription
  }
}

#if SWIFT_ENABLE_REFLECTION
extension StaticString: CustomReflectable {

  public var customMirror: Mirror {
    return Mirror(reflecting: description)
  }
}
#endif

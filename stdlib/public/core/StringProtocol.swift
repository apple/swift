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

/// A type that can represent a string as a collection of characters.
///
/// Do not declare new conformances to `StringProtocol`. Only the `String` and
/// `Substring` types in the standard library are valid conforming types.
public protocol StringProtocol
  : BidirectionalCollection,
  TextOutputStream, TextOutputStreamable,
  LosslessStringConvertible, ExpressibleByStringInterpolation,
  Hashable, Comparable
  where Iterator.Element == Character,
        Index == String.Index,
        SubSequence: StringProtocol,
        StringInterpolation == DefaultStringInterpolation
{
  associatedtype UTF8View: /*Bidirectional*/Collection
  where UTF8View.Element == UInt8, // Unicode.UTF8.CodeUnit
        UTF8View.Index == Index

  associatedtype UTF16View: BidirectionalCollection
  where UTF16View.Element == UInt16, // Unicode.UTF16.CodeUnit
        UTF16View.Index == Index

  associatedtype UnicodeScalarView: BidirectionalCollection
  where UnicodeScalarView.Element == Unicode.Scalar,
        UnicodeScalarView.Index == Index

  associatedtype SubSequence = Substring

  var utf8: UTF8View { get }
  var utf16: UTF16View { get }
  var unicodeScalars: UnicodeScalarView { get }

  func hasPrefix(_ prefix: String) -> Bool
  func hasSuffix(_ suffix: String) -> Bool

  func lowercased() -> String
  func uppercased() -> String

  /// Creates a string from the given Unicode code units in the specified
  /// encoding.
  ///
  /// - Parameters:
  ///   - codeUnits: A collection of code units encoded in the encoding
  ///     specified in `sourceEncoding`.
  ///   - sourceEncoding: The encoding in which `codeUnits` should be
  ///     interpreted.
  init<C: Collection, Encoding: Unicode.Encoding>(
    decoding codeUnits: C, as sourceEncoding: Encoding.Type
  )
    where C.Iterator.Element == Encoding.CodeUnit

  /// Creates a string from the null-terminated, UTF-8 encoded sequence of
  /// bytes at the given pointer.
  ///
  /// - Parameter nullTerminatedUTF8: A pointer to a sequence of contiguous,
  ///   UTF-8 encoded bytes ending just before the first zero byte.
  init(cString nullTerminatedUTF8: UnsafePointer<CChar>)

  /// Creates a string from the null-terminated sequence of bytes at the given
  /// pointer.
  ///
  /// - Parameters:
  ///   - nullTerminatedCodeUnits: A pointer to a sequence of contiguous code
  ///     units in the encoding specified in `sourceEncoding`, ending just
  ///     before the first zero code unit.
  ///   - sourceEncoding: The encoding in which the code units should be
  ///     interpreted.
  init<Encoding: Unicode.Encoding>(
    decodingCString nullTerminatedCodeUnits: UnsafePointer<Encoding.CodeUnit>,
    as sourceEncoding: Encoding.Type)

  /// Calls the given closure with a pointer to the contents of the string,
  /// represented as a null-terminated sequence of UTF-8 code units.
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withCString(_:)`. Do not store or return the pointer for
  /// later use.
  ///
  /// - Parameter body: A closure with a pointer parameter that points to a
  ///   null-terminated sequence of UTF-8 code units. If `body` has a return
  ///   value, that value is also used as the return value for the
  ///   `withCString(_:)` method. The pointer argument is valid only for the
  ///   duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  func withCString<Result>(
    _ body: (UnsafePointer<CChar>) throws -> Result) rethrows -> Result

  /// Calls the given closure with a pointer to the contents of the string,
  /// represented as a null-terminated sequence of code units.
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withCString(encodedAs:_:)`. Do not store or return the
  /// pointer for later use.
  ///
  /// - Parameters:
  ///   - body: A closure with a pointer parameter that points to a
  ///     null-terminated sequence of code units. If `body` has a return
  ///     value, that value is also used as the return value for the
  ///     `withCString(encodedAs:_:)` method. The pointer argument is valid
  ///     only for the duration of the method's execution.
  ///   - targetEncoding: The encoding in which the code units should be
  ///     interpreted.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  func withCString<Result, Encoding: Unicode.Encoding>(
    encodedAs targetEncoding: Encoding.Type,
    _ body: (UnsafePointer<Encoding.CodeUnit>) throws -> Result
  ) rethrows -> Result
}

extension StringProtocol {
  // TODO(String performance): Make a _SharedString for non-smol Substrings
  //
  // TODO(String performance): Provide a closure-based call with stack-allocated
  // _SharedString for non-smol Substrings
  //
  @inlinable
  public // @SPI(NSStringAPI.swift)
  var _ephemeralString: String {
    @inline(__always) get { return String(self) }
  }

  internal var _gutsSlice: _StringGutsSlice {
    @_specialize(where Self == String)
    @_specialize(where Self == Substring)
    @inline(__always) get {
      if let str = _specialize(self, for: String.self) {
        return _StringGutsSlice(str._guts)
      }
      if let subStr = _specialize(self, for: Substring.self) {
        return _StringGutsSlice(subStr._wholeGuts, subStr._offsetRange)
      }
      return _StringGutsSlice(String(self)._guts)
    }
  }

  @inlinable
  internal var _offsetRange: Range<Int> {
    @inline(__always) get {
      let start = startIndex
      let end = endIndex
      _internalInvariant(
        start.transcodedOffset == 0 && end.transcodedOffset == 0)
      return Range(_uncheckedBounds: (start._encodedOffset, end._encodedOffset))
    }
  }

  @inlinable
  internal var _wholeGuts: _StringGuts {
    @_specialize(where Self == String)
    @_specialize(where Self == Substring)
    @inline(__always) get {
      if let str = _specialize(self, for: String.self) {
        return str._guts
      }
      if let subStr = _specialize(self, for: Substring.self) {
        return subStr._wholeGuts
      }
      return String(self)._guts
    }
  }
}

// Contiguous UTF-8 strings
extension String {
  /// Returns whether this string is capable of providing access to
  /// validly-encoded UTF-8 contents in contiguous memory in O(1) time.
  ///
  /// Contiguous strings always operate in O(1) time for withUTF8 and always
  /// give a result for String.UTF8View.withContiguousStorageIfAvailable.
  /// Contiguous strings also benefit from fast-paths and better optimizations.
  ///
  @_alwaysEmitIntoClient
  public var isContiguousUTF8: Bool { return _guts.isFastUTF8 }

  /// If this string is not contiguous, make it so. If this mutates the string,
  /// it will invalidate any pre-existing indices.
  ///
  /// Complexity: O(n) if non-contiguous, O(1) if already contiguous
  ///
  @_alwaysEmitIntoClient
  public mutating func makeContiguousUTF8() {
    if _fastPath(isContiguousUTF8) { return }
    self = String._copying(self)
  }

  /// Runs `body` over the content of this string in contiguous memory. If this
  /// string is not contiguous, this will first make it contiguous, which will
  /// also speed up subsequent access. If this mutates the string,
  /// it will invalidate any pre-existing indices.
  ///
  /// Note that it is unsafe to escape the pointer provided to `body`. For
  /// example, strings of up to 15 UTF-8 code units in length may be represented
  /// in a small-string representation, and thus will be spilled into
  /// temporary stack space which is invalid after `withUTF8` finishes
  /// execution.
  ///
  /// Complexity: O(n) if non-contiguous, O(1) if already contiguous
  ///
  @_alwaysEmitIntoClient
  public mutating func withUTF8<R>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> R
  ) rethrows -> R {
    makeContiguousUTF8()
    return try _guts.withFastUTF8(body)
  }
}

// Contiguous UTF-8 strings
extension Substring {
  /// Returns whether this string is capable of providing access to
  /// validly-encoded UTF-8 contents in contiguous memory in O(1) time.
  ///
  /// Contiguous strings always operate in O(1) time for withUTF8 and always
  /// give a result for String.UTF8View.withContiguousStorageIfAvailable.
  /// Contiguous strings also benefit from fast-paths and better optimizations.
  ///
  @_alwaysEmitIntoClient
  public var isContiguousUTF8: Bool { return self.base.isContiguousUTF8 }

  /// If this string is not contiguous, make it so. If this mutates the
  /// substring, it will invalidate any pre-existing indices.
  ///
  /// Complexity: O(n) if non-contiguous, O(1) if already contiguous
  ///
  @_alwaysEmitIntoClient @inline(__always)
  public mutating func makeContiguousUTF8() {
    if isContiguousUTF8 { return }
    return _slowMakeContiguousUTF8()
  }

  @_alwaysEmitIntoClient // Swift 5.7
  @inline(never)
  internal mutating func _slowMakeContiguousUTF8() {
    _internalInvariant(!isContiguousUTF8)

    let scalarOffset = base.unicodeScalars.distance(
      from: base.startIndex, to: startIndex)
    let scalarCount = base.unicodeScalars.distance(
      from: startIndex, to: endIndex)

    let scalars = String._copying(base).unicodeScalars

    var newStart = scalars.index(scalars.startIndex, offsetBy: scalarOffset)
    var newEnd = scalars.index(newStart, offsetBy: scalarCount)

    if startIndex._isCharacterAligned { newStart = newStart._characterAligned }
    if endIndex._isCharacterAligned { newEnd = newEnd._characterAligned }

    self = Substring(_unchecked: scalars._guts, bounds: newStart ..< newEnd)
  }

  /// Runs `body` over the content of this substring in contiguous memory. If
  /// this substring is not contiguous, this will first make it contiguous,
  /// which will also speed up subsequent access. If this mutates the substring,
  /// it will invalidate any pre-existing indices.
  ///
  /// Note that it is unsafe to escape the pointer provided to `body`. For
  /// example, strings of up to 15 UTF-8 code units in length may be represented
  /// in a small-string representation, and thus will be spilled into
  /// temporary stack space which is invalid after `withUTF8` finishes
  /// execution.
  ///
  /// Complexity: O(n) if non-contiguous, O(1) if already contiguous
  ///
  @_alwaysEmitIntoClient
  public mutating func withUTF8<R>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> R
  ) rethrows -> R {
    makeContiguousUTF8()
    return try _wholeGuts.withFastUTF8(range: _offsetRange, body)
  }
}

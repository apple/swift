//===--- StringUTF8.swift - A UTF8 view of String -------------------------===//
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

// FIXME(ABI)#71 : The UTF-16 string view should have a custom iterator type to
// allow performance optimizations of linear traversals.

extension String {
  /// A view of a string's contents as a collection of UTF-8 code units.
  ///
  /// You can access a string's view of UTF-8 code units by using its `utf8`
  /// property. A string's UTF-8 view encodes the string's Unicode scalar
  /// values as 8-bit integers.
  ///
  ///     let flowers = "Flowers 💐"
  ///     for v in flowers.utf8 {
  ///         print(v)
  ///     }
  ///     // 70
  ///     // 108
  ///     // 111
  ///     // 119
  ///     // 101
  ///     // 114
  ///     // 115
  ///     // 32
  ///     // 240
  ///     // 159
  ///     // 146
  ///     // 144
  ///
  /// A string's Unicode scalar values can be up to 21 bits in length. To
  /// represent those scalar values using 8-bit integers, more than one UTF-8
  /// code unit is often required.
  ///
  ///     let flowermoji = "💐"
  ///     for v in flowermoji.unicodeScalars {
  ///         print(v, v.value)
  ///     }
  ///     // 💐 128144
  ///
  ///     for v in flowermoji.utf8 {
  ///         print(v)
  ///     }
  ///     // 240
  ///     // 159
  ///     // 146
  ///     // 144
  ///
  /// In the encoded representation of a Unicode scalar value, each UTF-8 code
  /// unit after the first is called a *continuation byte*.
  ///
  /// UTF8View Elements Match Encoded C Strings
  /// =========================================
  ///
  /// Swift streamlines interoperation with C string APIs by letting you pass a
  /// `String` instance to a function as an `Int8` or `UInt8` pointer. When you
  /// call a C function using a `String`, Swift automatically creates a buffer
  /// of UTF-8 code units and passes a pointer to that buffer. The code units
  /// of that buffer match the code units in the string's `utf8` view.
  ///
  /// The following example uses the C `strncmp` function to compare the
  /// beginning of two Swift strings. The `strncmp` function takes two
  /// `const char*` pointers and an integer specifying the number of characters
  /// to compare. Because the strings are identical up to the 14th character,
  /// comparing only those characters results in a return value of `0`.
  ///
  ///     let s1 = "They call me 'Bell'"
  ///     let s2 = "They call me 'Stacey'"
  ///
  ///     print(strncmp(s1, s2, 14))
  ///     // Prints "0"
  ///     print(String(s1.utf8.prefix(14))!)
  ///     // Prints "They call me '"
  ///
  /// Extending the compared character count to 15 includes the differing
  /// characters, so a nonzero result is returned.
  ///
  ///     print(strncmp(s1, s2, 15))
  ///     // Prints "-17"
  ///     print(String(s1.utf8.prefix(15))!)
  ///     // Prints "They call me 'B"
  @frozen
  public struct UTF8View: Sendable {
    @usableFromInline
    internal var _guts: _StringGuts

    @inlinable @inline(__always)
    internal init(_ guts: _StringGuts) {
      self._guts = guts
      _invariantCheck()
    }
  }
}

extension String.UTF8View {
  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    // TODO: Ensure index alignment
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension String.UTF8View: BidirectionalCollection {
  public typealias Index = String.Index

  public typealias Element = UTF8.CodeUnit

  /// The position of the first code unit if the UTF-8 view is
  /// nonempty.
  ///
  /// If the UTF-8 view is empty, `startIndex` is equal to `endIndex`.
  @inlinable @inline(__always)
  public var startIndex: Index { return _guts.startIndex }

  /// The "past the end" position---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// In an empty UTF-8 view, `endIndex` is equal to `startIndex`.
  @inlinable @inline(__always)
  public var endIndex: Index { return _guts.endIndex }

  /// Returns the next consecutive position after `i`.
  ///
  /// - Precondition: The next position is representable.
  @inlinable @inline(__always)
  public func index(after i: Index) -> Index {
    let i = _guts.ensureMatchingEncoding(i)
    if _fastPath(_guts.isFastUTF8) {
      // Note: deferred bounds check
      return i.strippingTranscoding.nextEncoded._knownUTF8
    }
    _precondition(i._encodedOffset < _guts.count,
      "String index is out of bounds")
    return _foreignIndex(after: i)
  }

  @inlinable @inline(__always)
  public func index(before i: Index) -> Index {
    let i = _guts.ensureMatchingEncoding(i)
    _precondition(!i.isZeroPosition, "String index is out of bounds")
    if _fastPath(_guts.isFastUTF8) {
      return i.strippingTranscoding.priorEncoded._knownUTF8
    }

    _precondition(i._encodedOffset <= _guts.count,
      "String index is out of bounds")
    return _foreignIndex(before: i)
  }

  @inlinable @inline(__always)
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    let i = _guts.ensureMatchingEncoding(i)
    if _fastPath(_guts.isFastUTF8) {
      let offset = n + i._encodedOffset
      _precondition(offset >= 0 && offset <= _guts.count,
        "String index is out of bounds")
      return Index(_encodedOffset: offset)._knownUTF8
    }

    return _foreignIndex(i, offsetBy: n)
  }

  @inlinable @inline(__always)
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    let i = _guts.ensureMatchingEncoding(i)
    if _fastPath(_guts.isFastUTF8) {
      // Check the limit: ignore limit if it precedes `i` (in the correct
      // direction), otherwise must not be beyond limit (in the correct
      // direction).
      let iOffset = i._encodedOffset
      let result = iOffset + n
      let limitOffset = limit._encodedOffset
      if n >= 0 {
        guard limitOffset < iOffset || result <= limitOffset else { return nil }
      } else {
        guard limitOffset > iOffset || result >= limitOffset else { return nil }
      }
      _precondition(result >= 0 && result <= _guts.count,
        "String index is out of bounds")
      return Index(_encodedOffset: result)._knownUTF8
    }

    return _foreignIndex(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable @inline(__always)
  public func distance(from i: Index, to j: Index) -> Int {
    let i = _guts.ensureMatchingEncoding(i)
    let j = _guts.ensureMatchingEncoding(j)
    if _fastPath(_guts.isFastUTF8) {
      return j._encodedOffset &- i._encodedOffset
    }
    _precondition(
      i._encodedOffset <= _guts.count && j._encodedOffset <= _guts.count,
      "String index is out of bounds")
    return _foreignDistance(from: i, to: j)
  }

  /// Accesses the code unit at the given position.
  ///
  /// The following example uses the subscript to print the value of a
  /// string's first UTF-8 code unit.
  ///
  ///     let greeting = "Hello, friend!"
  ///     let i = greeting.utf8.startIndex
  ///     print("First character's UTF-8 code unit: \(greeting.utf8[i])")
  ///     // Prints "First character's UTF-8 code unit: 72"
  ///
  /// - Parameter position: A valid index of the view. `position`
  ///   must be less than the view's end index.
  @inlinable @inline(__always)
  public subscript(i: Index) -> UTF8.CodeUnit {
    let i = _guts.ensureMatchingEncoding(i)
    _precondition(i._encodedOffset < _guts.count,
      "String index is out of bounds")
    return self[_unchecked: i]
  }

  @_alwaysEmitIntoClient @inline(__always)
  internal subscript(_unchecked i: Index) -> UTF8.CodeUnit {
    if _fastPath(_guts.isFastUTF8) {
      return _guts.withFastUTF8 { utf8 in utf8[_unchecked: i._encodedOffset] }
    }

    return _foreignSubscript(position: i)
  }
}

extension String.UTF8View: CustomStringConvertible {
  @inlinable @inline(__always)
  public var description: String { return String(_guts) }
}

extension String.UTF8View: CustomDebugStringConvertible {
  public var debugDescription: String {
    return "UTF8View(\(self.description.debugDescription))"
  }
}


extension String {
  /// A UTF-8 encoding of `self`.
  @inlinable
  public var utf8: UTF8View {
    @inline(__always) get { return UTF8View(self._guts) }
    set { self = String(newValue._guts) }
  }

  /// A contiguously stored null-terminated UTF-8 representation of the string.
  ///
  /// To access the underlying memory, invoke `withUnsafeBufferPointer` on the
  /// array.
  ///
  ///     let s = "Hello!"
  ///     let bytes = s.utf8CString
  ///     print(bytes)
  ///     // Prints "[72, 101, 108, 108, 111, 33, 0]"
  ///
  ///     bytes.withUnsafeBufferPointer { ptr in
  ///         print(strlen(ptr.baseAddress!))
  ///     }
  ///     // Prints "6"
  public var utf8CString: ContiguousArray<CChar> {
    @_effects(readonly) @_semantics("string.getUTF8CString")
    get {
      if _fastPath(_guts.isFastUTF8) {
        return _guts.withFastCChar {
          var tmp = ContiguousArray<CChar>()
          tmp.reserveCapacity($0.count + 1)
          tmp.append(contentsOf: $0)
          tmp.append(0)
          return tmp
        }
      }

      return _slowUTF8CString()
    }
  }

  @usableFromInline @inline(never) // slow-path
  internal func _slowUTF8CString() -> ContiguousArray<CChar> {
    var result = ContiguousArray<CChar>()
    result.reserveCapacity(self._guts.count + 1)
    for c in self.utf8 {
      result.append(CChar(bitPattern: c))
    }
    result.append(0)
    return result
  }

  /// Creates a string corresponding to the given sequence of UTF-8 code units.
  @available(swift, introduced: 4.0, message:
  "Please use failable String.init?(_:UTF8View) when in Swift 3.2 mode")
  @inlinable @inline(__always)
  public init(_ utf8: UTF8View) {
    self = String(utf8._guts)
  }
}

extension String.UTF8View {
  @inlinable @inline(__always)
  public var count: Int {
    if _fastPath(_guts.isFastUTF8) {
      return _guts.count
    }
    return _foreignCount()
  }
}

// Index conversions
extension String.UTF8View.Index {
  /// Creates an index in the given UTF-8 view that corresponds exactly to the
  /// specified `UTF16View` position.
  ///
  /// The following example finds the position of a space in a string's `utf16`
  /// view and then converts that position to an index in the string's
  /// `utf8` view.
  ///
  ///     let cafe = "Café 🍵"
  ///
  ///     let utf16Index = cafe.utf16.firstIndex(of: 32)!
  ///     let utf8Index = String.UTF8View.Index(utf16Index, within: cafe.utf8)!
  ///
  ///     print(Array(cafe.utf8[..<utf8Index]))
  ///     // Prints "[67, 97, 102, 195, 169]"
  ///
  /// If the position passed in `utf16Index` doesn't have an exact
  /// corresponding position in `utf8`, the result of the initializer is
  /// `nil`. For example, because UTF-8 and UTF-16 represent high Unicode code
  /// points differently, an attempt to convert the position of the trailing
  /// surrogate of a UTF-16 surrogate pair fails.
  ///
  /// The next example attempts to convert the indices of the two UTF-16 code
  /// points that represent the teacup emoji (`"🍵"`). The index of the lead
  /// surrogate is successfully converted to a position in `utf8`, but the
  /// index of the trailing surrogate is not.
  ///
  ///     let emojiHigh = cafe.utf16.index(after: utf16Index)
  ///     print(String.UTF8View.Index(emojiHigh, within: cafe.utf8))
  ///     // Prints "Optional(String.Index(...))"
  ///
  ///     let emojiLow = cafe.utf16.index(after: emojiHigh)
  ///     print(String.UTF8View.Index(emojiLow, within: cafe.utf8))
  ///     // Prints "nil"
  ///
  /// - Parameters:
  ///   - sourcePosition: A position in a `String` or one of its views.
  ///   - target: The `UTF8View` in which to find the new position.
  public init?(_ idx: String.Index, within target: String.UTF8View) {
    // Note: This method used to be inlinable until Swift 5.7.

    // As a special exception, we allow `idx` to be an UTF-16 index when `self`
    // is a UTF-8 string (or vice versa), to preserve compatibility with
    // (broken) code that keeps using indices from a bridged string after
    // converting the string to a native representation. Such indices are
    // invalid, but returning nil here can break code that appeared to work fine
    // for ASCII strings in Swift releases prior to 5.7.
    let idx = target._guts.ensureMatchingEncoding(idx)
    guard idx._encodedOffset <= target._guts.count else { return nil }

    if _slowPath(target._guts.isForeign) {
      guard idx._foreignIsWithin(target) else { return nil }
    } else {
      // All indices that are in range are valid, except sub-scalar UTF-16
      // indices pointing at trailing surrogates.
      guard idx.transcodedOffset == 0 else { return nil }
    }

    self = idx
  }
}

#if SWIFT_ENABLE_REFLECTION
// Reflection
extension String.UTF8View: CustomReflectable {
  /// Returns a mirror that reflects the UTF-8 view of a string.
  public var customMirror: Mirror {
    return Mirror(self, unlabeledChildren: self)
  }
}
#endif

//===--- Slicing Support --------------------------------------------------===//
/// In Swift 3.2, in the absence of type context,
///
///   someString.utf8[someString.utf8.startIndex..<someString.utf8.endIndex]
///
/// was deduced to be of type `String.UTF8View`.  Provide a more-specific
/// Swift-3-only `subscript` overload that continues to produce
/// `String.UTF8View`.
extension String.UTF8View {
  public typealias SubSequence = Substring.UTF8View

  @inlinable
  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> String.UTF8View.SubSequence {
    let r = _guts.validateSubscalarRange(r)
    return Substring.UTF8View(self, _bounds: r)
  }
}

extension String.UTF8View {
  /// Copies `self` into the supplied buffer.
  ///
  /// - Precondition: The memory in `self` is uninitialized. The buffer must
  ///   contain sufficient uninitialized memory to accommodate
  ///   `source.underestimatedCount`.
  ///
  /// - Postcondition: The `Pointee`s at `buffer[startIndex..<returned index]`
  ///   are initialized.
  @inlinable @inline(__always)
  public func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Iterator.Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Iterator.Element>.Index) {
    guard buffer.baseAddress != nil else {
        _preconditionFailure(
          "Attempt to copy string contents into nil buffer pointer")
    }
    guard let written = _guts.copyUTF8(into: buffer) else {
      _preconditionFailure(
        "Insufficient space allocated to copy string contents")
    }

    let it = String().utf8.makeIterator()
    return (it, buffer.index(buffer.startIndex, offsetBy: written))
  }
}

// Foreign string support
extension String.UTF8View {
  // Align a foreign UTF-16 index to a valid UTF-8 position. If there is a
  // transcoded offset already, this is already a valid UTF-8 position
  // (referring to a continuation byte) and returns `idx`. Otherwise, this will
  // scalar-align the index. This is needed because we may be passed a
  // non-scalar-aligned foreign index from the UTF16View.
  @_alwaysEmitIntoClient @inline(__always)
  internal func _utf8AlignForeignIndex(_ idx: String.Index) -> String.Index {
    _internalInvariant(_guts.isForeign)
    guard idx.transcodedOffset == 0 else { return idx }
    return _guts.scalarAlign(idx)
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(after idx: Index) -> Index {
    _internalInvariant(_guts.isForeign)
    _internalInvariant(idx._encodedOffset < _guts.count)

    let idx = _utf8AlignForeignIndex(idx)

    let (scalar, scalarLen) = _guts.foreignErrorCorrectedScalar(
      startingAt: idx.strippingTranscoding)
    let utf8Len = UTF8.width(scalar)

    if utf8Len == 1 {
      _internalInvariant(idx.transcodedOffset == 0)
      return idx.nextEncoded._scalarAligned._knownUTF16
    }

    // Check if we're still transcoding sub-scalar
    if idx.transcodedOffset < utf8Len - 1 {
      return idx.nextTranscoded._knownUTF16
    }

    // Skip to the next scalar
    _internalInvariant(idx.transcodedOffset == utf8Len - 1)
    return idx.encoded(offsetBy: scalarLen)._scalarAligned._knownUTF16
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(before idx: Index) -> Index {
    _internalInvariant(_guts.isForeign)
    _internalInvariant(idx._encodedOffset <= _guts.count)

    let idx = _utf8AlignForeignIndex(idx)

    if idx.transcodedOffset != 0 {
      _internalInvariant((1...3) ~= idx.transcodedOffset)
      return idx.priorTranscoded._knownUTF16
    }

    let (scalar, scalarLen) = _guts.foreignErrorCorrectedScalar(
      endingAt: idx.strippingTranscoding)
    let utf8Len = UTF8.width(scalar)
    return idx.encoded(
      offsetBy: -scalarLen
    ).transcoded(withOffset: utf8Len &- 1)._knownUTF16
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignSubscript(position idx: Index) -> UTF8.CodeUnit {
    _internalInvariant(_guts.isForeign)

    let idx = _utf8AlignForeignIndex(idx)

    let scalar = _guts.foreignErrorCorrectedScalar(
      startingAt: idx.strippingTranscoding).0
    let encoded = Unicode.UTF8.encode(scalar)._unsafelyUnwrappedUnchecked
    _internalInvariant(idx.transcodedOffset < 1+encoded.count)

    return encoded[
      encoded.index(encoded.startIndex, offsetBy: idx.transcodedOffset)]
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(_ i: Index, offsetBy n: Int) -> Index {
    _internalInvariant(_guts.isForeign)
    return _index(i, offsetBy: n)
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    _internalInvariant(_guts.isForeign)
    return _index(i, offsetBy: n, limitedBy: limit)
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignDistance(from i: Index, to j: Index) -> Int {
    _internalInvariant(_guts.isForeign)

    let i = _utf8AlignForeignIndex(i)
    let j = _utf8AlignForeignIndex(j)


    #if _runtime(_ObjC)
    // Currently, foreign means NSString
    let count = _guts._object.withCocoaObject {
      _cocoaStringUTF8Count($0, range: i._encodedOffset ..< j._encodedOffset)
    }
    if let count {
      // _cocoaStringUTF8Count gave us the scalar aligned count, but we still
      // need to compensate for sub-scalar indexing, e.g. if `i` is in the
      // middle of a two-byte UTF8 scalar.
      let refinedCount = (count - i.transcodedOffset) + j.transcodedOffset
      _internalInvariant(refinedCount == _distance(from: i, to: j))
      return refinedCount
    }
    #endif

    return _distance(from: i, to: j)
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignCount() -> Int {
    _internalInvariant(_guts.isForeign)
    return _foreignDistance(from: startIndex, to: endIndex)
  }
}

extension String.Index {
  @usableFromInline @inline(never) // opaque slow-path
  @_effects(releasenone)
  internal func _foreignIsWithin(_ target: String.UTF8View) -> Bool {
    _internalInvariant(target._guts.isForeign)
    return self == target._utf8AlignForeignIndex(self)
  }
}

extension String.UTF8View {
  @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    guard _guts.isFastUTF8 else { return nil }
    return try _guts.withFastUTF8(body)
  }
}

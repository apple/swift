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

extension String {
  // FIXME(strings): at least temporarily remove it to see where it was applied
  /// Creates a new string from the given substring.
  ///
  /// - Parameter substring: A substring to convert to a standalone `String`
  ///   instance.
  ///
  /// - Complexity: O(*n*), where *n* is the length of `substring`.
  @inlinable
  public init(_ substring: __shared Substring) {
    self = String._fromSubstring(substring)
  }
}

/// A slice of a string.
///
/// When you create a slice of a string, a `Substring` instance is the result.
/// Operating on substrings is fast and efficient because a substring shares
/// its storage with the original string. The `Substring` type presents the
/// same interface as `String`, so you can avoid or defer any copying of the
/// string's contents.
///
/// The following example creates a `greeting` string, and then finds the
/// substring of the first sentence:
///
///     let greeting = "Hi there! It's nice to meet you! 👋"
///     let endOfSentence = greeting.firstIndex(of: "!")!
///     let firstSentence = greeting[...endOfSentence]
///     // firstSentence == "Hi there!"
///
/// You can perform many string operations on a substring. Here, we find the
/// length of the first sentence and create an uppercase version.
///
///     print("'\(firstSentence)' is \(firstSentence.count) characters long.")
///     // Prints "'Hi there!' is 9 characters long."
///
///     let shoutingSentence = firstSentence.uppercased()
///     // shoutingSentence == "HI THERE!"
///
/// Converting a Substring to a String
/// ==================================
///
/// This example defines a `rawData` string with some unstructured data, and
/// then uses the string's `prefix(while:)` method to create a substring of
/// the numeric prefix:
///
///     let rawInput = "126 a.b 22219 zzzzzz"
///     let numericPrefix = rawInput.prefix(while: { "0"..."9" ~= $0 })
///     // numericPrefix is the substring "126"
///
/// When you need to store a substring or pass it to a function that requires a
/// `String` instance, you can convert it to a `String` by using the
/// `String(_:)` initializer. Calling this initializer copies the contents of
/// the substring to a new string.
///
///     func parseAndAddOne(_ s: String) -> Int {
///         return Int(s, radix: 10)! + 1
///     }
///     _ = parseAndAddOne(numericPrefix)
///     // error: cannot convert value...
///     let incrementedPrefix = parseAndAddOne(String(numericPrefix))
///     // incrementedPrefix == 127
///
/// Alternatively, you can convert the function that takes a `String` to one
/// that is generic over the `StringProtocol` protocol. The following code
/// declares a generic version of the `parseAndAddOne(_:)` function:
///
///     func genericParseAndAddOne<S: StringProtocol>(_ s: S) -> Int {
///         return Int(s, radix: 10)! + 1
///     }
///     let genericallyIncremented = genericParseAndAddOne(numericPrefix)
///     // genericallyIncremented == 127
///
/// You can call this generic function with an instance of either `String` or
/// `Substring`.
///
/// - Important: Don't store substrings longer than you need them to perform a
///   specific operation. A substring holds a reference to the entire storage
///   of the string it comes from, not just to the portion it presents, even
///   when there is no other reference to the original string. Storing
///   substrings may, therefore, prolong the lifetime of string data that is
///   no longer otherwise accessible, which can appear to be memory leakage.
@frozen
public struct Substring: Sendable {
  @usableFromInline
  internal var _slice: Slice<String>

  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal init(_unchecked slice: Slice<String>) {
    self._slice = slice
    _invariantCheck()
  }

  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal init(_unchecked guts: _StringGuts, bounds: Range<Index>) {
    self.init(_unchecked: Slice(base: String(guts), bounds: bounds))
  }

  @usableFromInline // This used to be @inlinable before 5.7
  @available(*, deprecated) // Use `init(_unchecked:)` in new code.
  internal init(_ slice: Slice<String>) {
    let r = slice._base._guts.validateScalarRange(slice._bounds)
    self._slice = Slice(base: slice._base, bounds: r)
    _invariantCheck()
  }

  @inline(__always)
  internal init(_ slice: _StringGutsSlice) {
    self.init(String(slice._guts)[slice.range])
  }

  /// Creates an empty substring.
  @inlinable @inline(__always)
  public init() {
    self._slice = Slice()
  }
}

extension Substring {
  /// Returns the underlying string from which this substring was derived.
  @_alwaysEmitIntoClient
  public var base: String { return _slice._base }

  @inlinable @inline(__always)
  internal var _wholeGuts: _StringGuts { _slice._base._guts }

  @inlinable @inline(__always)
  internal var _offsetRange: Range<Int> { _slice._bounds._encodedOffsetRange }

  @_alwaysEmitIntoClient @inline(__always)
  internal var _bounds: Range<Index> { _slice._bounds }
}

extension Substring {
  internal var _startIsCharacterAligned: Bool {
    startIndex._isCharacterAligned
  }
}

extension Substring {
  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    _internalInvariant(endIndex <= _wholeGuts.endIndex)
    _internalInvariant(
      _wholeGuts.hasMatchingEncoding(startIndex) &&
      _wholeGuts.hasMatchingEncoding(endIndex))
    _internalInvariant(
      startIndex._isScalarAligned && endIndex._isScalarAligned)
    self.base._invariantCheck()
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension Substring {
  /// Return true if and only if `i` is a valid index in this substring,
  /// that is to say, it exactly addresses one of the `Character`s in it.
  ///
  /// Note that if the start of the substring isn't `Character`-aligned in its
  /// base string, then the substring and the base may not share valid indices.
  internal func _isValidIndex(_ i: Index) -> Bool {
    guard
      _wholeGuts.hasMatchingEncoding(i),
      i >= startIndex,
      i <= endIndex,
      _wholeGuts.isOnUnicodeScalarBoundary(i)
    else {
      return false
    }
    let c = _wholeGuts.roundDownToNearestCharacter(
      i._scalarAligned, in: _bounds)
    return i == c
  }
}

extension Substring: StringProtocol {
  public typealias Index = String.Index
  public typealias SubSequence = Substring

  @inlinable @inline(__always)
  public var startIndex: Index { _slice._startIndex }

  @inlinable @inline(__always)
  public var endIndex: Index { _slice._endIndex }

  public func index(after i: Index) -> Index {
    // Note: Prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_slice.base.index(after:)`. Unfortunately, that approach isn't
    // compatible with SE-0180, as it allows Unicode scalars outside the
    // substring to affect grapheme breaking results within the substring. This
    // leads to Collection conformance issues when the `Substring`'s bounds do
    // not fall on grapheme boundaries in `base`.

    let i = _wholeGuts.validateCharacterIndex(i, in: _bounds)
    return _uncheckedIndex(after: i)
  }

  /// A version of `index(after:)` that assumes that the given index:
  ///
  /// - has the right encoding,
  /// - is within bounds, and
  /// - is character aligned within this substring.
  internal func _uncheckedIndex(after i: Index) -> Index {
    _internalInvariant(_wholeGuts.hasMatchingEncoding(i))
    _internalInvariant(i._isScalarAligned)
    _internalInvariant(i >= startIndex && i < endIndex)

    // Note: `i` must be `Character`-aligned within this substring, even if it
    // doesn't have the corresponding flag set.

    // TODO: known-ASCII fast path, single-scalar-grapheme fast path, etc.
    let stride = _characterStride(startingAt: i)

    // Make sure a cached stride cannot lead us beyond the substring's end
    // index. (This can happen if the substring's end isn't `Character`
    // aligned.)
    let nextOffset = Swift.min(
      i._encodedOffset &+ stride,
      endIndex._encodedOffset)
    let nextIndex = Index(_encodedOffset: nextOffset)._scalarAligned
    let nextStride = _characterStride(startingAt: nextIndex)

    var r = Index(
      encodedOffset: nextOffset, characterStride: nextStride)._scalarAligned

    // Don't set the `_isCharacterAligned` bit in indices of exotic substrings
    // whose startIndex isn't aligned on a grapheme cluster boundary. (Their
    // grapheme breaks may not match with those in `base`.)
    //
    // Note that we don't need to care about whether the end index is aligned
    // here.
    if _startIsCharacterAligned {
      r = r._characterAligned
    }

    return _wholeGuts.markEncoding(r)
  }

  public func index(before i: Index) -> Index {
    // Note: Prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_slice.base.index(before:)`. Unfortunately, that approach isn't
    // compatible with SE-0180, as it allows Unicode scalars outside the
    // substring to affect grapheme breaking results within the substring. This
    // leads to Collection conformance issues when the `Substring`'s bounds do
    // not fall on grapheme boundaries in `base`.

    let i = _wholeGuts.validateInclusiveCharacterIndex(i, in: _bounds)
    // Note: Aligning an index may move it closer towards the `startIndex`, so
    // this `i > startIndex` check needs to come after all the
    // alignment/validation work.
    _precondition(i > startIndex, "Substring index is out of bounds")

    return _uncheckedIndex(before: i)
  }

  /// A version of `index(before:)` that assumes that the given index:
  ///
  /// - has the right encoding,
  /// - is within bounds, and
  /// - is character aligned within this substring.
  internal func _uncheckedIndex(before i: Index) -> Index {
    _internalInvariant(_wholeGuts.hasMatchingEncoding(i))
    _internalInvariant(i._isScalarAligned)
    _internalInvariant(i > startIndex && i <= endIndex)

    // Note: `i` must be `Character`-aligned within this substring, even if it
    // doesn't have the corresponding flag set.

    // TODO: known-ASCII fast path, single-scalar-grapheme fast path, etc.
    let priorStride = _characterStride(endingAt: i)
    let priorOffset = i._encodedOffset &- priorStride
    _internalInvariant(priorOffset >= startIndex._encodedOffset)

    var r = Index(
      encodedOffset: priorOffset, characterStride: priorStride
    )._scalarAligned

    // Don't set the `_isCharacterAligned` bit in indices of exotic substrings
    // whose startIndex isn't aligned on a grapheme cluster boundary. (Their
    // grapheme breaks may not match with those in `base`.)
    if _startIsCharacterAligned {
      r = r._characterAligned
    }

    return _wholeGuts.markEncoding(r)
  }

  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    // Note: Prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_slice.base.index(_:offsetBy:)`. Unfortunately, that approach isn't
    // compatible with SE-0180, as it allows Unicode scalars outside the
    // substring to affect grapheme breaking results within the substring. This
    // leads to Collection conformance issues when the `Substring`'s bounds do
    // not fall on grapheme boundaries in `base`.

    // TODO: known-ASCII and single-scalar-grapheme fast path, etc.
    var i = _wholeGuts.validateInclusiveCharacterIndex(i, in: _bounds)
    if distance >= 0 {
      for _ in stride(from: 0, to: distance, by: 1) {
        _precondition(i < endIndex, "String index is out of bounds")
        i = _uncheckedIndex(after: i)
      }
    } else {
      for _ in stride(from: 0, to: distance, by: -1) {
        _precondition(i > startIndex, "String index is out of bounds")
        i = _uncheckedIndex(before: i)
      }
    }
    return i
  }

  public func index(
    _ i: Index, offsetBy distance: Int, limitedBy limit: Index
  ) -> Index? {
    // Note: Prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_slice.base.index(_:offsetBy:limitedBy:)`. Unfortunately, that approach
    // isn't compatible with SE-0180, as it allows Unicode scalars outside the
    // substring to affect grapheme breaking results within the substring. This
    // leads to Collection conformance issues when the `Substring`'s bounds do
    // not fall on grapheme boundaries in `base`.

    // Per SE-0180, `i` and `limit` are allowed to fall in between grapheme
    // breaks, in which case this function must still terminate without trapping
    // and return a result that makes sense.

    // Note: `limit` is intentionally not scalar (or character-) aligned to
    // ensure our behavior exactly matches the documentation above. We do need
    // to ensure it has a matching encoding, though. The same goes for `start`,
    // which is used to determine whether the limit applies at all.
    let limit = _wholeGuts.ensureMatchingEncoding(limit)
    let start = _wholeGuts.ensureMatchingEncoding(i)

    var i = _wholeGuts.validateInclusiveCharacterIndex(i, in: _bounds)
    if distance >= 0 {
      for _ in stride(from: 0, to: distance, by: 1) {
        guard limit < start || i < limit else { return nil }
        _precondition(i < endIndex, "String index is out of bounds")
        i = _uncheckedIndex(after: i)
      }
      guard limit < start || i <= limit else { return nil }
    } else {
      for _ in stride(from: 0, to: distance, by: -1) {
        guard limit > start || i > limit else { return nil }
        _precondition(i > startIndex, "String index is out of bounds")
        i = _uncheckedIndex(before: i)
      }
      guard limit > start || i >= limit else { return nil }
    }
    return i
  }

  public func distance(from start: Index, to end: Index) -> Int {
    // Note: Prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_slice.base.distance(from:to:)`. Unfortunately, that approach isn't
    // compatible with SE-0180, as it allows Unicode scalars outside the
    // substring to affect grapheme breaking results within the substring. This
    // leads to Collection conformance issues when the `Substring`'s bounds do
    // not fall on grapheme boundaries in `base`.

    // FIXME: Due to the `index(after:)` problem above, this function doesn't
    // always return consistent results when the given indices fall between
    // grapheme breaks -- swapping `start` and `end` may change the magnitude of
    // the result.

    let start = _wholeGuts.validateInclusiveCharacterIndex(start, in: _bounds)
    let end = _wholeGuts.validateInclusiveCharacterIndex(end, in: _bounds)

    // TODO: known-ASCII and single-scalar-grapheme fast path, etc.

    // Per SE-0180, `start` and `end` are allowed to fall in between Character
    // boundaries, in which case this function must still terminate without
    // trapping and return a result that makes sense.

    var i = start
    var count = 0
    if i < end {
      while i < end { // Note `<` instead of `==`
        count += 1
        i = _uncheckedIndex(after: i)
      }
    } else if i > end {
      while i > end { // Note `>` instead of `==`
        count -= 1
        i = _uncheckedIndex(before: i)
      }
    }
    return count
  }

  public subscript(i: Index) -> Character {
    // Note: SE-0180 requires us not to round `i` down to the nearest whole
    // `Character` boundary.
    let i = _wholeGuts.validateScalarIndex(i, in: _bounds)
    let stride = _characterStride(startingAt: i)
    // Don't let the subscript return data outside this substring.
    let endOffset = Swift.min(
      i._encodedOffset &+ stride,
      endIndex._encodedOffset)
    return _wholeGuts.errorCorrectedCharacter(
      startingAt: i._encodedOffset, endingAt: endOffset)
  }

  public mutating func replaceSubrange<C>(
    _ subrange: Range<Index>,
    with newElements: C
  ) where C: Collection, C.Iterator.Element == Iterator.Element {
    _replaceSubrange(subrange, with: newElements)
  }

  public mutating func replaceSubrange(
    _ subrange: Range<Index>, with newElements: Substring
  ) {
    _replaceSubrange(subrange, with: newElements)
  }

  internal mutating func _replaceSubrange<C: Collection>(
    _ subrange: Range<Index>, with newElements: C
  ) where C.Element == Element {
    // Note: SE-0180 requires us to use `subrange` bounds even if they aren't
    // `Character` aligned. (We still have to round things down to the nearest
    // scalar boundary, though, or we may generate ill-formed encodings.)
    let subrange = _wholeGuts.validateScalarRange(subrange, in: _bounds)

    // Replacing the range is easy -- we can just reuse `String`'s
    // implementation. However, we must also update `startIndex` and `endIndex`
    // to keep them valid & pointing to the same positions, which is somewhat
    // tricky.
    //
    // In Swift <=5.6, this used to forward to `Slice.replaceSubrange`, which
    // does it by counting elements, i.e., `Character`s. Unfortunately, that is
    // prone to return incorrect results in unusual cases, e.g.
    //
    //    - when the substring or the given subrange doesn't start/end on a
    //      character boundary, or
    //    - when the beginning/end of the replacement string ends up getting
    //      merged with the Character preceding/following the replaced range.
    //
    // The best way to avoid problems in these cases is to lower index
    // calculations to Unicode scalars (or below).
    _slice._base._guts.mutateSubrangeInSubstring(
      subrange: subrange,
      startIndex: &_slice._startIndex,
      endIndex: &_slice._endIndex,
      with: { $0.replaceSubrange(subrange, with: newElements) })

    _invariantCheck()
  }

  /// Creates a string from the given Unicode code units in the specified
  /// encoding.
  ///
  /// - Parameters:
  ///   - codeUnits: A collection of code units encoded in the encoding
  ///     specified in `sourceEncoding`.
  ///   - sourceEncoding: The encoding in which `codeUnits` should be
  ///     interpreted.
  @inlinable // specialization
  public init<C: Collection, Encoding: _UnicodeEncoding>(
    decoding codeUnits: C, as sourceEncoding: Encoding.Type
  ) where C.Iterator.Element == Encoding.CodeUnit {
    self.init(String(decoding: codeUnits, as: sourceEncoding))
  }

  /// Creates a string from the null-terminated, UTF-8 encoded sequence of
  /// bytes at the given pointer.
  ///
  /// - Parameter nullTerminatedUTF8: A pointer to a sequence of contiguous,
  ///   UTF-8 encoded bytes ending just before the first zero byte.
  public init(cString nullTerminatedUTF8: UnsafePointer<CChar>) {
    self.init(String(cString: nullTerminatedUTF8))
  }

  /// Creates a string from the null-terminated sequence of bytes at the given
  /// pointer.
  ///
  /// - Parameters:
  ///   - nullTerminatedCodeUnits: A pointer to a sequence of contiguous code
  ///     units in the encoding specified in `sourceEncoding`, ending just
  ///     before the first zero code unit.
  ///   - sourceEncoding: The encoding in which the code units should be
  ///     interpreted.
  @inlinable // specialization
  public init<Encoding: _UnicodeEncoding>(
    decodingCString nullTerminatedCodeUnits: UnsafePointer<Encoding.CodeUnit>,
    as sourceEncoding: Encoding.Type
  ) {
    self.init(
      String(decodingCString: nullTerminatedCodeUnits, as: sourceEncoding))
  }

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
  @inlinable // specialization
  public func withCString<Result>(
    _ body: (UnsafePointer<CChar>) throws -> Result) rethrows -> Result {
    // TODO(String performance): Detect when we cover the rest of a nul-
    // terminated String, and thus can avoid a copy.
    return try String(self).withCString(body)
  }

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
  @inlinable // specialization
  public func withCString<Result, TargetEncoding: _UnicodeEncoding>(
    encodedAs targetEncoding: TargetEncoding.Type,
    _ body: (UnsafePointer<TargetEncoding.CodeUnit>) throws -> Result
  ) rethrows -> Result {
    // TODO(String performance): Detect when we cover the rest of a nul-
    // terminated String, and thus can avoid a copy.
    return try String(self).withCString(encodedAs: targetEncoding, body)
  }
}

extension Substring {
  /// Return the length of the extended grapheme cluster that begins at `i`.
  ///
  /// This method assumes that `i` starts a new grapheme cluster; it does not
  /// verify that this is actually the case. If it isn't, then the return value
  /// reflects grapheme breaking results as if the string started at `i`,
  /// ignoring every preceding scalar.
  ///
  /// - Parameter `i`: An index within the bounds of this substring.
  internal func _characterStride(startingAt i: Index) -> Int {
    _internalInvariant(i._isScalarAligned)
    _internalInvariant(i._encodedOffset <= _wholeGuts.count)

    // If the index has a character stride, it reflects the stride assuming that
    // it addresses a `Character` boundary, which is exactly what we want.
    if let d = i.characterStride { return d }

    if i._encodedOffset == endIndex._encodedOffset { return 0 }

    // If we don't have cached information, we can simply invoke the
    // forward-only grapheme breaking algorithm. Note that this ignores the
    // Substring bounds; this is okay because this method never looks back at
    // preceding scalars, so it will place the boundary at the right position in
    // the substring. The reported stride may go above the end index, but that
    // case is handled in the caller.
    return _wholeGuts._opaqueCharacterStride(startingAt: i._encodedOffset)
  }

  
  /// Return the length of the extended grapheme cluster that ends with, or
  /// includes, `i`.
  ///
  /// This method does not assume that `i` addresses a grapheme cluster
  /// boundary; it looks back as far as necessary within the substring to find
  /// the right boundary location, stopping at the start index to prevent
  /// results that are inconsistent with `_characterStride(startingAt:)`.
  ///
  /// - Parameter `i`: An index within the bounds of this substring.
  internal func _characterStride(endingAt i: Index) -> Int {
    // Implicit precondition: `i` must be `Character`-aligned within this
    // substring, even if it doesn't have the corresponding flag set.

    _internalInvariant(i._isScalarAligned)
    _internalInvariant(i._encodedOffset <= _wholeGuts.count)

    if i == startIndex { return 0 }

    return _wholeGuts._opaqueCharacterStride(
      endingAt: i._encodedOffset, in: _offsetRange)
  }
}

#if SWIFT_ENABLE_REFLECTION
extension Substring: CustomReflectable {
 public var customMirror: Mirror { return String(self).customMirror }
}
#endif

extension Substring: CustomStringConvertible {
  @inlinable @inline(__always)
  public var description: String { return String(self) }
}

extension Substring: CustomDebugStringConvertible {
  public var debugDescription: String { return String(self).debugDescription }
}

extension Substring: LosslessStringConvertible {
  public init(_ content: String) {
    let range = Range(_uncheckedBounds: (content.startIndex, content.endIndex))
    self.init(_unchecked: Slice(base: content, bounds: range))
  }
}

extension Substring {
  @frozen
  public struct UTF8View: Sendable {
    @usableFromInline
    internal var _slice: Slice<String.UTF8View>

    /// Creates an instance that slices `base` at `_bounds`.
    @inlinable
    internal init(_ base: String.UTF8View, _bounds: Range<Index>) {
      _slice = Slice(base: base, bounds: _bounds)
    }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _wholeGuts: _StringGuts { _slice._base._guts }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _base: String.UTF8View { _slice._base }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _bounds: Range<Index> { _slice._bounds }
  }
}

extension Substring.UTF8View: BidirectionalCollection {
  public typealias Index = String.UTF8View.Index
  public typealias Indices = String.UTF8View.Indices
  public typealias Element = String.UTF8View.Element
  public typealias SubSequence = Substring.UTF8View

  @inlinable
  public var startIndex: Index { _slice._startIndex }

  @inlinable
  public var endIndex: Index { _slice._endIndex }

  @inlinable
  public subscript(index: Index) -> Element {
    let index = _wholeGuts.ensureMatchingEncoding(index)
    _precondition(index >= startIndex && index < endIndex,
      "String index is out of bounds")
    return _base[_unchecked: index]
  }

  @inlinable
  public var indices: Indices { return _slice.indices }

  @inlinable
  public func index(after i: Index) -> Index {
    // Note: deferred bounds check
    return _base.index(after: i)
  }

  @inlinable
  public func formIndex(after i: inout Index) {
    // Note: deferred bounds check
    _base.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    // Note: deferred bounds check
    return _base.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    // Note: deferred bounds check
    return _base.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return _base.distance(from: start, to: end)
  }

  @_alwaysEmitIntoClient
  @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try _slice.withContiguousStorageIfAvailable(body)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    // FIXME: This probably ought to ensure that all three indices have matching
    // encodings.
    _base._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    // FIXME: This probably ought to ensure that all three indices have matching
    // encodings.
    _base._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    // Note: deferred bounds check
    return _base.index(before: i)
  }

  @inlinable
  public func formIndex(before i: inout Index) {
    // Note: deferred bounds check
    _base.formIndex(before: &i)
  }

  @inlinable
  public subscript(r: Range<Index>) -> Substring.UTF8View {
    // FIXME(strings): tests.
    let r = _wholeGuts.validateSubscalarRange(r, in: _bounds)
    return Substring.UTF8View(_slice.base, _bounds: r)
  }
}

extension Substring {
  @inlinable
  public var utf8: UTF8View {
    get {
      // No need for index validation
      UTF8View(base.utf8, _bounds: _bounds)
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UTF8View) {
    // Note: We can trust that `content`'s bounds are valid, but they may not be
    // scalar aligned.
    let lower = content._wholeGuts.scalarAlign(content.startIndex)
    let upper = content._wholeGuts.scalarAlign(content.endIndex)
    let bounds = Range(_uncheckedBounds: (lower, upper))
    self.init(_unchecked: content._wholeGuts, bounds: bounds)
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// If `codeUnits` is an ill-formed code unit sequence, the result is `nil`.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-8 representation.
  public init?(_ codeUnits: Substring.UTF8View) {
    let guts = codeUnits._wholeGuts
    guard guts.isOnUnicodeScalarBoundary(codeUnits.startIndex),
          guts.isOnUnicodeScalarBoundary(codeUnits.endIndex) else {
      return nil
    }

    self = String(Substring(codeUnits))
  }
}

extension Substring {
  @frozen
  public struct UTF16View: Sendable {
    @usableFromInline
    internal var _slice: Slice<String.UTF16View>

    /// Creates an instance that slices `base` at `_bounds`.
    @inlinable
    internal init(_ base: String.UTF16View, _bounds: Range<Index>) {
      _slice = Slice(base: base, bounds: _bounds)
    }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _wholeGuts: _StringGuts { _slice._base._guts }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _base: String.UTF16View { _slice._base }

    @_alwaysEmitIntoClient @inline(__always)
    internal var _bounds: Range<Index> { _slice._bounds }
  }
}

extension Substring.UTF16View: BidirectionalCollection {
  public typealias Index = String.UTF16View.Index
  public typealias Indices = String.UTF16View.Indices
  public typealias Element = String.UTF16View.Element
  public typealias SubSequence = Substring.UTF16View

  @inlinable
  public var startIndex: Index { _slice._startIndex }

  @inlinable
  public var endIndex: Index { _slice._endIndex }

  @inlinable
  public subscript(index: Index) -> Element {
    let index = _wholeGuts.ensureMatchingEncoding(index)
    _precondition(index >= startIndex && index < endIndex,
      "String index is out of bounds")
    return _base[_unchecked: index]
  }

  @inlinable
  public var indices: Indices { return _slice.indices }

  @inlinable
  public func index(after i: Index) -> Index {
    // Note: deferred bounds check
    return _base.index(after: i)
  }

  @inlinable
  public func formIndex(after i: inout Index) {
    // Note: deferred bounds check
    _base.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    // Note: deferred bounds check
    return _base.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    // Note: deferred bounds check
    return _base.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return _base.distance(from: start, to: end)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    // FIXME: This probably ought to ensure that all three indices have matching
    // encodings.
    _base._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    // FIXME: This probably ought to ensure that all three indices have matching
    // encodings.
    _base._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    // Note: deferred bounds check
    return _base.index(before: i)
  }

  @inlinable
  public func formIndex(before i: inout Index) {
    // Note: deferred bounds check
    _base.formIndex(before: &i)
  }

  @inlinable
  public subscript(r: Range<Index>) -> Substring.UTF16View {
    let r = _wholeGuts.validateSubscalarRange(r, in: _bounds)
    return Substring.UTF16View(_slice.base, _bounds: r)
  }
}

extension Substring {
  @inlinable
  public var utf16: UTF16View {
    get {
      // No need for index validation
      UTF16View(base.utf16, _bounds: _bounds)
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UTF16View) {
    // Note: We can trust that `content`'s bounds are valid, but they may not be
    // scalar aligned.
    let lower = content._wholeGuts.scalarAlign(content.startIndex)
    let upper = content._wholeGuts.scalarAlign(content.endIndex)
    let bounds = Range(_uncheckedBounds: (lower, upper))
    self.init(_unchecked: content._wholeGuts, bounds: bounds)
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// If `codeUnits` is an ill-formed code unit sequence, the result is `nil`.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-16.
  public init?(_ codeUnits: Substring.UTF16View) {
    let guts = codeUnits._wholeGuts
    guard guts.isOnUnicodeScalarBoundary(codeUnits.startIndex),
          guts.isOnUnicodeScalarBoundary(codeUnits.endIndex) else {
      return nil
    }

    self = String(Substring(codeUnits))
  }
}
extension Substring {
  @frozen
  public struct UnicodeScalarView: Sendable {
    @usableFromInline
    internal var _slice: Slice<String.UnicodeScalarView>

    /// Creates an instance that slices `base` at `_bounds`.
    @_alwaysEmitIntoClient
    internal init(
      _unchecked base: String.UnicodeScalarView, bounds: Range<Index>
    ) {
      _slice = Slice(base: base, bounds: bounds)
      _invariantCheck()
    }

    /// Creates an instance that slices `base` at `_bounds`.
    @usableFromInline // This used to be inlinable before 5.7
    @available(*, deprecated, message: "Use `init(_unchecked:bounds)` in new code")
    internal init(_ base: String.UnicodeScalarView, _bounds: Range<Index>) {
      let start = base._guts.scalarAlign(_bounds.lowerBound)
      let end = base._guts.scalarAlign(_bounds.upperBound)
      _slice = Slice(base: base, bounds: Range(_uncheckedBounds: (start, end)))
    }
  }
}

extension Substring.UnicodeScalarView {
  @_alwaysEmitIntoClient @inline(__always)
  internal var _wholeGuts: _StringGuts { _slice._base._guts }

  @inline(__always)
  internal var _offsetRange: Range<Int> { _slice._bounds._encodedOffsetRange }

  @_alwaysEmitIntoClient
  @inline(__always)
  internal var _bounds: Range<Index> { _slice._bounds }
}

extension Substring.UnicodeScalarView {
  #if !INTERNAL_CHECKS_ENABLED
  @_alwaysEmitIntoClient @inline(__always)
  internal func _invariantCheck() {}
  #else
  @_alwaysEmitIntoClient
  @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    _internalInvariant(endIndex <= _wholeGuts.endIndex)
    _internalInvariant(
      _wholeGuts.hasMatchingEncoding(startIndex) &&
      _wholeGuts.hasMatchingEncoding(endIndex))
    _internalInvariant(
      startIndex._isScalarAligned && endIndex._isScalarAligned)
    _slice._base._invariantCheck()
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension Substring.UnicodeScalarView: BidirectionalCollection {
  public typealias Index = String.UnicodeScalarView.Index
  public typealias Indices = String.UnicodeScalarView.Indices
  public typealias Element = String.UnicodeScalarView.Element
  public typealias SubSequence = Substring.UnicodeScalarView

  //
  // Plumb slice operations through
  //
  @inlinable @inline(__always)
  public var startIndex: Index { _slice._startIndex }

  @inlinable @inline(__always)
  public var endIndex: Index { _slice._endIndex }

  @inlinable
  public subscript(index: Index) -> Element {
    let index = _wholeGuts.validateScalarIndex(index, in: _bounds)
    return _wholeGuts.errorCorrectedScalar(startingAt: index._encodedOffset).0
  }

  @inlinable
  public var indices: Indices {
    return _slice.indices
  }

  @inlinable
  public func index(after i: Index) -> Index {
    _slice._base.index(after: i)
  }

  @inlinable
  public func formIndex(after i: inout Index) {
    _slice._base.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    _slice._base.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    _slice._base.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    _slice._base.distance(from: start, to: end)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    _slice._base._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    _slice._base._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    _slice._base.index(before: i)
  }

  @inlinable
  public func formIndex(before i: inout Index) {
    _slice._base.formIndex(before: &i)
  }

  public subscript(r: Range<Index>) -> Substring.UnicodeScalarView {
    // Note: This used to be inlinable until Swift 5.7
    let r = _wholeGuts.validateScalarRange(r, in: _bounds)
    return Substring.UnicodeScalarView(_unchecked: _slice._base, bounds: r)
  }
}

extension Substring {
  @inlinable
  public var unicodeScalars: UnicodeScalarView {
    get {
      // No need to validate any indices.
      UnicodeScalarView(_unchecked: base.unicodeScalars, bounds: _bounds)
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UnicodeScalarView) {
      // No need to validate any indices.
    let slice = Slice(base: String(content._wholeGuts), bounds: content._bounds)
    self.init(_unchecked: slice)
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-16.
  public init(_ content: Substring.UnicodeScalarView) {
    self = String(Substring(content))
  }
}

extension Substring.UnicodeScalarView: RangeReplaceableCollection {
  @inlinable
  public init() { _slice = Slice.init() }

  public mutating func replaceSubrange<C: Collection>(
    _ subrange: Range<Index>, with replacement: C
  ) where C.Element == Element {
    let subrange = _wholeGuts.validateScalarRange(subrange, in: _bounds)

    // Replacing the range is easy -- we can just reuse `String`'s
    // implementation. However, we must also update `startIndex` and `endIndex`
    // to keep them valid & pointing to the same positions, which is somewhat
    // tricky.
    //
    // In Swift <=5.6, this used to forward to `Slice.replaceSubrange`, which
    // (incorrectly) assumes that indices before the replaced subrange are
    // preserved after the mutation. (This isn't true for strings, esp. when the
    // original value is UTF-16 encoded.)

    _slice._base._guts.mutateSubrangeInSubstring(
      subrange: subrange,
      startIndex: &_slice._startIndex,
      endIndex: &_slice._endIndex,
      with: { $0.replaceSubrange(subrange, with: replacement) })

    _invariantCheck()
  }
}

extension Substring: RangeReplaceableCollection {
  @_specialize(where S == String)
  @_specialize(where S == Substring)
  @_specialize(where S == Array<Character>)
  public init<S: Sequence>(_ elements: S)
  where S.Element == Character {
    if let str = elements as? String {
      self.init(str)
      return
    }
    if let subStr = elements as? Substring {
      self = subStr
      return
    }
    self.init(String(elements))
  }

  @inlinable // specialize
  public mutating func append<S: Sequence>(contentsOf elements: S)
  where S.Element == Character {
    var string = String(self)
    self = Substring() // Keep unique storage if possible
    string.append(contentsOf: elements)
    self = Substring(string)
  }
}

extension Substring {
  public func lowercased() -> String {
    return String(self).lowercased()
  }

  public func uppercased() -> String {
    return String(self).uppercased()
  }

  public func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> String {
    return try String(self.lazy.filter(isIncluded))
  }
}

extension Substring: TextOutputStream {
  public mutating func write(_ other: String) {
    append(contentsOf: other)
  }
}

extension Substring: TextOutputStreamable {
  @inlinable // specializable
  public func write<Target: TextOutputStream>(to target: inout Target) {
    target.write(String(self))
  }
}

extension Substring: ExpressibleByUnicodeScalarLiteral {
  @inlinable
  public init(unicodeScalarLiteral value: String) {
     self.init(value)
  }
}
extension Substring: ExpressibleByExtendedGraphemeClusterLiteral {
  @inlinable
  public init(extendedGraphemeClusterLiteral value: String) {
     self.init(value)
  }
}

extension Substring: ExpressibleByStringLiteral {
  @inlinable
  public init(stringLiteral value: String) {
     self.init(value)
  }
}

// String/Substring Slicing
extension String {
  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> Substring {
    var r = _guts.validateScalarRange(r)

    // Older binaries may generate `startIndex` without the
    // `_isCharacterAligned` flag. Compensate for that here so that substrings
    // that start at the beginning will never get the sad path in
    // `index(after:)`. Note that we don't need to do this for `upperBound` and
    // we don't need to compare against the `endIndex` -- those aren't nearly as
    // critical.
    if r.lowerBound._encodedOffset == 0 {
      r = Range(_uncheckedBounds:
        (r.lowerBound._characterAligned, r.upperBound))
    }

    return Substring(_unchecked: Slice(base: self, bounds: r))
  }
}

extension Substring {
  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> Substring {
    let r = _wholeGuts.validateScalarRange(r, in: _bounds)
    return Substring(_unchecked: Slice(base: base, bounds: r))
  }
}

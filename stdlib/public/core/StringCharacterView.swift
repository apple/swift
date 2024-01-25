//===--- StringCharacterView.swift - String's Collection of Characters ----===//
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
//
//  String is a collection of characters.
//
//===----------------------------------------------------------------------===//

import SwiftShims

extension String: BidirectionalCollection {
  public typealias SubSequence = Substring
  public typealias Element = Character

  /// The position of the first character in a nonempty string.
  ///
  /// In an empty string, `startIndex` is equal to `endIndex`.
  @inlinable @inline(__always)
  public var startIndex: Index { return _guts.startIndex }

  /// A string's "past the end" position---that is, the position one greater
  /// than the last valid subscript argument.
  ///
  /// In an empty string, `endIndex` is equal to `startIndex`.
  @inlinable @inline(__always)
  public var endIndex: Index { return _guts.endIndex }

  /// The number of characters in a string.
  ///
  /// To check whether a string is empty,
  /// use its `isEmpty` property instead of comparing `count` to zero.
  ///
  /// - Complexity: O(n), where n is the length of the string.
  @inline(__always)
  public var count: Int {
    return distance(from: startIndex, to: endIndex)
  }

  /// Return true if and only if `i` is a valid index in this substring,
  /// that is to say, it exactly addresses one of the `Character`s in it.
  internal func _isValidIndex(_ i: Index) -> Bool {
    return (
      _guts.hasMatchingEncoding(i)
      && i._encodedOffset <= _guts.count
      && _guts.isOnGraphemeClusterBoundary(i))
  }

  /// Returns the position immediately after the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  /// - Returns: The index value immediately after `i`.
  public func index(after i: Index) -> Index {
    let i = _guts.validateCharacterIndex(i)
    return _uncheckedIndex(after: i)
  }

  /// A version of `index(after:)` that assumes that the given index:
  ///
  /// - has the right encoding,
  /// - is within bounds, and
  /// - is scalar aligned.
  internal func _uncheckedIndex(after i: Index) -> Index {
    _internalInvariant(_guts.hasMatchingEncoding(i))
    _internalInvariant(i < endIndex)
    _internalInvariant(i._isCharacterAligned)

    // TODO: known-ASCII fast path, single-scalar-grapheme fast path, etc.
    let stride = _characterStride(startingAt: i)
    let nextOffset = i._encodedOffset &+ stride
    let nextIndex = Index(_encodedOffset: nextOffset)._characterAligned
    let nextStride = _characterStride(startingAt: nextIndex)
    let r = Index(encodedOffset: nextOffset, characterStride: nextStride)
    return _guts.markEncoding(r._characterAligned)
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  /// - Returns: The index value immediately before `i`.
  public func index(before i: Index) -> Index {
    // FIXME: This method used to not properly validate indices before 5.7;
    // temporarily allow older binaries to keep invoking undefined behavior as
    // before.
    let i = _guts.validateInclusiveCharacterIndex_5_7(i)

    // Note: Aligning an index may move it closer towards the `startIndex`, so
    // the `i > startIndex` check needs to come after rounding.
    _precondition(
      //ifLinkedOnOrAfter: .v5_7_0,
      i > startIndex, "String index is out of bounds")

    return _uncheckedIndex(before: i)
  }

  /// A version of `index(before:)` that assumes that the given index:
  ///
  /// - has the right encoding,
  /// - is within bounds, and
  /// - is character aligned.
  internal func _uncheckedIndex(before i: Index) -> Index {
    _internalInvariant(_guts.hasMatchingEncoding(i))
    _internalInvariant(i > startIndex && i <= endIndex)
    _internalInvariant(i._isCharacterAligned)

    // TODO: known-ASCII fast path, single-scalar-grapheme fast path, etc.
    let stride = _characterStride(endingAt: i)
    let priorOffset = i._encodedOffset &- stride

    let r = Index(encodedOffset: priorOffset, characterStride: stride)
    return _guts.markEncoding(r._characterAligned)
  }

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  ///
  ///     let s = "Swift"
  ///     let i = s.index(s.startIndex, offsetBy: 4)
  ///     print(s[i])
  ///     // Prints "t"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`.
  /// - Returns: An index offset by `distance` from the index `i`. If
  ///   `distance` is positive, this is the same value as the result of
  ///   `distance` calls to `index(after:)`. If `distance` is negative, this
  ///   is the same value as the result of `abs(distance)` calls to
  ///   `index(before:)`.
  /// - Complexity: O(*n*), where *n* is the absolute value of `distance`.
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    // Note: prior to Swift 5.7, this method used to be inlinable, forwarding to
    // `_index(_:offsetBy:)`.

    // TODO: known-ASCII and single-scalar-grapheme fast path, etc.

    // FIXME: This method used to not properly validate indices before 5.7;
    // temporarily allow older binaries to keep invoking undefined behavior as
    // before.
    var i = _guts.validateInclusiveCharacterIndex_5_7(i)

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

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  /// The operation doesn't require going beyond the limiting `s.endIndex`
  /// value, so it succeeds.
  ///
  ///     let s = "Swift"
  ///     if let i = s.index(s.startIndex, offsetBy: 4, limitedBy: s.endIndex) {
  ///         print(s[i])
  ///     }
  ///     // Prints "t"
  ///
  /// The next example attempts to retrieve an index six positions from
  /// `s.startIndex` but fails, because that distance is beyond the index
  /// passed as `limit`.
  ///
  ///     let j = s.index(s.startIndex, offsetBy: 6, limitedBy: s.endIndex)
  ///     print(j)
  ///     // Prints "nil"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection, unless the index passed as `limit` prevents offsetting
  /// beyond those bounds.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`.
  ///   - limit: A valid index of the collection to use as a limit. If
  ///     `distance > 0`, a limit that is less than `i` has no effect.
  ///     Likewise, if `distance < 0`, a limit that is greater than `i` has no
  ///     effect.
  /// - Returns: An index offset by `distance` from the index `i`, unless that
  ///   index would be beyond `limit` in the direction of movement. In that
  ///   case, the method returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the absolute value of `distance`.
  public func index(
    _ i: Index, offsetBy distance: Int, limitedBy limit: Index
  ) -> Index? {
    // Note: Prior to Swift 5.7, this function used to be inlinable, forwarding
    // to `BidirectionalCollection._index(_:offsetBy:limitedBy:)`.
    // Unfortunately, that approach isn't compatible with SE-0180, as it doesn't
    // support cases where `i` or `limit` aren't character aligned.

    // TODO: known-ASCII and single-scalar-grapheme fast path, etc.

    // Per SE-0180, `i` and `limit` are allowed to fall in between grapheme
    // breaks, in which case this function must still terminate without trapping
    // and return a result that makes sense.

    // Note: `limit` is intentionally not scalar (or character-) aligned to
    // ensure our behavior exactly matches the documentation above. We do need
    // to ensure it has a matching encoding, though. The same goes for `start`,
    // which is used to determine whether the limit applies at all.

    let limit = _guts.ensureMatchingEncoding(limit)
    let start = _guts.ensureMatchingEncoding(i)

    // FIXME: This method used to not properly validate indices before 5.7;
    // temporarily allow older binaries to keep invoking undefined behavior as
    // before.
    var i = _guts.validateInclusiveCharacterIndex_5_7(i)

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

  /// Returns the distance between two indices.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection. If `end` is equal to
  ///     `start`, the result is zero.
  /// - Returns: The distance between `start` and `end`.
  ///
  /// - Complexity: O(*n*), where *n* is the resulting distance.
  public func distance(from start: Index, to end: Index) -> Int {
    // Note: Prior to Swift 5.7, this function used to be inlinable, forwarding
    // to `BidirectionalCollection._distance(from:to:)`.

    // FIXME: This method used to not properly validate indices before 5.7;
    // temporarily allow older binaries to keep invoking undefined behavior as
    // before.
    let start = _guts.validateInclusiveCharacterIndex_5_7(start)
    let end = _guts.validateInclusiveCharacterIndex_5_7(end)

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
      while i > end { // Note `<` instead of `==`
        count -= 1
        i = _uncheckedIndex(before: i)
      }
    }
    return count
  }

  /// Accesses the character at the given position.
  ///
  /// You can use the same indices for subscripting a string and its substring.
  /// For example, this code finds the first letter after the first space:
  ///
  ///     let str = "Greetings, friend! How are you?"
  ///     let firstSpace = str.firstIndex(of: " ") ?? str.endIndex
  ///     let substr = str[firstSpace...]
  ///     if let nextCapital = substr.firstIndex(where: { $0 >= "A" && $0 <= "Z" }) {
  ///         print("Capital after a space: \(str[nextCapital])")
  ///     }
  ///     // Prints "Capital after a space: H"
  ///
  /// - Parameter i: A valid index of the string. `i` must be less than the
  ///   string's end index.
  public subscript(i: Index) -> Character {
    // Prior to Swift 5.7, this function used to be inlinable.

    // Note: SE-0180 requires us not to round `i` down to the nearest whole
    // `Character` boundary.
    let i = _guts.validateScalarIndex(i)
    let distance = _characterStride(startingAt: i)
    return _guts.errorCorrectedCharacter(
      startingAt: i._encodedOffset, endingAt: i._encodedOffset &+ distance)
  }

  /// Return the length of the `Character` starting at the given index, measured
  /// in encoded code units, and without looking back at any scalar that
  /// precedes `i`.
  ///
  /// Note: if `i` isn't `Character`-aligned, then this operation must still
  /// finish successfully and return the length of the grapheme cluster starting
  /// at `i` _as if the string started on that scalar_. (This can be different
  /// from the length of the whole character when the preceding scalars are
  /// present!)
  ///
  /// This method is called from inlinable `subscript` implementations in
  /// current and previous versions of the stdlib, which require this contract
  /// not to be violated.
  @usableFromInline
  @inline(__always)
  internal func _characterStride(startingAt i: Index) -> Int {
    // Prior to Swift 5.7, this function used to be inlinable.
    _internalInvariant_5_1(i._isScalarAligned)

    // Fast check if it's already been measured, otherwise check resiliently
    if let d = i.characterStride { return d }

    if i == endIndex { return 0 }

    return _guts._opaqueCharacterStride(startingAt: i._encodedOffset)
  }

  @usableFromInline
  @inline(__always)
  internal func _characterStride(endingAt i: Index) -> Int {
    // Prior to Swift 5.7, this function used to be inlinable.
    _internalInvariant_5_1(i._isScalarAligned)

    if i == startIndex { return 0 }

    return _guts._opaqueCharacterStride(endingAt: i._encodedOffset)
  }
}

extension String {
  @frozen
  public struct Iterator: IteratorProtocol, Sendable {
    @usableFromInline
    internal var _guts: _StringGuts

    @usableFromInline
    internal var _position: Int = 0

    @usableFromInline
    internal var _end: Int

    @inlinable
    internal init(_ guts: _StringGuts) {
      self._end = guts.count
      self._guts = guts
    }

    public mutating func next() -> Character? {
      // Prior to Swift 5.7, this function used to be inlinable.
      guard _fastPath(_position < _end) else { return nil }

      let len = _guts._opaqueCharacterStride(startingAt: _position)
      let nextPosition = _position &+ len
      let result = _guts.errorCorrectedCharacter(
        startingAt: _position, endingAt: nextPosition)
      _position = nextPosition
      return result
    }
  }

  @inlinable
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_guts)
  }
}


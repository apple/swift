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

import SwiftShims

/// CR and LF are common special cases in grapheme breaking logic
private var _CR: UInt8 { return 0x0d }
private var _LF: UInt8 { return 0x0a }

internal func _hasGraphemeBreakBetween(
  _ lhs: Unicode.Scalar, _ rhs: Unicode.Scalar
) -> Bool {

  // CR-LF is a special case: no break between these
  if lhs == Unicode.Scalar(_CR) && rhs == Unicode.Scalar(_LF) {
    return false
  }

  // Whether the given scalar, when it appears paired with another scalar
  // satisfying this property, has a grapheme break between it and the other
  // scalar.
  func hasBreakWhenPaired(_ x: Unicode.Scalar) -> Bool {
    // TODO: This doesn't generate optimal code, tune/re-write at a lower
    // level.
    //
    // NOTE: Order of case ranges affects codegen, and thus performance. All
    // things being equal, keep existing order below.
    switch x.value {
    // Unified CJK Han ideographs, common and some supplemental, amongst
    // others:
    //   U+3400 ~ U+A4CF
    case 0x3400...0xa4cf: return true

    // Repeat sub-300 check, this is beneficial for common cases of Latin
    // characters embedded within non-Latin script (e.g. newlines, spaces,
    // proper nouns and/or jargon, punctuation).
    //
    // NOTE: CR-LF special case has already been checked.
    case 0x0000...0x02ff: return true

    // Non-combining kana:
    //   U+3041 ~ U+3096
    //   U+30A1 ~ U+30FC
    case 0x3041...0x3096: return true
    case 0x30a1...0x30fc: return true

    // Non-combining modern (and some archaic) Cyrillic:
    //   U+0400 ~ U+0482 (first half of Cyrillic block)
    case 0x0400...0x0482: return true

    // Modern Arabic, excluding extenders and prependers:
    //   U+061D ~ U+064A
    case 0x061d...0x064a: return true

    // Precomposed Hangul syllables:
    //   U+AC00 ~ U+D7AF
    case 0xac00...0xd7af: return true

    // Common general use punctuation, excluding extenders:
    //   U+2010 ~ U+2029
    case 0x2010...0x2029: return true

    // CJK punctuation characters, excluding extenders:
    //   U+3000 ~ U+3029
    case 0x3000...0x3029: return true

    // Full-width forms:
    //   U+FF01 ~ U+FF9D
    case 0xFF01...0xFF9D: return true

    default: return false
    }
  }
  return hasBreakWhenPaired(lhs) && hasBreakWhenPaired(rhs)
}

extension _StringGuts {
  @inline(__always)
  internal func roundDownToNearestCharacter(
    _ i: String.Index
  ) -> String.Index {
    _internalInvariant(i._isScalarAligned)
    _internalInvariant(hasMatchingEncoding(i))
    _internalInvariant(i._encodedOffset <= count)

    let offset = i._encodedOffset
    if _fastPath(i._isCharacterAligned) { return i }
    if offset == 0 || offset == count { return i._characterAligned }
    return _slowRoundDownToNearestCharacter(i)
  }

  @inline(never)
  internal func _slowRoundDownToNearestCharacter(
    _ i: String.Index
  ) -> String.Index {
    let offset = i._encodedOffset
    let start = offset - _opaqueCharacterStride(endingAt: offset)
    let stride = _opaqueCharacterStride(startingAt: start)
    _internalInvariant(offset <= start + stride,
      "Grapheme breaking inconsistency")
    if offset >= start + stride {
      // Already aligned, or grapheme breaking returned an unexpected result.
      return i._characterAligned
    }
    let r = String.Index(encodedOffset: start, characterStride: stride)
    return markEncoding(r._characterAligned)
  }

  @inline(__always)
  internal func roundDownToNearestCharacter(
    _ i: String.Index,
    in bounds: Range<String.Index>
  ) -> String.Index {
    _internalInvariant(
      bounds.lowerBound._isScalarAligned && bounds.upperBound._isScalarAligned)
    _internalInvariant(
      hasMatchingEncoding(bounds.lowerBound)
      && hasMatchingEncoding(bounds.upperBound))
    _internalInvariant(bounds.upperBound <= endIndex)

    _internalInvariant(i._isScalarAligned)
    _internalInvariant(hasMatchingEncoding(i))
    _internalInvariant(i >= bounds.lowerBound && i <= bounds.upperBound)

    // We can only use the `_isCharacterAligned` bit if the start index is also
    // character-aligned.
    if _fastPath(
      bounds.lowerBound._isCharacterAligned && i._isCharacterAligned
    ) {
      return i
    }
    if i == bounds.lowerBound || i == bounds.upperBound { return i }
    return _slowRoundDownToNearestCharacter(i, in: bounds)
  }

  @inline(never)
  internal func _slowRoundDownToNearestCharacter(
    _ i: String.Index,
    in bounds: Range<String.Index>
  ) -> String.Index {
    let offset = i._encodedOffset

    let offsetBounds = bounds._encodedOffsetRange
    let prior =
      offset - _opaqueCharacterStride(endingAt: offset, in: offsetBounds)
    let stride = _opaqueCharacterStride(startingAt: prior)
    _internalInvariant(offset <= prior + stride,
      "Grapheme breaking inconsistency")
    if offset >= prior + stride {
      // Already aligned, or grapheme breaking returned an unexpected result.
      return i
    }
    var r = String.Index(encodedOffset: prior, characterStride: stride)
    if bounds.lowerBound._isCharacterAligned {
      r = r._characterAligned
    } else {
      r = r._scalarAligned
    }
    return markEncoding(r)
  }
}

extension _StringGuts {
  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func isOnGraphemeClusterBoundary(_ i: String.Index) -> Bool {
    if i._isCharacterAligned { return true }

    guard i.transcodedOffset == 0 else { return false }

    let offset = i._encodedOffset
    if offset == 0 || offset == self.count { return true }

    guard isOnUnicodeScalarBoundary(i) else { return false }

    let nearest = roundDownToNearestCharacter(i._scalarAligned)
    return i == nearest
  }
}

extension _StringGuts {
  /// Return the length of the extended grapheme cluster starting at offset `i`,
  /// assuming it falls on a grapheme cluster boundary.
  ///
  /// Note: This does not look behind at data preceding `i`, so if `i` is not on
  /// a grapheme cluster boundary, then it may return results that are
  /// inconsistent with `_opaqueCharacterStride(endingAt:)`. On the other hand,
  /// this behavior makes this suitable for use in substrings whose start index
  /// itself does not fall on a cluster boundary.
  @usableFromInline @inline(__always)
  @_effects(releasenone)
  internal func _opaqueCharacterStride(startingAt i: Int) -> Int {
    _internalInvariant(i < endIndex._encodedOffset)
    if isFastUTF8 {
      let fast = withFastUTF8 { utf8 in
        if i &+ 1 == utf8.count { return true }
        let pair = UnsafeRawPointer(
          utf8.baseAddress.unsafelyUnwrapped
        ).loadUnaligned(fromByteOffset: i, as: UInt16.self)
        //& 0x8080 == 0 is "both not ASCII", != 0x0A0D is "not CRLF"
        return pair & 0x8080 == 0 && pair != 0x0A0D
      }
      if _fastPath(fast) {
        _internalInvariant(_opaqueComplexCharacterStride(startingAt: i) == 1)
        return 1
      }
    }
    
    return _opaqueComplexCharacterStride(startingAt: i)
  }

  @_effects(releasenone) @inline(never)
  internal func _opaqueComplexCharacterStride(startingAt i: Int) -> Int {
    if _slowPath(isForeign) {
      return _foreignOpaqueCharacterStride(startingAt: i)
    }

    let nextIdx = withFastUTF8 { utf8 in
      nextBoundary(startingAt: i) { j in
        _internalInvariant(j >= 0)
        guard j < utf8.count else { return nil }
        let (scalar, len) = _decodeScalar(utf8, startingAt: j)
        return (scalar, j &+ len)
      }
    }

    return nextIdx &- i
  }

  /// Return the length of the extended grapheme cluster ending at offset `i`,
  /// or if `i` happens to be in the middle of a grapheme cluster, find and
  /// return the distance to its start.
  ///
  /// Note: unlike `_opaqueCharacterStride(startingAt:)`, this method always
  /// finds a correct grapheme cluster boundary.

  @usableFromInline @inline(__always)
  @_effects(releasenone)
  internal func _opaqueCharacterStride(endingAt i: Int) -> Int {
    if i <= 1 {
      return i
    }
    if isFastUTF8 {
      let fast = withFastUTF8 { utf8 in
        let pair = UnsafeRawPointer(
          utf8.baseAddress.unsafelyUnwrapped
        ).loadUnaligned(fromByteOffset: i &- 2, as: UInt16.self)
        //& 0x8080 == 0 is "both not ASCII", != 0x0A0D is "not CRLF"
        return pair & 0x8080 == 0 && pair != 0x0A0D
      }
      if _fastPath(fast) {
        _internalInvariant(_opaqueComplexCharacterStride(endingAt: i) == 1)
        return 1
      }
    }

    return _opaqueComplexCharacterStride(endingAt: i)
  }

  @_effects(releasenone) @inline(never)
  internal func _opaqueComplexCharacterStride(endingAt i: Int) -> Int {
    if _slowPath(isForeign) {
      return _foreignOpaqueCharacterStride(endingAt: i)
    }

    let previousIdx = withFastUTF8 { utf8 in
      previousBoundary(endingAt: i) { j in
        _internalInvariant(j <= utf8.count)
        guard j > 0 else { return nil }
        let (scalar, len) = _decodeScalar(utf8, endingAt: j)
        return (scalar, j &- len)
      }
    }

    return i &- previousIdx
  }

  /// Return the length of the extended grapheme cluster ending at offset `i` in
  /// bounds, or if `i` happens to be in the middle of a grapheme cluster, find
  /// and return the distance to its start.
  ///
  /// Note: unlike `_opaqueCharacterStride(startingAt:)`, this method always
  /// finds a correct grapheme cluster boundary within the substring defined by
  /// the specified bounds.
  @_effects(releasenone)
  internal func _opaqueCharacterStride(
    endingAt i: Int,
    in bounds: Range<Int>
  ) -> Int {
    _internalInvariant(i > bounds.lowerBound && i <= bounds.upperBound)
    if _slowPath(isForeign) {
      return _foreignOpaqueCharacterStride(endingAt: i, in: bounds)
    }

    let previousIdx = withFastUTF8 { utf8 in
      previousBoundary(endingAt: i) { j in
        _internalInvariant(j <= bounds.upperBound)
        guard j > bounds.lowerBound else { return nil }
        let (scalar, len) = _decodeScalar(utf8, endingAt: j)
        return (scalar, j &- len)
      }
    }

    _internalInvariant(bounds.contains(previousIdx))
    return i &- previousIdx
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(startingAt i: Int) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)

    let nextIdx = nextBoundary(startingAt: i) { j in
      _internalInvariant(j >= 0)
      guard j < count else { return nil }
      let scalars = String.UnicodeScalarView(self)
      let idx = String.Index(_encodedOffset: j)

      let scalar = scalars[idx]
      let nextIdx = scalars.index(after: idx)

      return (scalar, nextIdx._encodedOffset)
    }

    return nextIdx &- i
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(
    startingAt i: Int,
    in bounds: Range<Int>
  ) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)
    _internalInvariant(bounds.contains(i))

    let nextIdx = nextBoundary(startingAt: i) { j in
      _internalInvariant(j >= bounds.lowerBound)
      guard j < bounds.upperBound else { return nil }
      let scalars = String.UnicodeScalarView(self)
      let idx = String.Index(_encodedOffset: j)

      let scalar = scalars[idx]
      let nextIdx = scalars.index(after: idx)

      return (scalar, nextIdx._encodedOffset)
    }

    return nextIdx &- i
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(endingAt i: Int) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)

    let previousIdx = previousBoundary(endingAt: i) { j in
      _internalInvariant(j <= self.count)
      guard j > 0 else { return nil }
      let scalars = String.UnicodeScalarView(self)
      let idx = String.Index(_encodedOffset: j)

      let previousIdx = scalars.index(before: idx)
      let scalar = scalars[previousIdx]

      return (scalar, previousIdx._encodedOffset)
    }

    return i &- previousIdx
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(
    endingAt i: Int,
    in bounds: Range<Int>
  ) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)
    _internalInvariant(i > bounds.lowerBound && i <= bounds.upperBound)

    let previousIdx = previousBoundary(endingAt: i) { j in
      _internalInvariant(j <= bounds.upperBound)
      guard j > bounds.lowerBound else { return nil }
      let scalars = String.UnicodeScalarView(self)
      let idx = String.Index(_encodedOffset: j)

      let previousIdx = scalars.index(before: idx)

      let scalar = scalars[previousIdx]
      return (scalar, previousIdx._encodedOffset)
    }

    return i &- previousIdx
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }
}

extension Unicode.Scalar {
  fileprivate var _isLinkingConsonant: Bool {
    _swift_stdlib_isLinkingConsonant(value)
  }

  fileprivate var _isVirama: Bool {
    switch value {
    // Devanagari
    case 0x94D:
      return true
    // Bengali
    case 0x9CD:
      return true
    // Gujarati
    case 0xACD:
      return true
    // Oriya
    case 0xB4D:
      return true
    // Telugu
    case 0xC4D:
      return true
    // Malayalam
    case 0xD4D:
      return true

    default:
      return false
    }
  }
}

internal struct _GraphemeBreakingState: Sendable, Equatable {
  // When we're looking through an indic sequence, one of the requirements is
  // that there is at LEAST 1 Virama present between two linking consonants.
  // This value helps ensure that when we ultimately need to decide whether or
  // not to break that we've at least seen 1 when walking.
  var hasSeenVirama = false

  // When walking forwards in a string, we need to know whether or not we've
  // entered an emoji sequence to be able to eventually break after all of the
  // emoji's various extenders and zero width joiners. This bit allows us to
  // keep track of whether or not we're still in an emoji sequence when deciding
  // to break.
  var isInEmojiSequence = false

  // Similar to emoji sequences, we need to know not to break an Indic grapheme
  // sequence. This sequence is (potentially) composed of many scalars and isn't
  // as trivial as comparing two grapheme properties.
  var isInIndicSequence = false

  // When walking forward in a string, we need to not break on emoji flag
  // sequences. Emoji flag sequences are composed of 2 regional indicators, so
  // when we see our first (.regionalIndicator, .regionalIndicator) decision,
  // we need to know to return false in this case. However, if the next scalar
  // is another regional indicator, we reach the same decision rule, but in this
  // case we actually need to break there's a boundary between emoji flag
  // sequences.
  var shouldBreakRI = false
}

extension _GraphemeBreakingState: CustomStringConvertible {
  var description: String {
    var r = "["
    if hasSeenVirama { r += "V" }
    if isInEmojiSequence { r += "E" }
    if isInIndicSequence { r += "I" }
    if shouldBreakRI { r += "R" }
    r += "]"
    return r
  }
}

extension Unicode {
  /// A state machine for recognizing character (i.e., extended grapheme
  /// cluster) boundaries in an arbitrary series of Unicode scalars.
  ///
  /// To detect grapheme breaks in a sequence of Unicode scalars, feed each of
  /// them to the `hasBreak(before:)` method. The method returns true if the
  /// sequence has a grapheme break preceding the given value.
  ///
  /// The results produced by this state machine are guaranteed to match the way
  /// `String` splits its contents into `Character` values.
  @available(SwiftStdlib 5.8, *)
  public // SPI(Foundation) FIXME: We need API for this
  struct _CharacterRecognizer: Sendable {
    internal var _previous: Unicode.Scalar
    internal var _state: _GraphemeBreakingState

    /// Returns a non-nil value if it can be determined whether there is a
    /// grapheme break between `scalar1` and `scalar2` without knowing anything
    /// about the scalars that precede `scalar1`. This can optionally be used as
    /// a fast (but incomplete) test before spinning up a full state machine
    /// session.
    @_effects(releasenone)
    public static func quickBreak(
      between scalar1: Unicode.Scalar,
      and scalar2: Unicode.Scalar
    ) -> Bool? {
      if scalar1.value == 0xD, scalar2.value == 0xA {
        return false
      }
      if _hasGraphemeBreakBetween(scalar1, scalar2) {
        return true
      }
      return nil
    }

    /// Initialize a new character recognizer at the _start of text_ (sot)
    /// position.
    ///
    /// The resulting state machine will report a grapheme break on the
    /// first scalar that is fed to it.
    public init() {
      _state = _GraphemeBreakingState()
      // To avoid having to handle the empty case specially, we use NUL as the
      // placeholder before the first scalar. NUL is a control character, so per
      // rule GB5, it will induce an unconditional grapheme break before the
      // first actual scalar, emulating GB1.
      _previous = Unicode.Scalar(0 as UInt8)
    }

    /// Feeds the next scalar to the state machine, returning a Boolean value
    /// indicating whether it starts a new extended grapheme cluster.
    ///
    /// This method will always report a break the first time it is called
    /// on a newly initialized recognizer.
    ///
    /// The state machine does not carry information across character
    /// boundaries. I.e., if this method returns true, then `self` after the
    /// call is equivalent to feeding the same scalar to a newly initialized
    /// recognizer instance.
    @_effects(releasenone)
    public mutating func hasBreak(
      before next: Unicode.Scalar
    ) -> Bool {
      let r = _state.shouldBreak(between: _previous, and: next)
      if r {
        _state = _GraphemeBreakingState()
      }
      _previous = next
      return r
    }

    /// Decode the scalars in the given UTF-8 buffer and feed them to the
    /// recognizer up to and including the scalar following the first grapheme
    /// break. If the buffer contains a grapheme break, then this function
    /// returns the index range of the scalar that follows the first one;
    /// otherwise it returns `nil`.
    ///
    /// On return, the state of the recognizer is updated to reflect the scalars
    /// up to and including the returned one. You can detect additional grapheme
    /// breaks by feeding the recognizer subsequent data.
    ///
    /// - Parameter buffer: A buffer containing valid UTF-8 data, starting and
    ///    ending on Unicode scalar boundaries.
    ///
    /// - Parameter start: A valid index into `buffer`, addressing the first
    ///    code unit of a UTF-8 scalar in the buffer, or the end.
    ///
    /// - Returns: The index range of the scalar that follows the first grapheme
    ///    break in the buffer, if there is one. If the buffer contains no
    ///    grapheme breaks, then this function returns `nil`.
    ///
    /// - Warning: This function does not validate that the buffer contains
    ///    valid UTF-8 data; its behavior is undefined if given invalid input.
    @_effects(releasenone)
    public mutating func _firstBreak(
      inUncheckedUnsafeUTF8Buffer buffer: UnsafeBufferPointer<UInt8>,
      startingAt start: Int = 0
    ) -> Range<Int>? {
      var i = start
      while i < buffer.endIndex {
        let (next, n) = _decodeScalar(buffer, startingAt: i)
        if hasBreak(before: next) {
          return Range(_uncheckedBounds: (i, i &+ n))
        }
        i &+= n
      }
      return nil
    }
  }
}

@available(SwiftStdlib 5.9, *)
extension Unicode._CharacterRecognizer: Equatable {
  public static func ==(left: Self, right: Self) -> Bool {
    left._previous == right._previous && left._state == right._state
  }
}

@available(SwiftStdlib 5.9, *)
extension Unicode._CharacterRecognizer: CustomStringConvertible {
  public var description: String {
    return "\(_state)U+\(String(_previous.value, radix: 16, uppercase: true))"
  }
}


extension _StringGuts {
  // Returns the stride of the grapheme cluster starting at offset `index`,
  // assuming it is on a grapheme cluster boundary.
  //
  // This method never looks at data below `index`. If `index` isn't on a
  // grapheme cluster boundary, then the result may not be consistent with the
  // actual breaks in the string. `Substring` relies on this to generate the
  // right breaks if its start index isn't aligned on one -- in this case, the
  // substring's breaks may not match the ones in its base string.
  internal func nextBoundary(
    startingAt index: Int,
    nextScalar: (Int) -> (scalar: Unicode.Scalar, end: Int)?
  ) -> Int {
    _internalInvariant(index < endIndex._encodedOffset)

    // Note: If `index` in't already on a boundary, then starting with an empty
    // state here sometimes leads to this method returning results that diverge
    // from the true breaks in the string.
    var state = _GraphemeBreakingState()
    var (scalar, index) = nextScalar(index)!

    while true {
      guard let (scalar2, nextIndex) = nextScalar(index) else { break }
      if state.shouldBreak(between: scalar, and: scalar2) {
        break
      }
      index = nextIndex
      scalar = scalar2
    }

    return index
  }

  // Returns the stride of the grapheme cluster ending at offset `index`.
  //
  // This method uses `previousScalar` to looks back in the string as far as
  // necessary to find a correct grapheme cluster boundary, whether or not
  // `index` happens to be on a boundary itself.
  internal func previousBoundary(
    endingAt index: Int,
    previousScalar: (Int) -> (scalar: Unicode.Scalar, start: Int)?
  ) -> Int {
    // FIXME: This requires potentially arbitrary lookback in each iteration,
    // leading to quadratic behavior in some edge cases. Ideally lookback should
    // only be done once per cluster (or in the case of RI sequences, once per
    // flag sequence). One way to avoid most quadratic behavior is to replace
    // this implementation with a scheme that first searches backwards for a
    // safe point then iterates forward using the regular `shouldBreak` until we
    // reach `index`, as recommended in section 6.4 of TR#29.
    //
    // https://www.unicode.org/reports/tr29/#Random_Access

    var (scalar2, index) = previousScalar(index)!

    while true {
      guard let (scalar1, previousIndex) = previousScalar(index) else { break }
      if shouldBreakWithLookback(
        between: scalar1, and: scalar2, at: index, with: previousScalar
      ) {
        break
      }
      index = previousIndex
      scalar2 = scalar1
    }

    return index
  }
}

extension _GraphemeBreakingState {
  // Return true if there is an extended grapheme cluster boundary between two
  // scalars, based on state information previously collected about preceding
  // scalars.
  //
  // This method never looks at scalars other than the two that are explicitly
  // passed to it. The `state` parameter is assumed to hold all contextual
  // information necessary to make a correct decision; it gets updated with more
  // data as needed.
  //
  // This is based on the Unicode Annex #29 for [Grapheme Cluster Boundary
  // Rules](https://unicode.org/reports/tr29/#Grapheme_Cluster_Boundary_Rules).
  internal mutating func shouldBreak(
    between scalar1: Unicode.Scalar,
    and scalar2: Unicode.Scalar
  ) -> Bool {
    // GB3
    if scalar1.value == 0xD, scalar2.value == 0xA {
      return false
    }

    if _hasGraphemeBreakBetween(scalar1, scalar2) {
      return true
    }

    let x = Unicode._GraphemeBreakProperty(from: scalar1)
    
    // GB4 handled here because we don't need to know `y` for this csae
    if x == .control {
      return true
    }
    
    // This variable and the defer statement help toggle the isInEmojiSequence
    // state variable to false after every decision of 'shouldBreak'. If we
    // happen to see a rhs .extend or .zwj, then it's a signal that we should
    // continue treating the current grapheme cluster as an emoji sequence.
    var enterEmojiSequence = false

    // Very similar to emoji sequences, but for Indic grapheme sequences.
    var enterIndicSequence = false

    defer {
      self.isInEmojiSequence = enterEmojiSequence
      self.isInIndicSequence = enterIndicSequence
    }
    
    let y = Unicode._GraphemeBreakProperty(from: scalar2)

    switch (x, y) {

    // Fast path: If we know our scalars have no properties the decision is
    //            trivial and we don't need to crawl to the default statement.
    case (.any, .any):
      return true

    // (GB4 is handled above)

    // GB5
    case (_, .control):
      return true

    // GB6
    case (.l, .l),
         (.l, .v),
         (.l, .lv),
         (.l, .lvt):
      return false

    // GB7
    case (.lv, .v),
         (.v, .v),
         (.lv, .t),
         (.v, .t):
      return false

    // GB8
    case (.lvt, .t),
         (.t, .t):
      return false

    // GB9 (partial GB11)
    case (_, .extend),
         (_, .zwj):

      // Prepare for recognizing GB11, by remembering if we're in an emoji
      // sequence.
      //
      //   GB11: Extended_Pictographic Extend* ZWJ × Extended_Pictographic
      //
      // If our left-side scalar is a pictograph, then it starts a new emoji
      // sequence; the sequence continues through subsequent extend/extend and
      // extend/zwj pairs.
      if (
        x == .extendedPictographic || (self.isInEmojiSequence && x == .extend)
      ) {
        enterEmojiSequence = true
      }

      // If we're currently in an indic sequence (or if our lhs is a linking
      // consonant), then this check and everything underneath ensures that
      // we continue being in one and may check if this extend is a Virama.
      if self.isInIndicSequence || scalar1._isLinkingConsonant {
        if y == .extend {
          // If our extend's CCC is 0, then this rule does not apply.
          guard scalar2.properties.canonicalCombiningClass != .notReordered else {
            return false
          }
        }

        enterIndicSequence = true

        if scalar2._isVirama {
          self.hasSeenVirama = true
        }
      }

      return false

    // GB9a
    case (_, .spacingMark):
      return false

    // GB9b
    case (.prepend, _):
      return false

    // GB11
    case (.zwj, .extendedPictographic):
      return !self.isInEmojiSequence

    // GB12 & GB13
    case (.regionalIndicator, .regionalIndicator):
      defer {
        self.shouldBreakRI.toggle()
      }

      return self.shouldBreakRI

    // GB999
    default:
      // GB9c
      if
        self.isInIndicSequence,
        self.hasSeenVirama,
        scalar2._isLinkingConsonant
      {
        self.hasSeenVirama = false
        return false
      }

      return true
    }
  }
}

extension _StringGuts {
  // Return true if there is an extended grapheme cluster boundary between two
  // scalars, with no previous knowledge about preceding scalars.
  //
  // This method looks back as far as it needs to determine the correct
  // placement of boundaries.
  //
  // This is based off of the Unicode Annex #29 for [Grapheme Cluster Boundary
  // Rules](https://unicode.org/reports/tr29/#Grapheme_Cluster_Boundary_Rules).
  internal func shouldBreakWithLookback(
    between scalar1: Unicode.Scalar,
    and scalar2: Unicode.Scalar,
    at index: Int,
    with previousScalar: (Int) -> (scalar: Unicode.Scalar, start: Int)?
  ) -> Bool {
    // GB3
    if scalar1.value == 0xD, scalar2.value == 0xA {
      return false
    }

    if _hasGraphemeBreakBetween(scalar1, scalar2) {
      return true
    }

    let x = Unicode._GraphemeBreakProperty(from: scalar1)
    let y = Unicode._GraphemeBreakProperty(from: scalar2)

    switch (x, y) {

    // Fast path: If we know our scalars have no properties the decision is
    //            trivial and we don't need to crawl to the default statement.
    case (.any, .any):
      return true

    // GB4
    case (.control, _):
      return true

    // GB5
    case (_, .control):
      return true

    // GB6
    case (.l, .l),
         (.l, .v),
         (.l, .lv),
         (.l, .lvt):
      return false

    // GB7
    case (.lv, .v),
         (.v, .v),
         (.lv, .t),
         (.v, .t):
      return false

    // GB8
    case (.lvt, .t),
         (.t, .t):
      return false

    // GB9 (partial GB11)
    case (_, .extend),
         (_, .zwj):
      return false

    // GB9a
    case (_, .spacingMark):
      return false

    // GB9b
    case (.prepend, _):
      return false

    // GB11
    case (.zwj, .extendedPictographic):
      return !checkIfInEmojiSequence(at: index, with: previousScalar)

    // GB12 & GB13
    case (.regionalIndicator, .regionalIndicator):
      return countRIs(at: index, with: previousScalar)

    // GB999
    default:
      // GB9c
      switch (x, scalar2._isLinkingConsonant) {
      case (.extend, true):
        guard scalar1.properties.canonicalCombiningClass != .notReordered else {
          return true
        }
        return !checkIfInIndicSequence(at: index, with: previousScalar)

      case (.zwj, true):
        return !checkIfInIndicSequence(at: index, with: previousScalar)

      default:
        return true
      }
    }
  }

  // When walking backwards, it's impossible to know whether we were in an emoji
  // sequence without walking further backwards. This walks the string backwards
  // enough until we figure out whether or not to break our
  // (.zwj, .extendedPictographic) question. For example:
  //
  // Scalar view #1:
  //
  //     [.control, .zwj, .extendedPictographic]
  //                     ^
  //                     | = To determine whether or not we break here, we need
  //                         to see the previous scalar's grapheme property.
  //          ^
  //          | = This is neither .extendedPictographic nor .extend, thus we
  //              were never in an emoji sequence, so break between the .zwj
  //              and .extendedPictographic.
  //
  // Scalar view #2:
  //
  //     [.extendedPictographic, .zwj, .extendedPictographic]
  //                                  ^
  //                                  | = Same as above, move backwards one to
  //                                      view the previous scalar's property.
  //                ^
  //                | = This is an .extendedPictographic, so this indicates that
  //                    we are in an emoji sequence, so we should NOT break
  //                    between the .zwj and .extendedPictographic.
  //
  // Scalar view #3:
  //
  //     [.extendedPictographic, .extend, .extend, .zwj, .extendedPictographic]
  //                                                    ^
  //                                                    | = Same as above
  //                                         ^
  //                                         | = This is an .extend which means
  //                                             there is a potential emoji
  //                                             sequence, walk further backwards
  //                                             to find an .extendedPictographic.
  //
  //                               <-- = Another extend, go backwards more.
  //                ^
  //                | = We found our starting .extendedPictographic letting us
  //                    know that we are in an emoji sequence so our initial
  //                    break question is answered as NO.
  internal func checkIfInEmojiSequence(
    at index: Int,
    with previousScalar: (Int) -> (scalar: Unicode.Scalar, start: Int)?
  ) -> Bool {
    guard var i = previousScalar(index)?.start else { return false }
    while let prev = previousScalar(i) {
      i = prev.start
      let gbp = Unicode._GraphemeBreakProperty(from: prev.scalar)

      switch gbp {
      case .extend:
        continue
      case .extendedPictographic:
        return true
      default:
        return false
      }
    }
    return false
  }

  // When walking backwards, it's impossible to know whether we break when we
  // see our first ((.extend|.zwj), .linkingConsonant) without walking
  // further backwards. This walks the string backwards enough until we figure
  // out whether or not to break this indic sequence. For example:
  //
  // Scalar view #1:
  //
  //     [.virama, .extend, .linkingConsonant]
  //                       ^
  //                       | = To be able to know whether or not to break these
  //                           two, we need to walk backwards to determine if
  //                           this is a legitimate indic sequence.
  //      ^
  //      | = The scalar sequence ends without a starting linking consonant,
  //          so this is in fact not an indic sequence, so we can break the two.
  //
  // Scalar view #2:
  //
  //     [.linkingConsonant, .virama, .extend, .linkingConsonant]
  //                                          ^
  //                                          | = Same as above
  //                            ^
  //                            | = This is a virama, so we at least have seen
  //                                1 to be able to return true if we see a
  //                                linking consonant later.
  //         ^
  //         | = Is a linking consonant and we've seen a virama, so this is a
  //             legitimate indic sequence, so do NOT break the initial question.
  internal func checkIfInIndicSequence(
    at index: Int,
    with previousScalar: (Int) -> (scalar: Unicode.Scalar, start: Int)?
  ) -> Bool {
    guard let p = previousScalar(index) else { return false }

    var hasSeenVirama = p.scalar._isVirama
    var i = p.start

    while let (scalar, prev) = previousScalar(i) {
      i = prev
      let gbp = Unicode._GraphemeBreakProperty(from: scalar)

      switch (gbp, scalar._isLinkingConsonant) {
      case (.extend, false):
        guard scalar.properties.canonicalCombiningClass != .notReordered else {
          return false
        }

        if scalar._isVirama {
          hasSeenVirama = true
        }

      case (.zwj, false):
        continue

      // LinkingConsonant
      case (_, true):
        return hasSeenVirama

      default:
        return false
      }
    }
    return false
  }

  // When walking backwards, it's impossible to know whether we break when we
  // see our first (.regionalIndicator, .regionalIndicator) without walking
  // further backwards. This walks the string backwards enough until we figure
  // out whether or not to break these RIs. For example:
  //
  // Scalar view #1:
  //
  //     [.control, .regionalIndicator, .regionalIndicator]
  //                                   ^
  //                                   | = To be able to know whether or not to
  //                                       break these two, we need to walk
  //                                       backwards to determine if there were
  //                                       any previous .regionalIndicators in
  //                                       a row.
  //         ^
  //         | = Not a .regionalIndicator, so our total riCount is 0 and 0 is
  //             even thus we do not break.
  //
  // Scalar view #2:
  //
  //     [.control, .regionalIndicator, .regionalIndicator, .regionalIndicator]
  //                                                       ^
  //                                                       | = Same as above
  //                         ^
  //                         | = This is a .regionalIndicator, so continue
  //                             walking backwards for more of them. riCount is
  //                             now equal to 1.
  //         ^
  //         | = Not a .regionalIndicator. riCount = 1 which is odd, so break
  //             the last two .regionalIndicators.
  internal func countRIs(
    at index: Int,
    with previousScalar: (Int) -> (scalar: Unicode.Scalar, start: Int)?
  ) -> Bool {
    guard let p = previousScalar(index) else { return false }
    var i = p.start
    var riCount = 0
    while let p = previousScalar(i) {
      i = p.start

      let gbp = Unicode._GraphemeBreakProperty(from: p.scalar)
      guard gbp == .regionalIndicator else {
        break
      }

      riCount += 1
    }
    return riCount & 1 != 0
  }
}

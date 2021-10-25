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
  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func isOnGraphemeClusterBoundary(_ i: String.Index) -> Bool {
    guard i.transcodedOffset == 0 else { return false }

    let offset = i._encodedOffset
    if offset == 0 || offset == self.count { return true }

    guard isOnUnicodeScalarBoundary(i) else { return false }

    let str = String(self)
    return i == str.index(before: str.index(after: i))
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _opaqueCharacterStride(startingAt i: Int) -> Int {
    if _slowPath(isForeign) {
      return _foreignOpaqueCharacterStride(startingAt: i)
    }

    return self.withFastUTF8 { utf8 in
      let (sc1, len) = _decodeScalar(utf8, startingAt: i)
      if i &+ len == utf8.endIndex {
        // Last scalar is last grapheme
        return len
      }
      let (sc2, _) = _decodeScalar(utf8, startingAt: i &+ len)
      if _fastPath(_hasGraphemeBreakBetween(sc1, sc2)) {
        return len
      }

      let idx = String.Index(_encodedOffset: i)
      let walker = Unicode._GraphemeWalker(String(self).unicodeScalars)
      let nextIdx = walker.nextBoundary(startingAt: idx)
      return nextIdx._encodedOffset &- i
    }
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(startingAt i: Int) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)

    // TODO(String performance): Faster to do it from a pointer directly
    let count = _object.largeCount

    let startIdx = String.Index(_encodedOffset: i)
    let (sc1, len) = foreignErrorCorrectedScalar(startingAt: startIdx)
    if i &+ len == count {
      // Last scalar is last grapheme
      return len
    }
    let (sc2, _) = foreignErrorCorrectedScalar(
      startingAt: startIdx.encoded(offsetBy: len))
    if _fastPath(_hasGraphemeBreakBetween(sc1, sc2)) {
      return len
    }

    let walker = Unicode._GraphemeWalker(String(self).unicodeScalars)
    let nextIdx = walker.nextBoundary(startingAt: startIdx)
    return nextIdx._encodedOffset &- i
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _opaqueCharacterStride(endingAt i: Int) -> Int {
    if _slowPath(isForeign) {
      return _foreignOpaqueCharacterStride(endingAt: i)
    }

    return self.withFastUTF8 { utf8 in
      let (sc2, len) = _decodeScalar(utf8, endingAt: i)
      if i &- len == utf8.startIndex {
        // First scalar is first grapheme
        return len
      }
      let (sc1, _) = _decodeScalar(utf8, endingAt: i &- len)
      if _fastPath(_hasGraphemeBreakBetween(sc1, sc2)) {
        return len
      }

      let idx = String.Index(_encodedOffset: i)
      let walker = Unicode._GraphemeWalker(String(self).unicodeScalars)
      let previousIdx = walker.previousBoundary(endingAt: idx)
      return i &- previousIdx._encodedOffset
    }
  }

  @inline(never)
  @_effects(releasenone)
  private func _foreignOpaqueCharacterStride(endingAt i: Int) -> Int {
#if _runtime(_ObjC)
    _internalInvariant(isForeign)

    let endIdx = String.Index(_encodedOffset: i)
    let (sc2, len) = foreignErrorCorrectedScalar(endingAt: endIdx)
    if i &- len == 0 {
      // First scalar is first grapheme
      return len
    }
    let (sc1, _) = foreignErrorCorrectedScalar(
      endingAt: endIdx.encoded(offsetBy: -len))
    if _fastPath(_hasGraphemeBreakBetween(sc1, sc2)) {
      return len
    }

    let walker = Unicode._GraphemeWalker(String(self).unicodeScalars)
    let previousIdx = walker.previousBoundary(endingAt: endIdx)
    return i &- previousIdx._encodedOffset
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }
}

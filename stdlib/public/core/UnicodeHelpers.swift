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

//
// Low-level helper functions and utilities for interpreting Unicode
//
@inlinable
@inline(__always)
internal func _decodeUTF8(_ x: UInt8) -> Unicode.Scalar {
  _internalInvariant(UTF8.isASCII(x))
  return Unicode.Scalar(_unchecked: UInt32(x))
}

@inlinable
@inline(__always)
internal func _decodeUTF8(_ x: UInt8, _ y: UInt8) -> Unicode.Scalar {
  _internalInvariant(_utf8ScalarLength(x) == 2)
  _internalInvariant(UTF8.isContinuation(y))
  let x = UInt32(x)
  let value = ((x & 0b0001_1111) &<< 6) | _continuationPayload(y)
  return Unicode.Scalar(_unchecked: value)
}

@inlinable
@inline(__always)
internal func _decodeUTF8(
  _ x: UInt8, _ y: UInt8, _ z: UInt8
) -> Unicode.Scalar {
  _internalInvariant(_utf8ScalarLength(x) == 3)
  _internalInvariant(UTF8.isContinuation(y) && UTF8.isContinuation(z))
  let x = UInt32(x)
  let value = ((x & 0b0000_1111) &<< 12)
            | (_continuationPayload(y) &<< 6)
            | _continuationPayload(z)
  return Unicode.Scalar(_unchecked: value)
}

@inlinable
@inline(__always)
internal func _decodeUTF8(
  _ x: UInt8, _ y: UInt8, _ z: UInt8, _ w: UInt8
) -> Unicode.Scalar {
  _internalInvariant(_utf8ScalarLength(x) == 4)
  _internalInvariant(
    UTF8.isContinuation(y) && UTF8.isContinuation(z)
    && UTF8.isContinuation(w))
  let x = UInt32(x)
  let value = ((x & 0b0000_1111) &<< 18)
            | (_continuationPayload(y) &<< 12)
            | (_continuationPayload(z) &<< 6)
            | _continuationPayload(w)
  return Unicode.Scalar(_unchecked: value)
}

internal func _decodeScalar(
  _ utf16: UnsafeBufferPointer<UInt16>, startingAt i: Int
) -> (Unicode.Scalar, scalarLength: Int) {
  let high = utf16[i]
  if i + 1 >= utf16.count {
    _internalInvariant(!UTF16.isLeadSurrogate(high))
    _internalInvariant(!UTF16.isTrailSurrogate(high))
    return (Unicode.Scalar(_unchecked: UInt32(high)), 1)
  }

  if !UTF16.isLeadSurrogate(high) {
    _internalInvariant(!UTF16.isTrailSurrogate(high))
    return (Unicode.Scalar(_unchecked: UInt32(high)), 1)
  }

  let low = utf16[i+1]
  _internalInvariant(UTF16.isLeadSurrogate(high))
  _internalInvariant(UTF16.isTrailSurrogate(low))
  return (UTF16._decodeSurrogates(high, low), 2)
}

@inlinable
internal func _decodeScalar(
  _ utf8: UnsafeBufferPointer<UInt8>, startingAt i: Int
) -> (Unicode.Scalar, scalarLength: Int) {
  let cu0 = utf8[_unchecked: i]
  let len = _utf8ScalarLength(cu0)
  switch  len {
  case 1: return (_decodeUTF8(cu0), len)
  case 2: return (_decodeUTF8(cu0, utf8[_unchecked: i &+ 1]), len)
  case 3: return (_decodeUTF8(
    cu0, utf8[_unchecked: i &+ 1], utf8[_unchecked: i &+ 2]), len)
  case 4:
    return (_decodeUTF8(
      cu0,
      utf8[_unchecked: i &+ 1],
      utf8[_unchecked: i &+ 2],
      utf8[_unchecked: i &+ 3]),
    len)
  default: Builtin.unreachable()
  }
}

@inlinable
internal func _decodeScalar(
  _ utf8: UnsafeBufferPointer<UInt8>, endingAt i: Int
) -> (Unicode.Scalar, scalarLength: Int) {
  let len = _utf8ScalarLength(utf8, endingAt: i)
  let (scalar, scalarLen) = _decodeScalar(utf8, startingAt: i &- len)
  _internalInvariant(len == scalarLen)
  return (scalar, len)
}

@inlinable @inline(__always)
internal func _utf8ScalarLength(_ x: UInt8) -> Int {
  _internalInvariant(!UTF8.isContinuation(x))
  if UTF8.isASCII(x) { return 1 }
  // TODO(String micro-performance): check codegen
  return (~x).leadingZeroBitCount
}

@inlinable @inline(__always)
internal func _utf8ScalarLength(
  _ utf8: UnsafeBufferPointer<UInt8>, endingAt i: Int
  ) -> Int {
  var len = 1
  while UTF8.isContinuation(utf8[_unchecked: i &- len]) {
    len &+= 1
  }
  _internalInvariant(len == _utf8ScalarLength(utf8[i &- len]))
  return len
}

@inlinable
@inline(__always)
internal func _continuationPayload(_ x: UInt8) -> UInt32 {
  return UInt32(x & 0x3F)
}

@inlinable
internal func _scalarAlign(
  _ utf8: UnsafeBufferPointer<UInt8>, _ idx: Int
) -> Int {
  guard _fastPath(idx != utf8.count) else { return idx }

  var i = idx
  while _slowPath(UTF8.isContinuation(utf8[_unchecked: i])) {
    i &-= 1
    _internalInvariant(i >= 0,
      "Malformed contents: starts with continuation byte")
  }
  return i
}

//
// Scalar helpers
//
extension _StringGuts {
  @inlinable
  @inline(__always) // fast-path: fold common fastUTF8 check
  internal func scalarAlign(_ idx: Index) -> Index {
    let result: String.Index
    if _fastPath(idx._isScalarAligned) {
      result = idx
    } else {
      // TODO(String performance): isASCII check
      result = scalarAlignSlow(idx)
    }

    _internalInvariant(isOnUnicodeScalarBoundary(result),
      "Alignment bit is set for non-aligned index")
    _internalInvariant_5_1(result._isScalarAligned)
    return result
  }

  @inline(never) // slow-path
  @_alwaysEmitIntoClient // Swift 5.1
  internal func scalarAlignSlow(_ idx: Index) -> Index {
    _internalInvariant_5_1(!idx._isScalarAligned)

    if _slowPath(idx.transcodedOffset != 0 || idx._encodedOffset == 0) {
      // Transcoded index offsets are already scalar aligned
      return String.Index(_encodedOffset: idx._encodedOffset)._scalarAligned
    }
    if _slowPath(self.isForeign) {
      // In 5.1 this check was added to foreignScalarAlign, but when this is
      // emitted into a client that then runs against a 5.0 stdlib, it calls
      // a version of foreignScalarAlign that doesn't check for this, which
      // ends up asking CFString for its endIndex'th character, which throws
      // an exception. So we duplicate the check here for back deployment.
      guard idx._encodedOffset != self.count else { return idx._scalarAligned }

      let foreignIdx = foreignScalarAlign(idx)
      _internalInvariant_5_1(foreignIdx._isScalarAligned)
      return foreignIdx
    }

    return String.Index(_encodedOffset:
      self.withFastUTF8 { _scalarAlign($0, idx._encodedOffset) }
    )._scalarAligned
  }

  @inlinable
  internal func fastUTF8ScalarLength(startingAt i: Int) -> Int {
    _internalInvariant(isFastUTF8)
    let len = _utf8ScalarLength(self.withFastUTF8 { $0[i] })
    _internalInvariant((1...4) ~= len)
    return len
  }

  @inlinable
  internal func fastUTF8ScalarLength(endingAt i: Int) -> Int {
    _internalInvariant(isFastUTF8)

    return self.withFastUTF8 { utf8 in
      _internalInvariant(i == utf8.count || !UTF8.isContinuation(utf8[i]))
      var len = 1
      while UTF8.isContinuation(utf8[i &- len]) {
        _internalInvariant(i &- len > 0)
        len += 1
      }
      _internalInvariant(len <= 4)
      return len
    }
  }

  @inlinable
  internal func fastUTF8Scalar(startingAt i: Int) -> Unicode.Scalar {
    _internalInvariant(isFastUTF8)
    return self.withFastUTF8 { _decodeScalar($0, startingAt: i).0 }
  }

  @usableFromInline
  @_effects(releasenone)
  internal func isOnUnicodeScalarBoundary(_ i: String.Index) -> Bool {
    // TODO(String micro-performance): check isASCII

    // Beginning and end are always scalar aligned; mid-scalar never is
    guard i.transcodedOffset == 0 else { return false }
    if i == self.startIndex || i == self.endIndex { return true }

    if _fastPath(isFastUTF8) {
      return self.withFastUTF8 {
        return !UTF8.isContinuation($0[i._encodedOffset])
      }
    }

    return i == foreignScalarAlign(i)
  }
}

//
// Error-correcting helpers (U+FFFD for unpaired surrogates) for accessing
// contents of foreign strings
//
extension _StringGuts {
  @_effects(releasenone)
  private func _getForeignCodeUnit(at i: Int) -> UInt16 {
#if _runtime(_ObjC)
    // Currently, foreign  means NSString
    return _cocoaStringSubscript(_object.cocoaObject, i)
#else
  fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }

  @usableFromInline
  @_effects(releasenone)
  internal func foreignErrorCorrectedScalar(
    startingAt idx: String.Index
  ) -> (Unicode.Scalar, scalarLength: Int) {
    _internalInvariant(idx.transcodedOffset == 0)
    _internalInvariant(idx._encodedOffset < self.count)

    let start = idx._encodedOffset
    let leading = _getForeignCodeUnit(at: start)

    if _fastPath(!UTF16.isSurrogate(leading)) {
      return (Unicode.Scalar(_unchecked: UInt32(leading)), 1)
    }

    // Validate foreign strings on-read: trailing surrogates are invalid,
    // leading need following trailing
    //
    // TODO(String performance): Consider having a valid performance flag
    // available to check, and assert it's not set in the condition here.
    let nextOffset = start &+ 1
    if _slowPath(UTF16.isTrailSurrogate(leading) || nextOffset == self.count) {
      return (Unicode.Scalar._replacementCharacter, 1)
    }
    let trailing = _getForeignCodeUnit(at: nextOffset)
    if _slowPath(!UTF16.isTrailSurrogate(trailing)) {
      return (Unicode.Scalar._replacementCharacter, 1)
    }

    return (UTF16._decodeSurrogates(leading, trailing), 2)
  }

  @_effects(releasenone)
  internal func foreignErrorCorrectedScalar(
    endingAt idx: String.Index
  ) -> (Unicode.Scalar, scalarLength: Int) {
    _internalInvariant(idx.transcodedOffset == 0)
    _internalInvariant(idx._encodedOffset <= self.count)
    _internalInvariant(idx._encodedOffset > 0)

    let end = idx._encodedOffset
    let trailing = _getForeignCodeUnit(at: end &- 1)
    if _fastPath(!UTF16.isSurrogate(trailing)) {
      return (Unicode.Scalar(_unchecked: UInt32(trailing)), 1)
    }

    // Validate foreign strings on-read: trailing surrogates are invalid,
    // leading need following trailing
    //
    // TODO(String performance): Consider having a valid performance flag
    // available to check, and assert it's not set in the condition here.
    let priorOffset = end &- 2
    if _slowPath(UTF16.isLeadSurrogate(trailing) || priorOffset < 0) {
      return (Unicode.Scalar._replacementCharacter, 1)
    }
    let leading = _getForeignCodeUnit(at: priorOffset)
    if _slowPath(!UTF16.isLeadSurrogate(leading)) {
      return (Unicode.Scalar._replacementCharacter, 1)
    }

    return (UTF16._decodeSurrogates(leading, trailing), 2)
  }

  @_effects(releasenone)
  internal func foreignErrorCorrectedUTF16CodeUnit(
    at idx: String.Index
  ) -> UInt16 {
    _internalInvariant(idx.transcodedOffset == 0)
    _internalInvariant(idx._encodedOffset < self.count)

    let start = idx._encodedOffset
    let cu = _getForeignCodeUnit(at: start)
    if _fastPath(!UTF16.isSurrogate(cu)) {
      return cu
    }

    // Validate foreign strings on-read: trailing surrogates are invalid,
    // leading need following trailing
    //
    // TODO(String performance): Consider having a valid performance flag
    // available to check, and assert it's not set in the condition here.
    if UTF16.isLeadSurrogate(cu) {
      let nextOffset = start &+ 1
      guard nextOffset < self.count,
            UTF16.isTrailSurrogate(_getForeignCodeUnit(at: nextOffset))
      else { return UTF16._replacementCodeUnit }
    } else {
      let priorOffset = start &- 1
      guard priorOffset >= 0,
            UTF16.isLeadSurrogate(_getForeignCodeUnit(at: priorOffset))
      else { return UTF16._replacementCodeUnit }
    }

    return cu
  }

  @usableFromInline @inline(never) // slow-path
  @_effects(releasenone)
  internal func foreignScalarAlign(_ idx: Index) -> Index {
    guard idx._encodedOffset != self.count else { return idx._scalarAligned }

    _internalInvariant(idx._encodedOffset < self.count)

    let ecCU = foreignErrorCorrectedUTF16CodeUnit(at: idx)
    if _fastPath(!UTF16.isTrailSurrogate(ecCU)) {
      return idx._scalarAligned
    }
    _internalInvariant(idx._encodedOffset > 0,
      "Error-correction shouldn't give trailing surrogate at position zero")
    return String.Index(_encodedOffset: idx._encodedOffset &- 1)._scalarAligned
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func foreignErrorCorrectedGrapheme(
    startingAt start: Int, endingAt end: Int
  ) -> Character {
#if _runtime(_ObjC)
    _internalInvariant(self.isForeign)

    // Both a fast-path for single-code-unit graphemes and validation:
    //   ICU treats isolated surrogates as isolated graphemes
    let count = end &- start
    if start &- end == 1 {
      return Character(String(self.foreignErrorCorrectedScalar(
        startingAt: String.Index(_encodedOffset: start)
      ).0))
    }

    // TODO(String performance): Stack buffer if small enough
    let cus = Array<UInt16>(unsafeUninitializedCapacity: count) {
      buffer, initializedCapacity in
      _cocoaStringCopyCharacters(
        from: self._object.cocoaObject,
        range: start..<end,
        into: buffer.baseAddress._unsafelyUnwrappedUnchecked)
      initializedCapacity = count
    }
    return cus.withUnsafeBufferPointer {
      return Character(String._uncheckedFromUTF16($0))
    }
#else
    fatalError("No foreign strings on Linux in this version of Swift")
#endif
  }
}

// Higher level aggregate operations. These should only be called when the
// result is the sole operation done by a caller, otherwise it's always more
// efficient to use `withFastUTF8` in the caller.
extension _StringGuts {
  @inlinable @inline(__always)
  internal func errorCorrectedScalar(
    startingAt i: Int
  ) -> (Unicode.Scalar, scalarLength: Int) {
    if _fastPath(isFastUTF8) {
      return withFastUTF8 { _decodeScalar($0, startingAt: i) }
    }
    return foreignErrorCorrectedScalar(
      startingAt: String.Index(_encodedOffset: i))
  }
  @inlinable @inline(__always)
  internal func errorCorrectedCharacter(
    startingAt start: Int, endingAt end: Int
  ) -> Character {
    if _fastPath(isFastUTF8) {
      return withFastUTF8(range: start..<end) { utf8 in
        return Character(unchecked: String._uncheckedFromUTF8(utf8))
      }
    }

    return foreignErrorCorrectedGrapheme(startingAt: start, endingAt: end)
  }
}

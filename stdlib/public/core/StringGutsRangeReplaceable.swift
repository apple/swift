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

// COW helpers
extension _StringGuts {
  internal var nativeCapacity: Int? {
    @inline(never)
    @_effects(releasenone)
    get {
      guard hasNativeStorage else { return nil }
      return _object.withNativeStorage { $0.capacity }
    }
  }

  internal var nativeUnusedCapacity: Int? {
    @inline(never)
    @_effects(releasenone)
    get {
      guard hasNativeStorage else { return nil }
      return _object.withNativeStorage { $0.unusedCapacity }
    }
  }

  // If natively stored and uniquely referenced, return the storage's total
  // capacity. Otherwise, nil.
  internal var uniqueNativeCapacity: Int? {
    @inline(never)
    @_effects(releasenone)
    mutating get {
      guard isUniqueNative else { return nil }
      return _object.withNativeStorage { $0.capacity }
    }
  }

  // If natively stored and uniquely referenced, return the storage's spare
  // capacity. Otherwise, nil.
  internal var uniqueNativeUnusedCapacity: Int? {
    @inline(never)
    @_effects(releasenone)
    mutating get {
      guard isUniqueNative else { return nil }
      return _object.withNativeStorage { $0.unusedCapacity }
    }
  }

  @usableFromInline // @testable
  internal var isUniqueNative: Bool {
    @inline(__always) mutating get {
      // Note: mutating so that self is `inout`.
      guard hasNativeStorage else { return false }
      defer { _fixLifetime(self) }
      var bits: UInt = _object.largeAddressBits
      return _isUnique_native(&bits)
    }
  }
}

// Range-replaceable operation support
extension _StringGuts {
  @inline(__always)
  internal mutating func updateNativeStorage<R>(
    _ body: (__StringStorage) -> R
  ) -> R {
    let (r, cf) = self._object.withNativeStorage {
      let r = body($0)
      let cf = $0._countAndFlags
      return (r, cf)
    }
    // We need to pick up new count/flags from the modified storage.
    self._object._setCountAndFlags(to: cf)
    return r
  }

  @inlinable
  internal init(_initialCapacity capacity: Int) {
    self.init()
    if _slowPath(capacity > _SmallString.capacity) {
      self.grow(capacity) // TODO: no factor should be applied
    }
  }

  internal mutating func reserveCapacity(_ n: Int) {
    // Check if there's nothing to do
    if n <= _SmallString.capacity { return }
    if let currentCap = self.uniqueNativeCapacity, currentCap >= n { return }

    // Grow
    self.grow(n) // TODO: no factor should be applied
  }

  // Grow to accommodate at least `n` code units
  @usableFromInline
  internal mutating func grow(_ n: Int) {
    defer {
      self._invariantCheck()
      _internalInvariant(
        self.uniqueNativeCapacity != nil && self.uniqueNativeCapacity! >= n)
    }

    _internalInvariant(
      self.uniqueNativeCapacity == nil || self.uniqueNativeCapacity! < n)

    // If unique and native, apply a 2x growth factor to avoid problematic
    // performance when used in a loop. If one if those doesn't apply, we
    // can just use the requested capacity (at least the current utf-8 count).
    // TODO: Don't do this! Growth should only happen for append...
    let growthTarget: Int
    if let capacity = self.uniqueNativeCapacity {
      growthTarget = Swift.max(n, capacity * 2)
    } else {
      growthTarget = Swift.max(n, self.utf8Count)
    }

    // `isFastUTF8` is not the same as `isNative`. It can include small
    // strings or foreign strings that provide contiguous UTF-8 access.
    if _fastPath(isFastUTF8) {
      let isASCII = self.isASCII
      let storage = self.withFastUTF8 {
        __StringStorage.create(
          initializingFrom: $0,
          codeUnitCapacity: growthTarget,
          isASCII: isASCII)
      }

      self = _StringGuts(storage)
      return
    }

    _foreignGrow(growthTarget)
  }

  @inline(never) // slow-path
  private mutating func _foreignGrow(_ n: Int) {
    let newString = String(_uninitializedCapacity: n) { buffer in
      guard let count = _foreignCopyUTF8(into: buffer) else {
       fatalError("String capacity was smaller than required")
      }
      return count
    }
    self = newString._guts
  }

  // Ensure unique native storage with sufficient capacity for the following
  // append.
  private mutating func prepareForAppendInPlace(
    totalCount: Int,
    otherUTF8Count otherCount: Int
  ) {
    defer {
      _internalInvariant(self.uniqueNativeUnusedCapacity != nil,
        "growth should produce uniqueness")
      _internalInvariant(self.uniqueNativeUnusedCapacity! >= otherCount,
        "growth should produce enough capacity")
    }

    // See if we can accommodate without growing or copying. If we have
    // sufficient capacity, we do not need to grow, and we can skip the copy if
    // unique. Otherwise, growth is required.
    let sufficientCapacity: Bool
    if let unused = self.nativeUnusedCapacity, unused >= otherCount {
      sufficientCapacity = true
    } else {
      sufficientCapacity = false
    }

    if self.isUniqueNative && sufficientCapacity {
      return
    }

    // If we have to resize anyway, and we fit in smol, we should have made one
    _internalInvariant(totalCount > _SmallString.capacity)

    // Non-unique storage: just make a copy of the appropriate size, otherwise
    // grow like an array.
    let growthTarget: Int
    if sufficientCapacity {
      growthTarget = totalCount
    } else {
      growthTarget = Swift.max(
        totalCount, _growArrayCapacity(nativeCapacity ?? 0))
    }
    self.grow(growthTarget) // NOTE: this already has exponential growth...
  }

  internal mutating func append(_ other: _StringGuts) {
    if self.isSmall && other.isSmall {
      if let smol = _SmallString(self.asSmall, appending: other.asSmall) {
        self = _StringGuts(smol)
        return
      }
    }
    append(_StringGutsSlice(other))
  }

  @inline(never)
  @_effects(readonly)
  private func _foreignConvertedToSmall() -> _SmallString {
    let smol = String(_uninitializedCapacity: _SmallString.capacity) { buffer in
      guard let count = _foreignCopyUTF8(into: buffer) else {
        fatalError("String capacity was smaller than required")
      }
      return count
    }
    _internalInvariant(smol._guts.isSmall)
    return smol._guts.asSmall
  }

  private func _convertedToSmall() -> _SmallString {
    _internalInvariant(utf8Count <= _SmallString.capacity)
    if _fastPath(isSmall) {
      return asSmall
    }
    if isFastUTF8 {
      return withFastUTF8 { _SmallString($0)! }
    }
    return _foreignConvertedToSmall()
  }

  internal mutating func append(_ slicedOther: _StringGutsSlice) {
    defer { self._invariantCheck() }

    let otherCount = slicedOther.utf8Count

    let totalCount = utf8Count + otherCount

    /*
     Goal: determine if we need to allocate new native capacity
     Possible scenarios in which we need to allocate:
     • Not uniquely owned and native: we can't use the capacity to grow into,
        have to become unique + native by allocating
     • Not enough capacity: have to allocate to grow

     Special case: a non-smol String that can fit in a smol String but doesn't
        meet the above criteria shouldn't throw away its buffer just to be smol.
        The reasoning here is that it may be bridged or have reserveCapacity'd
        in preparation for appending more later, in which case we would end up
        have to allocate anyway to convert back from smol.

        If we would have to re-allocate anyway then that's not a problem and we
        should just be smol.

        e.g. consider
        var str = "" // smol
        str.reserveCapacity(100) // large native unique
        str += "<html>" // don't convert back to smol here!
        str += htmlContents // because this would have to anyway!
     */
    let hasEnoughUsableSpace = isUniqueNative &&
      nativeUnusedCapacity! >= otherCount
    let shouldBeSmol = totalCount <= _SmallString.capacity &&
      (isSmall || !hasEnoughUsableSpace)

    if shouldBeSmol {
      let smolSelf = _convertedToSmall()
      let smolOther = String(Substring(slicedOther))._guts._convertedToSmall()
      // TODO: In-register slicing
      self = _StringGuts(_SmallString(smolSelf, appending: smolOther)!)
      return
    }

    prepareForAppendInPlace(totalCount: totalCount, otherUTF8Count: otherCount)

    if slicedOther.isFastUTF8 {
      let otherIsASCII = slicedOther.isASCII
      slicedOther.withFastUTF8 { otherUTF8 in
        self.appendInPlace(otherUTF8, isASCII: otherIsASCII)
      }
      return
    }

    _foreignAppendInPlace(slicedOther)
  }

  internal mutating func appendInPlace(
    _ other: UnsafeBufferPointer<UInt8>, isASCII: Bool
  ) {
    updateNativeStorage { $0.appendInPlace(other, isASCII: isASCII) }
  }

  @inline(never) // slow-path
  private mutating func _foreignAppendInPlace(_ other: _StringGutsSlice) {
    _internalInvariant(!other.isFastUTF8)
    _internalInvariant(self.uniqueNativeUnusedCapacity != nil)

    var iter = Substring(other).utf8.makeIterator()
    updateNativeStorage { $0.appendInPlace(&iter, isASCII: other.isASCII) }
  }

  internal mutating func clear() {
    guard isUniqueNative else {
      self = _StringGuts()
      return
    }

    // Reset the count
    updateNativeStorage { $0.clear() }
  }

  internal mutating func remove(from lower: Index, to upper: Index) {
    let lowerOffset = lower._encodedOffset
    let upperOffset = upper._encodedOffset
    _internalInvariant(lower.transcodedOffset == 0 && upper.transcodedOffset == 0)
    _internalInvariant(lowerOffset <= upperOffset && upperOffset <= self.count)

    if isUniqueNative {
      updateNativeStorage { $0.remove(from: lowerOffset, to: upperOffset) }
      return
    }

    // TODO(cleanup): Add append on guts taking range, use that
    var result = String()
    // FIXME: It should be okay to get rid of excess capacity
    // here. rdar://problem/45635432
    result.reserveCapacity(
      nativeCapacity ?? (count &- (upperOffset &- lowerOffset)))
    result.append(contentsOf: String(self)[..<lower])
    result.append(contentsOf: String(self)[upper...])
    self = result._guts
  }

  // - Returns: The encoded offset range of the replaced contents in the result.
  @discardableResult
  internal mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) -> Range<Int>
  where C: Collection, C.Iterator.Element == Character {
    if isUniqueNative {
      if let repl = newElements as? String {
        if repl._guts.isFastUTF8 {
          return repl._guts.withFastUTF8 {
            uniqueNativeReplaceSubrange(
              bounds, with: $0, isASCII: repl._guts.isASCII)
          }
        }
      } else if let repl = newElements as? Substring {
        if repl._wholeGuts.isFastUTF8 {
          return repl._wholeGuts.withFastUTF8(range: repl._offsetRange) {
            uniqueNativeReplaceSubrange(
              bounds, with: $0, isASCII: repl._wholeGuts.isASCII)
          }
        }
      }
      return uniqueNativeReplaceSubrange(
        bounds, with: newElements.lazy.flatMap { $0.utf8 })
    }

    var result = String()
    // FIXME: It should be okay to get rid of excess capacity
    // here. rdar://problem/45635432
    if let capacity = self.nativeCapacity {
      result.reserveCapacity(capacity)
    }
    let selfStr = String(self)
    result.append(contentsOf: selfStr[..<bounds.lowerBound])
    let i = result._guts.count
    result.append(contentsOf: newElements)
    let j = result._guts.count
    result.append(contentsOf: selfStr[bounds.upperBound...])
    self = result._guts
    return Range(_uncheckedBounds: (i, j))
  }

  // - Returns: The encoded offset range of the replaced contents in the result.
  @discardableResult
  internal mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) -> Range<Int>
  where C: Collection, C.Iterator.Element == UnicodeScalar {
    if isUniqueNative {
      if let repl = newElements as? String.UnicodeScalarView {
        if repl._guts.isFastUTF8 {
          return repl._guts.withFastUTF8 {
            uniqueNativeReplaceSubrange(
              bounds, with: $0, isASCII: repl._guts.isASCII)
          }
        }
      } else if let repl = newElements as? Substring.UnicodeScalarView {
        if repl._wholeGuts.isFastUTF8 {
          return repl._wholeGuts.withFastUTF8(range: repl._offsetRange) {
            uniqueNativeReplaceSubrange(
              bounds, with: $0, isASCII: repl._wholeGuts.isASCII)
          }
        }
      }
      if #available(SwiftStdlib 5.1, *) {
        return uniqueNativeReplaceSubrange(
          bounds, with: newElements.lazy.flatMap { $0.utf8 })
      } else {
        // FIXME: The stdlib should not have a deployment target this ancient.
        let c = newElements.reduce(0) { $0 + UTF8.width($1) }
        var utf8: [UInt8] = []
        utf8.reserveCapacity(c)
        utf8 = newElements.reduce(into: utf8) { utf8, next in
          next.withUTF8CodeUnits { utf8.append(contentsOf: $0) }
        }
        return uniqueNativeReplaceSubrange(bounds, with: utf8)
      }
    }

    var result = String.UnicodeScalarView()
    // FIXME: It should be okay to get rid of excess capacity
    // here. rdar://problem/45635432
    if let capacity = self.nativeCapacity {
      result.reserveCapacity(capacity)
    }
    let selfStr = String.UnicodeScalarView(self)
    result.append(contentsOf: selfStr[..<bounds.lowerBound])
    let i = result._guts.count
    result.append(contentsOf: newElements)
    let j = result._guts.count
    result.append(contentsOf: selfStr[bounds.upperBound...])
    self = result._guts
    return Range(_uncheckedBounds: (i, j))
  }

  // - Returns: The encoded offset range of the replaced contents in the result.
  internal mutating func uniqueNativeReplaceSubrange(
    _ bounds: Range<Index>,
    with codeUnits: UnsafeBufferPointer<UInt8>,
    isASCII: Bool
  ) -> Range<Int> {
    let neededCapacity =
      bounds.lowerBound._encodedOffset
      + codeUnits.count + (self.count - bounds.upperBound._encodedOffset)
    reserveCapacity(neededCapacity)

    _internalInvariant(bounds.lowerBound.transcodedOffset == 0)
    _internalInvariant(bounds.upperBound.transcodedOffset == 0)

    let start = bounds.lowerBound._encodedOffset
    let end = bounds.upperBound._encodedOffset
    updateNativeStorage {
      $0.replace(from: start, to: end, with: codeUnits)
    }
    return Range(_uncheckedBounds: (start, start + codeUnits.count))
  }

  // - Returns: The encoded offset range of the replaced contents in the result.
  internal mutating func uniqueNativeReplaceSubrange<C: Collection>(
    _ bounds: Range<Index>,
    with codeUnits: C
  ) -> Range<Int>
  where C.Element == UInt8 {
    let replCount = codeUnits.count

    let neededCapacity =
      bounds.lowerBound._encodedOffset
      + replCount + (self.count - bounds.upperBound._encodedOffset)
    reserveCapacity(neededCapacity)

    _internalInvariant(bounds.lowerBound.transcodedOffset == 0)
    _internalInvariant(bounds.upperBound.transcodedOffset == 0)

    let start = bounds.lowerBound._encodedOffset
    let end = bounds.upperBound._encodedOffset
    updateNativeStorage {
      $0.replace(
        from: start, to: end, with: codeUnits, replacementCount: replCount)
    }
    return Range(_uncheckedBounds: (start, start + replCount))
  }

  /// Run `body` to mutate the given `subrange` of this string within
  /// `startIndex ..< endIndex`, then update `startIndex` and `endIndex` to be
  /// valid positions in the resulting string, addressing the same (logical)
  /// locations as in the original string.
  ///
  /// This is used by both `Substring` and `Substring.UnicodeScalarView` to
  /// implement their `replaceSubrange` methods.
  ///
  /// - Parameter subrange: A scalar-aligned offset range in this string.
  /// - Parameter startIndex: The start index of the substring that performs
  ///   this operation.
  /// - Parameter endIndex: The end index of the substring that performs this
  ///   operations.
  /// - Parameter body: The mutation operation to execute on `self`. The
  ///   returned offset range must correspond to `subrange` in the resulting
  ///   string.
  internal mutating func mutateSubrangeInSubstring(
    subrange: Range<Index>,
    startIndex: inout Index,
    endIndex: inout Index,
    with body: (inout _StringGuts) -> Range<Int>
  ) {
    _internalInvariant(
      subrange.lowerBound >= startIndex && subrange.upperBound <= endIndex)

    guard _fastPath(isUTF8) else {
      // UTF-16 string. The mutation will convert this to the native UTF-8
      // encoding, so we need to do some extra work to preserve our bounds.
      let utf8StartOffset = String(self).utf8.distance(
        from: self.startIndex, to: startIndex)
      let oldUTF8Count = String(self).utf8.distance(
        from: startIndex, to: endIndex)

      let oldUTF8SubrangeCount = String(self).utf8.distance(
        from: subrange.lowerBound, to: subrange.upperBound)

      let newUTF8Subrange = body(&self)
      _internalInvariant(isUTF8)

      let newUTF8Count =
        oldUTF8Count + newUTF8Subrange.count - oldUTF8SubrangeCount

      var newStride = 0

      if !newUTF8Subrange.isEmpty {
        // Get the character stride in the entire string, not just the substring.
        // (Characters in a substring may end beyond the bounds of it.)
        newStride = _opaqueCharacterStride(startingAt: utf8StartOffset)
      }

      startIndex = String.Index(
        encodedOffset: utf8StartOffset,
        transcodedOffset: 0,
        characterStride: newStride)._scalarAligned._knownUTF8
      if isOnGraphemeClusterBoundary(startIndex) {
        startIndex = startIndex._characterAligned
      }

      endIndex = String.Index(
        encodedOffset: utf8StartOffset + newUTF8Count,
        transcodedOffset: 0)._scalarAligned._knownUTF8
      return
    }

    // UTF-8 string.

    let oldRange = subrange._encodedOffsetRange
    let newRange = body(&self)

    let oldBounds = Range(
      _uncheckedBounds: (startIndex._encodedOffset, endIndex._encodedOffset))
    let newBounds = Range(_uncheckedBounds: (
        oldBounds.lowerBound,
        oldBounds.upperBound &+ newRange.count &- oldRange.count))

    // Update `startIndex` if necessary. The replacement may have invalidated
    // its cached character stride and character alignment flag, but not its
    // stored offset, encoding, or scalar alignment.
    //
    // We are exploiting the fact that mutating the string _after_ the scalar
    // following the end of the character at `startIndex` cannot possibly change
    // the length of that character. (This is true because `index(after:)` never
    // needs to look ahead by more than one Unicode scalar.)
    let oldStride = startIndex.characterStride ?? 0
    if oldRange.lowerBound <= oldBounds.lowerBound &+ oldStride {
      var newStride = 0

      if !newBounds.isEmpty {
        // Get the character stride in the entire string, not just the substring.
        // (Characters in a substring may end beyond the bounds of it.)
        newStride = _opaqueCharacterStride(startingAt: newBounds.lowerBound)
      }

      var newStart = String.Index(
        encodedOffset: newBounds.lowerBound,
        characterStride: newStride
      )._scalarAligned._knownUTF8

      // Preserve character alignment flag if possible
      if startIndex._isCharacterAligned,
        (oldRange.lowerBound > oldBounds.lowerBound ||
         isOnGraphemeClusterBoundary(newStart)) {
        newStart = newStart._characterAligned
      }

      startIndex = newStart
    }

    // Update `endIndex`.
    if newBounds.upperBound != endIndex._encodedOffset {
      endIndex = Index(
        _encodedOffset: newBounds.upperBound
      )._scalarAligned._knownUTF8
    }
  }
}


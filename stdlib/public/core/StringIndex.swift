//===--- StringIndex.swift ------------------------------------------------===//
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

import SwiftShims

/*

String's Index has the following layout:

 ┌──────────┬────────────────╥────────────────┬───────╥───────┐
 │ b63:b16  │      b15:b14   ║     b13:b8     │ b7:b4 ║ b3:b0 │
 ├──────────┼────────────────╫────────────────┼───────╫───────┤
 │ position │ transc. offset ║ grapheme cache │ rsvd  ║ flags │
 └──────────┴────────────────╨────────────────┴───────╨───────┘
                             └────── resilient ───────┘

Position, transcoded offset, and flags are fully exposed in the ABI. Grapheme
cache and reserved bits are partially resilient: the fact that there are 11 bits
with a default value of `0` is ABI, but not the layout, construction, or
interpretation of those bits. All use of grapheme cache should be behind
non-inlinable function calls. Inlinable code should not set a non-zero value to
resilient bits: doing so breaks future evolution as the meaning of those bits
isn't frozen.

- position aka `encodedOffset`: A 48-bit offset into the string's code units

- transcoded offset: a 2-bit sub-scalar offset, derived from transcoding

<resilience barrier>

- grapheme cache: A 6-bit value remembering the distance to the next extended
  grapheme cluster boundary, or 0 if unknown. The value stored (if any) must be
  calculated assuming that the index addresses a boundary itself, i.e., without
  looking back at scalars preceding the index. (Substrings that don't start on a
  `Character` boundary heavily rely on this.)

- reserved: 4 unused bits available for future flags etc. The meaning of each
  bit may change between stdlib versions. These must be set to zero if
  constructing an index in inlinable code.

<resilience barrier>

  * b0: `_isScalarAligned`

    If set, index is known to be on a Unicode scalar boundary (see below).
    (Introduced in Swift 5.1)

  * b1: `_isCharacterAligned`

    If set, the index is known to be on an extended grapheme cluster
    boundary (i.e., on a Swift `Character`.)
    (Introduced in Swift 5.7)

  * b2: UTF-8 encoding

    If set, the position is known to be expressed in UTF-8 code units.
    (Introduced in Swift 5.7)

  * b3: UTF-16 encoding

    If set, the position is known to be expressed in UTF-16 code units.
    (Introduced in Swift 5.7)

Before Swift 5.7, bits b1, b2 and b3 used to be part of the resilient slice. See
the notes on Character Alignment and Index Encoding below to see how this works.

*/
extension String {
  /// A position of a character or code unit in a string.
  @frozen
  public struct Index: Sendable {
    @usableFromInline
    internal var _rawBits: UInt64

    @inlinable @inline(__always)
    init(_ raw: UInt64) {
      self._rawBits = raw
      self._invariantCheck()
    }
  }
}

extension String.Index {
  @inlinable @inline(__always)
  internal var orderingValue: UInt64 { return _rawBits &>> 14 }

  // Whether this is at the canonical "start" position, that is encoded AND
  // transcoded offset of 0.
  @inlinable @inline(__always)
  internal var isZeroPosition: Bool { return orderingValue == 0 }

  /// The UTF-16 code unit offset corresponding to this index.
  public func utf16Offset<S: StringProtocol>(in s: S) -> Int {
    return s.utf16.distance(from: s.utf16.startIndex, to: self)
  }

  /// The offset into a string's code units for this index.
  @available(swift, deprecated: 4.2, message: """
    encodedOffset has been deprecated as most common usage is incorrect. \
    Use utf16Offset(in:) to achieve the same behavior.
    """)
  @inlinable
  public var encodedOffset: Int { return _encodedOffset }

  @inlinable @inline(__always)
  internal var _encodedOffset: Int {
    return Int(truncatingIfNeeded: _rawBits &>> 16)
  }

  @inlinable @inline(__always)
  internal var transcodedOffset: Int {
    return Int(truncatingIfNeeded: orderingValue & 0x3)
  }

  @usableFromInline
  internal var characterStride: Int? {
    let value = (_rawBits & 0x3F00) &>> 8
    return value > 0 ? Int(truncatingIfNeeded: value) : nil
  }

  @inlinable @inline(__always)
  internal init(encodedOffset: Int, transcodedOffset: Int) {
    let pos = UInt64(truncatingIfNeeded: encodedOffset)
    let trans = UInt64(truncatingIfNeeded: transcodedOffset)
    _internalInvariant(pos == pos & 0x0000_FFFF_FFFF_FFFF)
    _internalInvariant(trans <= 3)

    self.init((pos &<< 16) | (trans &<< 14))
  }

  /// Creates a new index at the specified UTF-16 code unit offset
  ///
  /// - Parameter offset: An offset in UTF-16 code units.
  public init<S: StringProtocol>(utf16Offset offset: Int, in s: S) {
    let (start, end) = (s.utf16.startIndex, s.utf16.endIndex)
    guard offset >= 0,
          let idx = s.utf16.index(start, offsetBy: offset, limitedBy: end)
    else {
      self = end.nextEncoded
      return
    }
    self = idx
  }

  /// Creates a new index at the specified code unit offset.
  ///
  /// - Parameter offset: An offset in code units.
  @available(swift, deprecated: 4.2, message: """
    encodedOffset has been deprecated as most common usage is incorrect. \
    Use String.Index(utf16Offset:in:) to achieve the same behavior.
    """)
  @inlinable
  public init(encodedOffset offset: Int) {
    self.init(_encodedOffset: offset)
  }

  @inlinable @inline(__always)
  internal init(_encodedOffset offset: Int) {
    self.init(encodedOffset: offset, transcodedOffset: 0)
  }

  @usableFromInline
  internal init(
    encodedOffset: Int, transcodedOffset: Int, characterStride: Int
  ) {
    self.init(encodedOffset: encodedOffset, transcodedOffset: transcodedOffset)
    if _slowPath(characterStride > 0x3F) { return }
    self._rawBits |= UInt64(truncatingIfNeeded: characterStride &<< 8)
    self._invariantCheck()
  }

  @usableFromInline
  internal init(encodedOffset pos: Int, characterStride char: Int) {
    self.init(encodedOffset: pos, transcodedOffset: 0, characterStride: char)
  }

  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    _internalInvariant(_encodedOffset >= 0)
    if self._isCharacterAligned {
      _internalInvariant(_isScalarAligned)
    }
    if self._isScalarAligned {
      _internalInvariant_5_1(transcodedOffset == 0)
    }
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

// Creation helpers, which will make migration easier if we decide to use and
// propagate the reserved bits.
extension String.Index {
  @inlinable @inline(__always)
  internal var strippingTranscoding: String.Index {
    return String.Index(_encodedOffset: self._encodedOffset)
  }

  @inlinable @inline(__always)
  internal var nextEncoded: String.Index {
    _internalInvariant(self.transcodedOffset == 0)
    return String.Index(_encodedOffset: self._encodedOffset &+ 1)
  }

  @inlinable @inline(__always)
  internal var priorEncoded: String.Index {
    _internalInvariant(self.transcodedOffset == 0)
    return String.Index(_encodedOffset: self._encodedOffset &- 1)
  }

  @inlinable @inline(__always)
  internal var nextTranscoded: String.Index {
    return String.Index(
      encodedOffset: self._encodedOffset,
      transcodedOffset: self.transcodedOffset &+ 1)
  }

  @inlinable @inline(__always)
  internal var priorTranscoded: String.Index {
    return String.Index(
      encodedOffset: self._encodedOffset,
      transcodedOffset: self.transcodedOffset &- 1)
  }


  // Get an index with an encoded offset relative to this one.
  // Note: strips any transcoded offset.
  @inlinable @inline(__always)
  internal func encoded(offsetBy n: Int) -> String.Index {
    return String.Index(_encodedOffset: self._encodedOffset &+ n)
  }

  @inlinable @inline(__always)
  internal func transcoded(withOffset n: Int) -> String.Index {
    _internalInvariant(self.transcodedOffset == 0)
    return String.Index(encodedOffset: self._encodedOffset, transcodedOffset: n)
  }
}

extension String.Index {
  @_alwaysEmitIntoClient @inline(__always) // Swift 5.7
  internal static var __scalarAlignmentBit: UInt64 { 0x1 }

  @_alwaysEmitIntoClient @inline(__always) // Swift 5.7
  internal static var __characterAlignmentBit: UInt64 { 0x2 }

  @_alwaysEmitIntoClient @inline(__always) // Swift 5.7
  internal static var __utf8Bit: UInt64 { 0x4 }

  @_alwaysEmitIntoClient @inline(__always) // Swift 5.7
  internal static var __utf16Bit: UInt64 { 0x8 }

  @_alwaysEmitIntoClient @inline(__always) // Swift 5.7
  internal static func __encodingBit(utf16: Bool) -> UInt64 {
    let utf16 = Int8(Builtin.zext_Int1_Int8(utf16._value))
    return __utf8Bit &<< utf16
  }
}

/*
  Index Scalar Alignment

  SE-0180 unifies the Index type of String and all its views and allows
  non-scalar-aligned indices to be used across views. In order to guarantee
  behavior, we often have to check and perform scalar alignment. To speed up
  these checks, we allocate a bit denoting known-to-be-scalar-aligned, so that
  the alignment check can skip the load. The below shows what views need to
  check for alignment before they can operate, and whether the indices they
  produce are aligned.

  ┌───────────────╥───────────────────────────┬─────────────────────────┐
  │ View          ║ Requires Scalar Alignment │ Produces Scalar Aligned │
  ╞═══════════════╬═══════════════════════════╪═════════════════════════╡
  │ Native UTF8   ║ no                        │ no                      │
  ├───────────────╫───────────────────────────┼─────────────────────────┤
  │ Native UTF16  ║ yes                       │ no                      │
  ╞═══════════════╬═══════════════════════════╪═════════════════════════╡
  │ Foreign UTF8  ║ yes                       │ no                      │
  ├───────────────╫───────────────────────────┼─────────────────────────┤
  │ Foreign UTF16 ║ no                        │ no                      │
  ╞═══════════════╬═══════════════════════════╪═════════════════════════╡
  │ UnicodeScalar ║ yes                       │ yes                     │
  ├───────────────╫───────────────────────────┼─────────────────────────┤
  │ Character     ║ yes                       │ yes                     │
  └───────────────╨───────────────────────────┴─────────────────────────┘

  The "requires scalar alignment" applies to any operation taking a String.Index
  that's not defined entirely in terms of other operations taking a
  String.Index. These include:

  * index(after:)
  * index(before:)
  * subscript
  * distance(from:to:) (since `to` is compared against directly)
  * UTF16View._nativeGetOffset(for:)

*/
extension String.Index {
  @_alwaysEmitIntoClient // Swift 5.1
  @inline(__always)
  internal var _isScalarAligned: Bool {
    0 != _rawBits & Self.__scalarAlignmentBit
  }

  @_alwaysEmitIntoClient // Swift 5.1
  @inline(__always)
  internal var _scalarAligned: String.Index {
    var idx = self
    idx._rawBits |= Self.__scalarAlignmentBit
    idx._invariantCheck()
    return idx
  }
}

// ### Character (a.k.a. Extended Grapheme Cluster) Alignment
//
// Swift 5.7 assigned a new bit denoting that an index is known to be
// `Character`-aligned. This is used to enable more reliable detection &
// handling of extended grapheme cluster boundaries in indexing edge cases
// introduced by SE-0180, without slowing down the usual case, when code isn't
// interchanging indices between views.
//
// Beware! In substrings whose bounds aren't `Character`-aligned, extended
// grapheme breaks are sometimes in different places than in their base string.
// (The sequence of characters in a substring depend only on the Unicode scalars
// that make up its contents, not on their surrounding context.) Therefore, such
// substrings must not look at or set this bit: indices must be reliably
// interchangeable between strings and their associated substrings, even if the
// latter are irregular.
//
// Note that `startIndex` and `endIndex` have fully inlinable implementations.
// This means that when code built on older releases runs on 5.7, this bit may
// not be set on these, even though they are always `Character`-aligned. This is
// fine -- `index(after:)` and `index(before:)` still do the right thing with
// minimal/no performance loss. (The start/end index is handled specially.)
extension String.Index {
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _isCharacterAligned: Bool {
    0 != _rawBits & Self.__characterAlignmentBit
  }

  /// Return the same index with both the scalar- and `Character`-aligned bits
  /// set.
  ///
  /// (`Character` alignment implies scalar alignment.)
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _characterAligned: String.Index {
    let r = _rawBits | Self.__characterAlignmentBit | Self.__scalarAlignmentBit
    let idx = Self(r)
    idx._invariantCheck()
    return idx
  }
}

extension String.Index {
  @_alwaysEmitIntoClient // Swift 5.7
  internal func _copyingAlignment(from index: Self) -> Self {
    let mask = Self.__scalarAlignmentBit | Self.__characterAlignmentBit
    return Self((_rawBits & ~mask) | (index._rawBits & mask))
  }
}

// ### Index Encoding
//
// Swift 5.7 introduced bookkeeping to keep track of the Unicode encoding
// associated with the position value in String indices. Indices whose position
// is an offset into UTF-8 storage come with the corresponding flag set, and a
// separate flag is set for UTF-16 indices. (Only foreign strings can be UTF-16
// encoded. As of 5.7, all foreign strings are UTF-16; but this is subject to
// change later if we ever decide to implement additional foreign forms.)
//
// In releases before 5.7, the bits corresponding to these flags were considered
// reserved, and they were both set to zero in inlinable code. This means that
// (on ABI stable platforms at least) we cannot assume that either of these bits
// will be reliably set. If they are both clear, then we must fall back to
// assuming that the index has the right encoding for whatever string it is used
// on. However, if either of these bits are set, then the other bit's value is
// also reliable -- whether it's set or cleared.
//
// The indices of ASCII strings are encoding-independent, i.e. transcoding such
// strings from UTF-8 to UTF-16 (or vice versa) does not change the position
// value of any of their indices. Therefore it isn't an error for an index to
// have both of these flags set. (The start index of every string also behaves
// this way: position zero is the same no matter how what encoding is used for
// the rest of string.)
//
// These two bits (along with the isForeignUTF8 flag in StringObject) allow
// newer versions of the Standard Library to more reliably catch runtime errors
// where client code is applying an index from a UTF-16 string to a UTF-8 one,
// or vice versa. This typically happens when indices from a UTF-16 Cocoa string
// that was verbatim bridged into Swift are accidentally applied to a mutated
// version of the same string. (The mutation turns it into a UTF-8 native
// string, where the same numerical offsets might correspond to wildly different
// logical positions.)
//
// Such code has always been broken, as the old indices are documented to be no
// longer valid after the mutation; however, in previous releases such cases
// weren't reliably detected, and if the code was only ever tested on ASCII
// strings, then the bug could lie dormant for a long time. (Until the code
// encounters a non-ASCII character and someone gets surprised that the results
// no longer make sense.)
//
// As more code gets rebuilt with Swift 5.7+, the stdlib will gradually become
// able to reliably catch and correct all such issues. The error cases are
// handled in `_StringGuts.ensureMatchingEncoding(_:)`; see there for the sordid
// details.
extension String.Index {
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _encodingBits: UInt64 {
    _rawBits & (Self.__utf8Bit | Self.__utf16Bit)
  }

  /// Returns true if the position in this index can be interpreted as an offset
  /// into UTF-8-encoded string storage.
  ///
  /// (This returns true if either we know for sure that this is an UTF-8 index,
  /// or if we don't have enough information to determine its encoding.)
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _canBeUTF8: Bool {
    // The only way an index cannot be UTF-8 is it has only the UTF-16 flag set.
    _encodingBits != Self.__utf16Bit
  }

  /// Returns true if the position in this index can be interpreted as offset
  /// into UTF-16-encoded string storage.
  ///
  /// (This returns true if either we know for sure that this is an UTF-16
  /// index, or if we don't have enough information to determine its
  /// encoding.)
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _canBeUTF16: Bool {
    // The only way an index cannot be UTF-16 is it has only the UTF-8 flag set.
    _encodingBits != Self.__utf8Bit
  }

  /// Returns true if the encoding of this index isn't known to be in conflict
  /// with the specified encoding.
  ///
  /// If the index was created by code that was built on a stdlib below 5.7,
  /// then this check may incorrectly return true on a mismatching index, but it
  /// is guaranteed to never incorrectly return false. If all loaded binaries
  /// were built in 5.7+, then this method is guaranteed to always return the
  /// correct value.
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal func _hasMatchingEncoding(isUTF8 utf8: Bool) -> Bool {
    _encodingBits != Self.__encodingBit(utf16: utf8)
  }

  /// Returns the same index with the UTF-8 bit set.
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _knownUTF8: Self { Self(_rawBits | Self.__utf8Bit) }

  /// Returns the same index with the UTF-16 bit set.
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _knownUTF16: Self { Self(_rawBits | Self.__utf16Bit) }

  /// Returns the same index with both UTF-8 & UTF-16 bits set.
  @_alwaysEmitIntoClient // Swift 5.7
  @inline(__always)
  internal var _encodingIndependent: Self {
    Self(_rawBits | Self.__utf8Bit | Self.__utf16Bit)
  }

  @_alwaysEmitIntoClient // Swift 5.7
  internal func _copyingEncoding(from index: Self) -> Self {
    let mask = Self.__utf8Bit | Self.__utf16Bit
    return Self((_rawBits & ~mask) | (index._rawBits & mask))
  }
}

extension String.Index: Equatable {
  @inlinable @inline(__always)
  public static func == (lhs: String.Index, rhs: String.Index) -> Bool {
    return lhs.orderingValue == rhs.orderingValue
  }
}

extension String.Index: Comparable {
  @inlinable @inline(__always)
  public static func < (lhs: String.Index, rhs: String.Index) -> Bool {
    return lhs.orderingValue < rhs.orderingValue
  }
}

extension String.Index: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(orderingValue)
  }
}

@available(SwiftStdlib 5.7, *)
extension String.Index: CustomStringConvertible {
  internal var _encodingDescription: String {
    switch (_rawBits & Self.__utf8Bit != 0, _rawBits & Self.__utf16Bit != 0) {
    case (false, false): return "unknown"
    case (true, false): return "utf8"
    case (false, true): return "utf16"
    case (true, true): return "any"
    }
  }

  @available(SwiftStdlib 5.7, *)
  @inline(never)
  public var description: String {
    // For the regular `description`, we just print the offset value along with
    // its encoding and the transcoded offset, in a compact form: `23.utf8+1`.
    //
    // This is intended to expose crucial positioning information in a form that
    // remains relatively readable even when part of a larger printout, e.g., as
    // with range expressions like `12[utf8]..<56[utf8]+1`.
    var d = "\(_encodedOffset)[\(_encodingDescription)]"
    if transcodedOffset != 0 {
      d += "+\(transcodedOffset)"
    }
    return d
  }
}

@available(SwiftStdlib 5.7, *)
extension String.Index: CustomDebugStringConvertible {
  @available(SwiftStdlib 5.7, *)
  @inline(never)
  public var debugDescription: String {
    var d = "String.Index("
    d += "offset: \(_encodedOffset)"
    d += ", encoding: \(_encodingDescription)"
    if transcodedOffset != 0 {
      d += ", transcodedOffset: \(transcodedOffset)"
    }
    if _isCharacterAligned {
      d += ", aligned: character"
    } else if _isScalarAligned {
      d += ", aligned: scalar"
    }
    if let stride = characterStride {
      d += ", stride: \(stride)"
    }
    d += ")"
    return d
  }
}

#if SWIFT_ENABLE_REFLECTION
@available(SwiftStdlib 5.7, *)
extension String.Index: CustomReflectable {
  @available(SwiftStdlib 5.7, *)
  @inline(never)
  public var customMirror: Mirror {
    var children: [(label: String?, value: Any)] = []
    children.reserveCapacity(5)
    children.append(("_offset", _encodedOffset))
    children.append(("_encoding", _encodingDescription))
    if transcodedOffset > 0 {
      children.append(("_transcodedOffset", transcodedOffset))
    }
    if _isCharacterAligned {
      children.append(("_aligned", "character"))
    } else if _isScalarAligned {
      children.append(("_aligned", "scalar"))
    }
    if let stride = characterStride {
      children.append(("_stride", stride))
    }
    return Mirror(self, children: children, displayStyle: .struct)
  }
}
#endif

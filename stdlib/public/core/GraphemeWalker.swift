//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// GraphemeWalker is a tool to determine the Unicode grapheme break boundaries.
// This works on strings of UTF8 and UTF16 (for bridged NSString) and returns
// either the next or previous stride of the grapheme cluster. For example:
//
//   let someFlags = "🇺🇸🇬🇧"
//
// Byte and Scalar Representation:
//
//   |(       🇺       ) (        🇸       )|(        🇬       ) (        🇧      )|
//   [240, 159, 135, 186, 240, 159, 135, 184, 240, 159, 135, 172, 240, 159, 135, 167]
//    ^                                        ^
//    | = String's start index                 | = String's next index
//
//
// In this representation we see that there are a total of 16 utf8 code units,
// but we don't want to report the count of a string as 16 to a user to sees
// two emoji "character"s. We also see a total of 4 Unicode scalars, 2 of which
// make up the single U.S. flag (two regional indicators), i.e. these two create
// a "grapheme cluster" who represent a singular grapheme or in Swift's terms,
// "Character". The job of GraphemeWalker is to determine these grapheme cluster
// boundaries and to correctly report that the string "🇺🇸🇬🇧" has two characters
// (🇺🇸 and 🇬🇧).
internal struct GraphemeWalker {
  // This is either a buffer of utf8 code units for native Swift strings, or
  // a buffer of utf16 code units when working with bridged NSStrings.
  let buffer: UnsafeRawBufferPointer
  var offset = 0
  var stride = 0
  var isUTF8 = true

  var inEmojiSequence = false
  var shouldBreakRI = false

  var utf8: UnsafeBufferPointer<UInt8> {
    _internalInvariant(isUTF8)

    let base = buffer.baseAddress._unsafelyUnwrappedUnchecked

    // This is ok because we're given a typed buffer to UInt8 that we type
    // erase on init.
    let start = base.assumingMemoryBound(to: UInt8.self)
    return UnsafeBufferPointer(start: start, count: buffer.count)
  }

  var utf16: UnsafeBufferPointer<UInt16> {
    _internalInvariant(!isUTF8)

    let base = buffer.baseAddress._unsafelyUnwrappedUnchecked

    // This is ok because we're given a typed buffer to UInt16 that we type
    // erase on init.
    let start = base.assumingMemoryBound(to: UInt16.self)
    return UnsafeBufferPointer(start: start, count: buffer.count)
  }

  init(buffer: UnsafeBufferPointer<UInt8>, offset: Int) {
    self.buffer = UnsafeRawBufferPointer(buffer)
    self.offset = offset
  }

  init(buffer: UnsafeBufferPointer<UInt16>, offset: Int) {
    self.buffer = UnsafeRawBufferPointer(buffer)
    self.offset = offset

    self.isUTF8 = false
  }

  func decodeScalar(startingAt idx: Int) -> (Unicode.Scalar, Int) {
    if _fastPath(isUTF8) {
      return _decodeScalar(utf8, startingAt: idx)

    // We're UTF16.
    } else {
      return _decodeScalar(utf16, startingAt: idx)
    }
  }

  // Returns the stride of the next grapheme cluster at the previous boundary at
  // offset.
  mutating func nextStride() -> Int {
    while true {
      let (sc1, len1) = decodeScalar(startingAt: offset &+ stride)

      stride &+= len1

      let (sc2, len2) = decodeScalar(startingAt: offset &+ stride)

      let gbp1 = Unicode.GraphemeBreakProperty(from: sc1)
      let gbp2 = Unicode.GraphemeBreakProperty(from: sc2)

      if shouldBreak(gbp1, between: gbp2) {
        return stride
      }

      // If we've reached the end of the buffer, then there's nothing else for
      // us to do.
      if (offset &+ stride &+ len2) == buffer.count {
        return stride &+ len2
      }
    }
  }

  // The "algorithm" that determines whether or not we should break between
  // certain grapheme break properties.
  //
  // This is based off of the Unicode Annex #29 for [Grapheme Cluster Boundary
  // Rules](https://unicode.org/reports/tr29/#Grapheme_Cluster_Boundary_Rules).
  mutating func shouldBreak(
    _ x: Unicode.GraphemeBreakProperty,
    between y: Unicode.GraphemeBreakProperty
  ) -> Bool {
    var enterEmojiSequence = false

    defer {
      inEmojiSequence = enterEmojiSequence
    }

    switch (x, y) {

    // GB3
    case (.cr, .lf):
      return false

    // GB4
    case (.control, _),
         (.cr, _),
         (.lf, _):
      return true

    // GB5
    case (_, .control),
         (_, .cr),
         (_, .lf):
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

    // GB9 & GB9a (partial GB11)
    case (_, .extend),
         (_, .zwj),
         (_, .spacingMark):
      if inEmojiSequence || x == .extendedPictographic, y != .spacingMark {
        enterEmojiSequence = true
      }
      return false

    // GB9b
    case (.prepend, _):
      return false

    // GB11
    case (.zwj, .extendedPictographic):
      if inEmojiSequence {
        return false
      } else {
        return true
      }

    // GB12 & GB13
    case (.regionalIndicator, .regionalIndicator):
      shouldBreakRI.toggle()
      return !shouldBreakRI

    // GB999
    default:
      return true
    }
  }
}

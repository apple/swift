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

// GraphemeIterator is a tool to determine the Unicode grapheme break boundaries.
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
// "Character". The job of GraphemeIterator is to determine these grapheme
// cluster boundaries and to correctly report that the string "🇺🇸🇬🇧" has two
// characters (🇺🇸 and 🇬🇧).
internal struct GraphemeIterator {
  let scalars: String.UnicodeScalarView

  var index: String.Index
  var offset: Int

  var cachedGBP: (Unicode.Scalar, Unicode.GraphemeBreakProperty)? = nil

  var inEmojiSequence: Bool? = nil
  var shouldBreakRI: Bool? = nil

  init(_ string: String, offset: Int) {
    self.scalars = string.unicodeScalars
    self.index = String.Index(_encodedOffset: offset)
    self.offset = offset
  }

  mutating func getGraphemeBreakProperty(
    for scalar: Unicode.Scalar
  ) -> Unicode.GraphemeBreakProperty {
    if let cache = cachedGBP, cache.0 == scalar {
      return cache.1
    }

    let gbp = Unicode.GraphemeBreakProperty(from: scalar)
    cachedGBP = (scalar, gbp)
    return gbp
  }
}


extension GraphemeIterator: IteratorProtocol {
  // Returns the stride of the next grapheme cluster at the previous boundary at
  // offset.
  mutating func next() -> Int? {
    guard index != scalars.endIndex else {
      return nil
    }

    if inEmojiSequence == nil, shouldBreakRI == nil {
      inEmojiSequence = false
      shouldBreakRI = false
    }

    while scalars.index(after: index) != scalars.endIndex {
      let sc1 = scalars[index]
      scalars.formIndex(after: &index)

      let sc2 = scalars[index]

      let gbp1 = getGraphemeBreakProperty(for: sc1)
      let gbp2 = getGraphemeBreakProperty(for: sc2)

      if shouldBreak(gbp1, between: gbp2, in: &self) {
        return index._encodedOffset &- offset
      }
    }

    // If we've reached the end of the buffer, then there's nothing else for
    // us to do.
    return scalars.index(after: index)._encodedOffset &- offset
  }

  // Returns the stride of the previous grapheme cluster at the current boundary
  // at offset.
  mutating func previous() -> Int? {
    guard index != scalars.startIndex else {
      return nil
    }

    scalars.formIndex(before: &index)

    repeat {
      let sc2 = scalars[index]
      let sc2Idx = index

      scalars.formIndex(before: &index)

      let sc1 = scalars[index]

      let gbp2 = getGraphemeBreakProperty(for: sc2)
      let gbp1 = getGraphemeBreakProperty(for: sc1)

      if shouldBreak(gbp1, between: gbp2, in: &self) {
        return offset &- sc2Idx._encodedOffset
      }
    } while index != scalars.startIndex

    // If we've reached the beginning of the buffer, then there's nothing else
    // for us to do.
    return offset &- index._encodedOffset
  }
}

// The "algorithm" that determines whether or not we should break between
// certain grapheme break properties.
//
// This is based off of the Unicode Annex #29 for [Grapheme Cluster Boundary
// Rules](https://unicode.org/reports/tr29/#Grapheme_Cluster_Boundary_Rules).
internal func shouldBreak(
  _ x: Unicode.GraphemeBreakProperty,
  between y: Unicode.GraphemeBreakProperty,
  in iterator: inout GraphemeIterator
) -> Bool {
  var enterEmojiSequence = false

  defer {
    iterator.inEmojiSequence = enterEmojiSequence
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

  // GB9 (partial GB11)
  case (_, .extend),
       (_, .zwj):
    if iterator.inEmojiSequence ?? false || x == .extendedPictographic {
      enterEmojiSequence = true
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
    // This is only ever nil when walking backwards.
    if iterator.inEmojiSequence == nil {
      checkIfInEmojiSequence(in: &iterator)
    }

    if iterator.inEmojiSequence! {
      return false
    } else {
      return true
    }

  // GB12 & GB13
  case (.regionalIndicator, .regionalIndicator):
    // This is only ever nil when walking backwards.
    if iterator.shouldBreakRI == nil {
      countRIs(in: &iterator)
    }

    iterator.shouldBreakRI?.toggle()
    return !iterator.shouldBreakRI!

  // GB999
  default:
    return true
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
internal func checkIfInEmojiSequence(in iterator: inout GraphemeIterator) {
  var emojiIdx = iterator.index

  guard emojiIdx != iterator.scalars.startIndex else {
    iterator.inEmojiSequence = false
    return
  }

  repeat {
    iterator.scalars.formIndex(before: &emojiIdx)

    let sc = iterator.scalars[emojiIdx]
    let gbp = iterator.getGraphemeBreakProperty(for: sc)

    switch gbp {
    case .extend:
      continue
    case .extendedPictographic:
      iterator.inEmojiSequence = true
      return
    default:
      iterator.inEmojiSequence = false
      return
    }
  } while emojiIdx != iterator.scalars.startIndex
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
internal func countRIs(in iterator: inout GraphemeIterator) {
  var riIdx = iterator.index

  guard riIdx != iterator.scalars.startIndex else {
    iterator.shouldBreakRI = false
    return
  }

  var riCount = 0

  repeat {
    iterator.scalars.formIndex(before: &riIdx)

    let sc = iterator.scalars[riIdx]
    let gbp = iterator.getGraphemeBreakProperty(for: sc)

    guard gbp == .regionalIndicator else {
      break
    }

    riCount += 1
  } while riIdx != iterator.scalars.startIndex

  iterator.shouldBreakRI = riCount & 1 != 0
}

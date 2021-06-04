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

import GenUtils

extension Unicode {
  enum GraphemeBreakProperty: UInt32 {
    // We don't store the other properties, so we really don't care about them
    // here.

    case control = 0
    case extend = 1
    case prepend = 2
    case spacingMark = 3
    case extendedPictographic = 4

    init?(_ str: String) {
      switch str {
      case "Extend":
        self = .extend

      // Although CR and LF are distinct properties, we have fast paths in place
      // for those cases, so combine them here to allow for more contiguous
      // ranges.
      case "Control",
           "CR",
           "LF":
        self = .control
      case "Prepend":
        self = .prepend
      case "SpacingMark":
        self = .spacingMark
      case "Extended_Pictographic":
        self = .extendedPictographic
      default:
        return nil
      }
    }
  }
}

// Takes an unflattened array of scalar ranges and grapheme break properties and
// attempts to merge ranges who share the same break property. E.g:
//
//     0x0 ... 0xA  = .control
//     0xB ... 0xB  = .control
//     0xC ... 0x1F = .control
//
//    into:
//
//    0x0 ... 0x1F = .control
func flatten(
  _ unflattened: [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)]
) -> [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)] {
  var result: [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)] = []

  for elt in unflattened.sorted(by: { $0.0.lowerBound < $1.0.lowerBound }) {
    guard !result.isEmpty, result.last!.1 == elt.1 else {
      result.append(elt)
      continue
    }
    
    if elt.0.lowerBound == result.last!.0.upperBound + 1 {
      result[result.count - 1].0 = result.last!.0.lowerBound ... elt.0.upperBound
    } else {
      result.append(elt)
    }
  }

  return result
}

// Given a path to one of the Unicode data files, reads it and returns the
// unflattened list of scalar & grapheme break property.
//
// Each line in one of these data files is formatted like the following:
//
//     007F..009F    ; Control # Cc  [33] <control-007F>..<control-009F>
//     00AD          ; Control # Cf       SOFT HYPHEN
//
// Where each section is split by a ';'. The first section informs us of either
// the range of scalars who conform to this property or the singular scalar
// who does. The second section tells us what grapheme break property these
// scalars conform to. There are extra comments telling us what general
// category these scalars are a part of, how many scalars are in a range, and
// the name of the scalars.
func getGraphemeBreakPropertyData(
  for path: String
) -> [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)] {
  let data = readFile(path)

  var unflattened: [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)] = []

  for line in data.split(separator: "\n") {
    // Skip comments
    guard !line.hasPrefix("#") else {
      continue
    }

    // Each line in this file is broken up into two sections:
    // 1: Either the singular scalar or a range of scalars who conform to said
    //    grapheme break property.
    // 2: The grapheme break property that said scalar(s) conform to (with
    //    additional comments noting the character category, name and amount of
    //    scalars the range represents).
    let components = line.split(separator: ";")

    // Get the property first because it may be one we don't care about.
    let splitProperty = components[1].split(separator: "#")
    let filteredProperty = splitProperty[0].filter { !$0.isWhitespace }

    guard let gbp = Unicode.GraphemeBreakProperty(filteredProperty) else {
      continue
    }

    let scalars: ClosedRange<UInt32>

    let filteredScalars = components[0].filter { !$0.isWhitespace }

    // If we have . appear, it means we have a legitimate range. Otherwise,
    // it's a singular scalar.
    if filteredScalars.contains(".") {
      let range = filteredScalars.split(separator: ".")

      scalars = UInt32(range[0], radix: 16)! ... UInt32(range[1], radix: 16)!
    } else {
      let scalar = UInt32(filteredScalars, radix: 16)!

      scalars = scalar ... scalar
    }

    unflattened.append((scalars, gbp))
  }

  return unflattened
}

// Takes the flattened data and writes it as a static C array.
func emit(
  _ data: [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)],
  into result: inout String
) {
  result += """
  static __swift_uint32_t _swift_stdlib_graphemeBreakProperties[\(data.count)] = {

  """

  formatCollection(data, into: &result) { (range, gbp) -> String in
    // Our value uses the 21 bits to represent the scalar, 8 bits to represent
    // the range's count, and finally the last three bits to represent the
    // grapheme break property enum.

    // E.g. for the top 3 bits:
    // 000 = control
    // 001 = extend
    // 010 = prepend
    // 011 = spacingMark
    // 100 = extendedPictographic (not using the extra range bit)
    // 101 = extendedPictographic (using the extra range bit)
    // 110 = not used (will never occur)
    // 111 = not used (will never occur)

    var value = range.lowerBound
    value |= UInt32(range.count - 1) << 21
    value |= gbp.rawValue << 29

    // However, for extendedPictographic because we don't use second to top bit,
    // we can use an extra bit for the range value to store more.
    if gbp == .extendedPictographic {
      value = range.lowerBound
      value |= UInt32(range.count - 1) << 21
      value |= 1 << 31
    }

    return "0x\(String(value, radix: 16))"
  }

  result += "\n};\n\n"
}

// Writes the stdlib internal routine for binary searching the grapheme array.
func emitAccessor(
  _ dataCount: Int,
  into result: inout String
) {
  result += """
  SWIFT_RUNTIME_STDLIB_INTERNAL
  __swift_uint8_t _swift_stdlib_getGraphemeBreakProperty(__swift_uint32_t scalar) {
    auto low = 0;
    auto high = \(dataCount) - 1;

    while (high >= low) {
      auto idx = low + (high - low) / 2;

      auto entry = _swift_stdlib_graphemeBreakProperties[idx];

      // Shift the enum and range count out of the value.
      auto lower = (entry << 11) >> 11;

      // Shift the enum out first, then shift out the scalar value.
      auto upper = lower + ((entry << 3) >> 24);

      // Shift everything out.
      auto enumValue = (__swift_uint8_t)(entry >> 29);

      // Special case: extendedPictographic who used an extra bit for the range.
      if (enumValue == 5) {
        upper = lower + ((entry << 2) >> 23);
      }

      if (scalar >= lower && scalar <= upper) {
        return enumValue;
      }

      if (scalar > upper) {
        low = idx + 1;
        continue;
      }

      if (scalar < lower) {
        high = idx - 1;
        continue;
      }
    }

    // If we made it out here, then our scalar was not found in the grapheme
    // array (this occurs when a scalar doesn't map to any grapheme break
    // property). Return the max value here to indicate .any.
    return 0xFF;
  }

  """
}

// Main entry point into the grapheme break property generator.
func generateGraphemeBreakProperty() {
  var result = readFile("Input/UnicodeGrapheme.cpp")

  let baseData = getGraphemeBreakPropertyData(
    for: "Data/GraphemeBreakProperty.txt"
  )
  let emojiData = getGraphemeBreakPropertyData(for: "Data/emoji-data.txt")

  let flattened = flatten(baseData + emojiData)

  var data: [(ClosedRange<UInt32>, Unicode.GraphemeBreakProperty)] = []

  for (range, gbp) in flattened {
    guard range.count < 0x200 else {
      let lower = String(range.lowerBound, radix: 16)
      let upper = String(range.upperBound, radix: 16)

      print("Manual range: 0x\(lower) ... 0x\(upper) for \(gbp)")
      continue
    }

    data.append((range, gbp))
  }

  emit(data, into: &result)

  emitAccessor(data.count, into: &result)

  write(result, to: "Output/UnicodeGrapheme.cpp")
}

generateGraphemeBreakProperty()

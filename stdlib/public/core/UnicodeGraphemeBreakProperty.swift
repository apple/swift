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

import SwiftShims

extension Unicode {
  internal enum GraphemeBreakProperty {
    case any
    case control
    case cr
    case extend
    case extendedPictographic
    case l
    case lf
    case lv
    case lvt
    case prepend
    case regionalIndicator
    case spacingMark
    case t
    case v
    case zwj

    init(from scalar: Unicode.Scalar) {
      switch scalar.value {
      case 0xA:
        self = .lf
      case 0xD:
        self = .cr

      // Some fast paths for ascii characters...
      case 0x0 ... 0x1F:
        self = .control
      case 0x20 ... 0x7E:
        self = .any

      case 0x200D:
        self = .zwj
      case 0x1100 ... 0x115F,
           0xA960 ... 0xA97C:
        self = .l
      case 0x1160 ... 0x11A7,
           0xD7B0 ... 0xD7C6:
        self = .v
      case 0x11A8 ... 0x11FF,
           0xD7CB ... 0xD7FB:
        self = .t
      case 0xAC00 ... 0xD7A3:
        if scalar.value % 28 == 16 {
          self = .lv
        } else {
          self = .lvt
        }
      case 0x1F1E6 ... 0x1F1FF:
        self = .regionalIndicator
      case 0x1FC00 ... 0x1FFFD:
        self = .extendedPictographic
      case 0xE01F0 ... 0xE0FFF:
        self = .control
      default:
        // Otherwise, default to binary searching the data array.
        let rawEnumValue = _swift_stdlib_getGraphemeBreakProperty(scalar.value)

        switch rawEnumValue {
        case 0:
          self = .control
        case 1:
          self = .extend
        case 2:
          self = .prepend
        case 3:
          self = .spacingMark

        // Extended pictographic uses 2 values for its representation.
        case 4, 5:
          self = .extendedPictographic
        default:
          self = .any
        }
      }
    }
  }
}

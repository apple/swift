//===--- ProcMapsScanner.swift --------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Defines ProcMapsScanner, which is for scanning the /proc/<pid>/maps
//  pseudofiles on Linux.
//
//===----------------------------------------------------------------------===//

#if os(Linux)

import Swift

// Lines in /proc/pid/maps files match the following regex:
//
//     ^(?<start>[A-Fa-f0-9]+)-(?<end>[A-Fa-f0-9]+)\s+
//     (?<perms>[-rwxsp]{4})\s+
//     (?<offset>[A-Fa-f0-9]+)\s+
//     (?<major>[A-Fa-f0-9]+):(?<minor>[A-Fa-f0-9]+)\s+
//     (?<inode>\d+)\s+
//     (?<pathname>.*)\s*$

struct ProcMapsScanner<S: StringProtocol>: Sequence, IteratorProtocol {
  typealias SS = S.SubSequence

  struct Match {
    var start: SS
    var end: SS
    var perms: SS
    var offset: SS
    var major: SS
    var minor: SS
    var inode: SS
    var pathname: SS
  }

  private enum State {
    case start
    case scanningStart
    case scanningEnd
    case afterEnd
    case scanningPerms(Int)
    case afterPerms
    case scanningOffset
    case afterOffset
    case scanningMajor
    case scanningMinor
    case afterMinor
    case scanningInode
    case afterInode
    case scanningPathname
    case scanningPathnameWhitespace
  }

  private var procMaps: S
  private var procMapsUTF8: S.UTF8View
  private var ndx: S.UTF8View.Index

  init(_ string: S) {
    procMaps = string
    procMapsUTF8 = string.utf8
    ndx = procMapsUTF8.startIndex
  }

  mutating func scanMatch() -> Match? {
    var match: Match = Match(start: "",
                             end: "",
                             perms: "",
                             offset: "",
                             major: "",
                             minor: "",
                             inode: "",
                             pathname: "")
    var currentChunk = ndx
    var state: State = .start

    func isPerm(_ ch: UInt8) -> Bool {
      return ch == UInt8(ascii: "-") || ch == UInt8(ascii: "r")
        || ch == UInt8(ascii: "w") || ch == UInt8(ascii: "x")
        || ch == UInt8(ascii: "s") || ch == UInt8(ascii: "p")
    }

    func isDecimal(_ ch: UInt8) -> Bool {
      return ch >= UInt8(ascii: "0") && ch <= UInt8(ascii: "9")
    }

    func isHex(_ ch: UInt8) -> Bool {
      return ch >= UInt8(ascii: "A") && ch <= UInt8(ascii: "F")
        || ch >= UInt8(ascii: "a") && ch <= UInt8(ascii: "f")
        || ch >= UInt8(ascii: "0") && ch <= UInt8(ascii: "9")
    }

    func isWhitespace(_ ch: UInt8) -> Bool {
      return ch == UInt8(ascii: " ") || ch == UInt8(ascii: "\t")
    }

    while ndx < procMapsUTF8.endIndex {
      let ch = procMapsUTF8[ndx]
      let next = procMapsUTF8.index(after: ndx)

      switch state {
        case .start:
          if !isHex(ch) {
            return nil
          }
          state = .scanningStart
        case .scanningStart:
          if ch == UInt8(ascii: "-") {
            match.start = procMaps[currentChunk..<ndx]
            state = .scanningEnd
            currentChunk = next
          } else if !isHex(ch) {
            return nil
          }
        case .scanningEnd:
          if isWhitespace(ch) {
            match.end = procMaps[currentChunk..<ndx]
            state = .afterEnd
          } else if !isHex(ch) {
            return nil
          }
        case .afterEnd:
          if isPerm(ch) {
            currentChunk = ndx
            state = .scanningPerms(1)
          } else if !isWhitespace(ch) {
            return nil
          }
        case let .scanningPerms(length):
          if length == 4 {
            if isWhitespace(ch) {
              match.perms = procMaps[currentChunk..<ndx]
              state = .afterPerms
            } else {
              return nil
            }
          } else if isPerm(ch) {
            state = .scanningPerms(length + 1)
          } else {
            return nil
          }
        case .afterPerms:
          if isHex(ch) {
            currentChunk = ndx
            state = .scanningOffset
          } else if !isWhitespace(ch) {
            return nil
          }
        case .scanningOffset:
          if isWhitespace(ch) {
            match.offset = procMaps[currentChunk..<ndx]
            state = .afterOffset
          } else if !isHex(ch) {
            return nil
          }
        case .afterOffset:
          if isHex(ch) {
            currentChunk = ndx
            state = .scanningMajor
          } else if !isWhitespace(ch) {
            return nil
          }
        case .scanningMajor:
          if ch == UInt8(ascii: ":") {
            match.major = procMaps[currentChunk..<ndx]
            state = .scanningMinor
            currentChunk = next
          } else if !isHex(ch) {
            return nil
          }
        case .scanningMinor:
          if isWhitespace(ch) {
            match.minor = procMaps[currentChunk..<ndx]
            state = .afterMinor
          } else if !isHex(ch) {
            return nil
          }
        case .afterMinor:
          if isDecimal(ch) {
            currentChunk = ndx
            state = .scanningInode
          } else if !isWhitespace(ch) {
            return nil
          }
        case .scanningInode:
          if isWhitespace(ch) {
            match.inode = procMaps[currentChunk..<ndx]
            state = .afterInode
          } else if !isDecimal(ch) {
            return nil
          }
        case .afterInode:
          if ch == 0x0a {
            ndx = next
            return match
          } else if !isWhitespace(ch) {
            currentChunk = ndx
            state = .scanningPathname
          }
        case .scanningPathname:
          if isWhitespace(ch) || ch == 0x0a {
            match.pathname = procMaps[currentChunk..<ndx]
            state = .scanningPathnameWhitespace
            if ch == 0x0a {
              ndx = next
              return match
            }
          }
        case .scanningPathnameWhitespace:
          if !isWhitespace(ch) {
            state = .scanningPathname
          } else if ch == 0x0a {
            ndx = next
            return match
          }
      }

      ndx = next
    }

    if case .scanningPathname = state {
      match.pathname = procMaps[currentChunk...]
    }

    return match
  }

  mutating func next() -> Match? {
    while ndx != procMapsUTF8.endIndex {
      if let match = scanMatch() {
        return match
      }

      // If we failed to match, skip to end of line and retry
      while ndx != procMapsUTF8.endIndex {
        let ch = procMapsUTF8[ndx]
        ndx = procMapsUTF8.index(after: ndx)
        if ch == 0x0a {
          break
        }
      }
    }

    return nil
  }
}

#endif // os(Linux)

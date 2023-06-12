//===--- Utils.swift - Utility functions ----------------------------------===//
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
// Utility functions that are used in the backtracing library.
//
//===----------------------------------------------------------------------===//

import Swift

@_implementationOnly import OS.Libc

internal func hex<T: FixedWidthInteger>(_ value: T,
                                        prefix shouldPrefix: Bool = true,
                                        width: Int = MemoryLayout<T>.size * 2)
  -> String {
  let digits = String(value, radix: 16)
  let padding = digits.count >= width ? "" : String(repeating: "0",
                                                    count: width - digits.count)
  let prefix = shouldPrefix ? "0x" : ""

  return "\(prefix)\(padding)\(digits)"
}

internal func hex(_ bytes: [UInt8]) -> String {
  return bytes.map{ hex($0, prefix: false) }.joined(separator: "")
}

enum PadAlignment {
  case left
  case right
}

func pad<T>(_ value: T, _ width: Int, align: PadAlignment = .left) -> String {
  let string = String(describing: value)
  let padding = string.count >= width ? "" : String(repeating: " ",
                                                    count: width - string.count)
  switch align {
    case .left:
      return string + padding
    case .right:
      return padding + string
  }
}

@_spi(Utils)
public func readString(from file: String) -> String? {
  let fd = _swift_open(file, O_RDONLY, 0)
  if fd < 0 {
    return nil
  }
  defer { close(fd) }

  // Files in /proc are awkward; you can't get their length and then
  // read the data in one chunk, because they have zero length and don't
  // support seeking.
  var bytes: [UInt8] = []
  withUnsafeTemporaryAllocation(of: UInt8.self, capacity: 4096) { buffer in
    while true {
      let bytesRead = read(fd, buffer.baseAddress, buffer.count)
      if bytesRead <= 0 {
        break
      }

      bytes.append(contentsOf: buffer[0..<bytesRead])
    }
  }

  return String(decoding: bytes, as: UTF8.self)
}

@_spi(Utils)
public func stripWhitespace<S: StringProtocol>(_ s: S)
    -> S.SubSequence {
  guard let firstNonWhitespace = s.firstIndex(where: { !$0.isWhitespace })
  else {
    return ""
  }
  let lastNonWhitespace = s.lastIndex(where: { !$0.isWhitespace })!
  return s[firstNonWhitespace...lastNonWhitespace]
}

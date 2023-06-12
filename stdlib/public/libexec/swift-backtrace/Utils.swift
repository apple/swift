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
// Utility functions that are used in the swift-backtrace program.
//
//===----------------------------------------------------------------------===//

#if canImport(Darwin)
import Darwin.C
#elseif canImport(Glibc)
import Glibc
#elseif canImport(CRT)
import CRT
#endif

import Swift

internal func hex<T: FixedWidthInteger>(_ value: T,
                                        withPrefix: Bool = true) -> String {
  let digits = String(value, radix: 16)
  let padTo = value.bitWidth / 4
  let padding = digits.count >= padTo ? "" : String(repeating: "0",
                                                    count: padTo - digits.count)
  let prefix = withPrefix ? "0x" : ""

  return "\(prefix)\(padding)\(digits)"
}

internal func hex(_ bytes: [UInt8]) -> String {
  return bytes.map{ hex($0, withPrefix: false) }.joined(separator: "")
}

internal func parseUInt64<S: StringProtocol>(_ s: S) -> UInt64? {
  if s.hasPrefix("0x") {
    return UInt64(s.dropFirst(2), radix: 16)
  } else if s.hasPrefix("0b") {
    return UInt64(s.dropFirst(2), radix: 2)
  } else if s.hasPrefix("0o") {
    return UInt64(s.dropFirst(2), radix: 8)
  } else {
    return UInt64(s, radix: 10)
  }
}

#if os(macOS) || os(Linux)

struct PosixError: Error {
  var errno: Int32

  var desription: String {
    return String(cString: strerror(self.errno))
  }
}

internal func recursiveRemoveContents(_ dir: String) throws {
  guard let dirp = opendir(dir) else {
    throw PosixError(errno: errno)
  }
  defer {
    closedir(dirp)
  }
  while let dp = readdir(dirp) {
    let name: String =
      withUnsafePointer(to: &dp.pointee.d_name) {
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
        let len = Int(dp.pointee.d_namlen)
#else
        let len = Int(strlen($0))
#endif
        return String(decoding: UnsafeRawBufferPointer(start: $0,
                                                       count: len),
                      as: UTF8.self)
      }
    if name == "." || name == ".." {
      continue
    }
    let fullPath = "\(dir)/\(name)"
    if dp.pointee.d_type == DT_DIR {
      try recursiveRemove(fullPath)
    } else {
      if unlink(fullPath) != 0 {
        throw PosixError(errno: errno)
      }
    }
  }
}

internal func recursiveRemove(_ dir: String) throws {
  try recursiveRemoveContents(dir)

  if rmdir(dir) != 0 {
    throw PosixError(errno: errno)
  }
}

internal func withTemporaryDirectory(pattern: String, shouldDelete: Bool = true,
                                     body: (String) throws -> ()) throws {
  var buf = Array<UInt8>(pattern.utf8)
  buf.append(0)

  guard let dir = buf.withUnsafeMutableBufferPointer({
    if let ptr = mkdtemp($0.baseAddress!) {
      return String(cString: ptr)
    }
    return nil
  }) else {
    throw PosixError(errno: errno)
  }

  defer {
    if shouldDelete {
      try? recursiveRemove(dir)
    }
  }

  try body(dir)
}

internal func spawn(_ path: String, args: [String]) throws {
  var cargs = args.map{ strdup($0) }
  cargs.append(nil)
  let result = cargs.withUnsafeBufferPointer{
    posix_spawn(nil, path, nil, nil, $0.baseAddress!, nil)
  }
  for arg in cargs {
    free(arg)
  }
  if result != 0 {
    throw PosixError(errno: errno)
  }
}

#endif // os(macOS)

struct CFileStream: TextOutputStream {
  var fp: UnsafeMutablePointer<FILE>

  public func write(_ string: String) {
    fputs(string, fp)
  }

  public func flush() {
    fflush(fp)
  }
}

var standardOutput = CFileStream(fp: stdout)
var standardError = CFileStream(fp: stderr)

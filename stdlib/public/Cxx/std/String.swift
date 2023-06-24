//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import CxxStdlibShim

// MARK: Initializing C++ string from a Swift String

extension std.string {
  /// Creates a C++ string having the same content as the given Swift string.
  ///
  /// - Complexity: O(*n*), where *n* is the number of UTF-8 code units in the
  ///   Swift string.
  public init(_ string: String) {
    self.init()
    for char in string.utf8 {
      self.push_back(value_type(bitPattern: char))
    }
  }

  public init(_ string: UnsafePointer<CChar>?) {
    self.init()

    guard let str = string else {
      return
    }

    let len = UTF8._nullCodeUnitOffset(in: str)
    for i in 0..<len {
      let char = UInt8(str[i])
      self.push_back(value_type(bitPattern: char))
    }
  }
}

extension std.u16string {
  /// Creates a C++ UTF-16 string having the same content as the given Swift
  /// string.
  ///
  /// - Complexity: O(*n*), where *n* is the number of UTF-16 code units in the
  ///   Swift string.
  public init(_ string: String) {
    self.init()
    for char in string.utf16 {
      self.push_back(char)
    }
  }
}

// MARK: Initializing C++ string from a Swift String literal

extension std.string: ExpressibleByStringLiteral {
  public init(stringLiteral value: String) {
    self.init(value)
  }
}

extension std.u16string: ExpressibleByStringLiteral {
  public init(stringLiteral value: String) {
    self.init(value)
  }
}

// MARK: Concatenating and comparing C++ strings

extension std.string: Equatable {
  public static func ==(lhs: std.string, rhs: std.string) -> Bool {
    return lhs.compare(rhs) == 0
  }

  public static func +=(lhs: inout std.string, rhs: std.string) {
    lhs.append(rhs)
  }

  @inlinable
  public mutating func append(_ other: std.string) {
    __appendUnsafe(other) // ignore the returned pointer
  }

  public static func +(lhs: std.string, rhs: std.string) -> std.string {
    var copy = lhs
    copy += rhs
    return copy
  }
}

extension std.u16string: Equatable {
  public static func ==(lhs: std.u16string, rhs: std.u16string) -> Bool {
    return lhs.compare(rhs) == 0
  }

  public static func +=(lhs: inout std.u16string, rhs: std.u16string) {
    lhs.append(rhs)
  }

  @inlinable
  public mutating func append(_ other: std.u16string) {
    __appendUnsafe(other) // ignore the returned pointer
  }

  public static func +(lhs: std.u16string, rhs: std.u16string) -> std.u16string {
    var copy = lhs
    copy += rhs
    return copy
  }
}

// MARK: Hashing C++ strings

extension std.string: Hashable {
  public func hash(into hasher: inout Hasher) {
    // Call std::hash<std::string>::operator()
    let cxxHash = __swift_interopHashOfString().callAsFunction(self)
    hasher.combine(cxxHash)
  }
}

extension std.u16string: Hashable {
  public func hash(into hasher: inout Hasher) {
    // Call std::hash<std::u16string>::operator()
    let cxxHash = __swift_interopHashOfU16String().callAsFunction(self)
    hasher.combine(cxxHash)
  }
}

// MARK: Getting a Swift description of a C++ string

extension std.string: CustomDebugStringConvertible {
  public var debugDescription: String {
    return "std.string(\(String(self)))"
  }
}

extension std.u16string: CustomDebugStringConvertible {
  public var debugDescription: String {
    return "std.u16string(\(String(self)))"
  }
}

extension std.string: CustomStringConvertible {
  public var description: String {
    return String(self)
  }
}

extension std.u16string: CustomStringConvertible {
  public var description: String {
    return String(self)
  }
}

// MARK: Initializing Swift String from a C++ string

extension String {
  /// Creates a String having the same content as the given C++ string.
  ///
  /// If `cxxString` contains ill-formed UTF-8 code unit sequences, this
  /// initializer replaces them with the Unicode replacement character
  /// (`"\u{FFFD}"`).
  ///
  /// - Complexity: O(*n*), where *n* is the number of bytes in the C++ string.
  public init(_ cxxString: std.string) {
    let buffer = UnsafeBufferPointer<CChar>(
      start: cxxString.__c_strUnsafe(),
      count: cxxString.size())
    self = buffer.withMemoryRebound(to: UInt8.self) {
      String(decoding: $0, as: UTF8.self)
    }
    withExtendedLifetime(cxxString) {}
  }

  /// Creates a String having the same content as the given C++ UTF-16 string.
  ///
  /// If `cxxString` contains ill-formed UTF-16 code unit sequences, this
  /// initializer replaces them with the Unicode replacement character
  /// (`"\u{FFFD}"`).
  ///
  /// - Complexity: O(*n*), where *n* is the number of bytes in the C++ UTF-16
  ///   string.
  public init(_ cxxU16String: std.u16string) {
    let buffer = UnsafeBufferPointer<UInt16>(
      start: cxxU16String.__dataUnsafe(),
      count: cxxU16String.size())
    self = String(decoding: buffer, as: UTF16.self)
    withExtendedLifetime(cxxU16String) {}
  }
}

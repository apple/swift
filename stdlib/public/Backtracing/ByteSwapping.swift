//===--- ByteSwapping.swift - Utilities for byte swapping -----------------===//
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
// Defines a ByteSwappable protocol that types can implement to indicate that
// they are able to perform byte swap operations.
//
// Mostly the types that implement this should be defined in C.
//
//===----------------------------------------------------------------------===//

import Swift

protocol ByteSwappable {
  var byteSwapped: Self { get }
  var bigEndian: Self { get }
  var littleEndian: Self { get }

  init(bigEndian: Self)
  init(littleEndian: Self)
}

extension ByteSwappable {
  init(bigEndian value: Self) {
#if _endian(big)
    self = value
#else
    self = value.byteSwapped
#endif
  }

  init(littleEndian value: Self) {
#if _endian(little)
    self = value
#else
    self = value.byteSwapped
#endif
  }

  var littleEndian: Self {
#if _endian(little)
    return self
#else
    return self.byteSwapped
#endif
  }

  var bigEndian: Self {
#if _endian(big)
    return self
#else
    return self.byteSwapped
#endif
  }
}

extension Array where Self.Element: ByteSwappable {
  mutating func swapBytes() {
    for n in 0..<self.count {
      self[n] = self[n].byteSwapped
    }
  }
}

extension UnsafeMutableBufferPointer where Self.Element: ByteSwappable {
  func swapBytes() {
    for n in 0..<self.count {
      self[n] = self[n].byteSwapped
    }
  }
}

extension Array where Self.Element: FixedWidthInteger {
  mutating func swapBytes() {
    for n in 0..<self.count {
      self[n] = self[n].byteSwapped
    }
  }
}

extension UnsafeMutableBufferPointer where Self.Element: FixedWidthInteger {
  func swapBytes() {
    for n in 0..<self.count {
      self[n] = self[n].byteSwapped
    }
  }
}

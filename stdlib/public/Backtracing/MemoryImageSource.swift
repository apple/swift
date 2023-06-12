//===--- MemoryImageSource.swift - An image source that reads from a file ---===//
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
// Defines MemoryImageSource, an image source that reads data using a
// MemoryReader.
//
//===----------------------------------------------------------------------===//

import Swift

class MemoryImageSource<M: MemoryReader>: ImageSource {
  private var reader: M

  public var isMappedImage: Bool { return true }
  public var path: String? { return nil }
  public var bounds: Bounds? { return nil }

  public init(with reader: M) {
    self.reader = reader
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    try reader.fetch(from: addr, into: buffer)
  }
}

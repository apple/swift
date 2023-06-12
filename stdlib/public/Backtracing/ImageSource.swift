//===--- ImageSource.swift - A place from which to read image data --------===//
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
// Defines ImageSource, which is a protocol that can be implemented to
// provide an image reader with a way to read data from a file, a buffer
// in memory, or wherever we might wish to read an image from.
//
//===----------------------------------------------------------------------===//

import Swift

struct ImageBounds<Address: FixedWidthInteger,
                   Size: FixedWidthInteger> {
  var base: Address
  var size: Size
  var end: Address {
    return base + Address(size)
  }

  func adjusted(by offset: some FixedWidthInteger) -> Self {
    return Self(base: base + Address(offset), size: size - Size(offset))
  }
}

protocol ImageSource: MemoryReader {
  typealias Bounds = ImageBounds<Address, Size>

  /// Says whether we are looking at a loaded image in memory or not.
  /// The layout in memory may differ from the on-disk layout; in particular,
  /// some information may not be available when the image is mapped into
  /// memory (an example is ELF section headers).
  var isMappedImage: Bool { get }

  /// If this ImageSource knows its path, this will be non-nil.
  var path: String? { get }

  /// If this ImageSource knows its bounds, this will be non-nil.
  var bounds: Bounds? { get }
}

struct ImageSourceCursor {
  typealias Address = UInt64
  typealias Size = UInt64
  typealias Bounds = ImageBounds<Address, Size>

  var source: any ImageSource
  var pos: Address

  init(source: any ImageSource, offset: Address = 0) {
    self.source = source
    self.pos = offset
  }

  public mutating func read<T>(into buffer: UnsafeMutableBufferPointer<T>) throws {
    try source.fetch(from: pos, into: buffer)
    pos += UInt64(MemoryLayout<T>.stride * buffer.count)
  }

  public mutating func read<T>(into pointer: UnsafeMutablePointer<T>) throws {
    try source.fetch(from: pos, into: pointer)
    pos += UInt64(MemoryLayout<T>.stride)
  }

  public mutating func read<T>(as type: T.Type) throws -> T {
    let stride = MemoryLayout<T>.stride
    let result = try source.fetch(from: pos, as: type)
    pos += UInt64(stride)
    return result
  }

  public mutating func read<T>(count: Int, as type: T.Type) throws -> [T] {
    let stride = MemoryLayout<T>.stride
    let result = try source.fetch(from: pos, count: count, as: type)
    pos += UInt64(stride * count)
    return result
  }

  public mutating func readString() throws -> String? {
    var bytes: [UInt8] = []
    while true {
      let ch = try read(as: UInt8.self)
      if ch == 0 {
        break
      }
      bytes.append(ch)
    }

    return String(decoding: bytes, as: UTF8.self)
  }

}

extension ImageSource {
  /// Fetch all the data from this image source (which must be bounded)
  func fetchAllBytes() -> [UInt8]? {
    guard let bounds = self.bounds else {
      return nil
    }
    if let data = try? fetch(from: bounds.base,
                             count: Int(bounds.size),
                             as: UInt8.self) {
      return data
    }
    return nil
  }
}

enum SubImageSourceError: Error {
  case outOfRangeFetch(UInt64, Int)
}

struct SubImageSource<S: ImageSource>: ImageSource {
  var parent: S
  var baseAddress: Address
  var length: Size
  var path: String? { return parent.path }

  var bounds: Bounds? {
    return Bounds(base: 0, size: length)
  }

  public init(parent: S, baseAddress: Address, length: Size) {
    self.parent = parent
    self.baseAddress = baseAddress
    self.length = length
  }

  public var isMappedImage: Bool {
    return parent.isMappedImage
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let toFetch = buffer.count * MemoryLayout<T>.stride
    if addr < 0 || addr > length {
      throw SubImageSourceError.outOfRangeFetch(UInt64(addr), toFetch)
    }
    if Address(length) - addr < toFetch {
      throw SubImageSourceError.outOfRangeFetch(UInt64(addr), toFetch)
    }

    return try parent.fetch(from: baseAddress + addr, into: buffer)
  }
}

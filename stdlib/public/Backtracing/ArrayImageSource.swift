//===--- ArrayImageSource.swift - An image source backed by an Array -------===//
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
// Defines ArrayImageSource, an image source that is backed by a Swift Array.
//
//===----------------------------------------------------------------------===//

import Swift

@_implementationOnly import OS.Libc

enum ArrayImageSourceError: Error {
  case outOfBoundsRead(UInt64, UInt64)
}

struct ArrayImageSource<T>: ImageSource {
  private var array: Array<T>

  public init(array: Array<T>) {
    self.array = array
  }

  public var isMappedImage: Bool { return false }
  public var path: String? { return nil }
  public var bounds: Bounds? {
    return Bounds(base: 0, size: Size(array.count * MemoryLayout<T>.stride))
  }

  public func fetch<U>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<U>) throws {
    try array.withUnsafeBytes{
      let size = Size($0.count)
      let requested = Size(buffer.count * MemoryLayout<U>.stride)
      if addr > size || requested > size - addr {
        throw ArrayImageSourceError.outOfBoundsRead(addr, requested)
      }

      memcpy(buffer.baseAddress!, $0.baseAddress! + Int(addr), Int(requested))
    }
  }
}

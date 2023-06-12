//===--- Image.swift - Binary image protocol for Swift --------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines a protocol for binary image files that allows us to fetch what
// we need without knowing all of the gory details.
//
//===----------------------------------------------------------------------===//

import Swift

struct ImageSymbol {
  var name: String
  var offset: Int
}

internal protocol Image {
  associatedtype Source: ImageSource

  typealias UUID = [UInt8]
  typealias Address = Source.Address

  init(source: Source, baseAddress: Address, endAddress: Address) throws

  var baseAddress: Address { get set }
  var endAddress: Address { get set }

  var source: Source { get }
  var uuid: UUID? { get }
  var shouldByteSwap: Bool { get }

  func swapIfRequired<T: FixedWidthInteger>(_ x: T) -> T
  func swapIfRequired<T: ByteSwappable>(_ x: T) -> T
  func swapIfRequired<T>(_ x: T) -> T

  func swapIfRequired<T: FixedWidthInteger>(array: inout [T])
  func swapIfRequired<T: ByteSwappable>(array: inout [T])
  func swapIfRequired<T>(array: inout [T])

  func swapIfRequired<T: FixedWidthInteger>(buffer: UnsafeMutableBufferPointer<T>)
  func swapIfRequired<T: ByteSwappable>(buffer: UnsafeMutableBufferPointer<T>)
  func swapIfRequired<T>(buffer: UnsafeMutableBufferPointer<T>)

  func swapIfRequired<T: FixedWidthInteger>(pointer: UnsafeMutablePointer<T>)
  func swapIfRequired<T: ByteSwappable>(pointer: UnsafeMutablePointer<T>)
  func swapIfRequired<T>(pointer: UnsafeMutablePointer<T>)

  func fetch<T>(from addr: Address,
                into buffer: UnsafeMutableBufferPointer<T>) throws
  func fetch<T>(from addr: Address, into pointer: UnsafeMutablePointer<T>) throws
  func fetch<T>(from addr: Address, count: Int, as: T.Type) throws -> [T]
  func fetch<T>(from addr: Address, as type: T.Type) throws -> T

  func fetchUnswapped<T>(from addr: Address,
                         into buffer: UnsafeMutableBufferPointer<T>) throws
  func fetchUnswapped<T>(from addr: Address,
                         into pointer: UnsafeMutablePointer<T>) throws
  func fetchUnswapped<T>(from addr: Address, count: Int, as: T.Type) throws -> [T]
  func fetchUnswapped<T>(from addr: Address, as type: T.Type) throws -> T

  func lookupSymbol(address: Address) -> ImageSymbol?
}

extension Image {
  public func swapIfRequired<T: FixedWidthInteger>(_ x: T) -> T {
    if shouldByteSwap {
      return x.byteSwapped
    }
    return x
  }

  public func swapIfRequired<T: ByteSwappable>(_ x: T) -> T {
    if shouldByteSwap {
      return x.byteSwapped
    }
    return x
  }

  public func swapIfRequired<T>(_ x: T) -> T {
    return x
  }

  public func swapIfRequired<T: ByteSwappable>(array: inout [T]) {
    if shouldByteSwap {
      array.swapBytes()
    }
  }
  public func swapIfRequired<T: FixedWidthInteger>(array: inout [T]) {
    if shouldByteSwap {
      array.swapBytes()
    }
  }
  public func swapIfRequired<T>(array: inout [T]) {
    // Nothing to do
  }

  public func swapIfRequired<T: ByteSwappable>(buffer: UnsafeMutableBufferPointer<T>) {
    if shouldByteSwap {
      buffer.swapBytes()
    }
  }
  public func swapIfRequired<T: FixedWidthInteger>(buffer: UnsafeMutableBufferPointer<T>) {
    if shouldByteSwap {
      buffer.swapBytes()
    }
  }
  public func swapIfRequired<T>(buffer: UnsafeMutableBufferPointer<T>) {
    // Nothing to do
  }

  public func swapIfRequired<T: ByteSwappable>(pointer: UnsafeMutablePointer<T>) {
    if shouldByteSwap {
      pointer.pointee = pointer.pointee.byteSwapped
    }
  }
  public func swapIfRequired<T: FixedWidthInteger>(pointer: UnsafeMutablePointer<T>) {
    if shouldByteSwap {
      pointer.pointee = pointer.pointee.byteSwapped
    }
  }
  public func swapIfRequired<T>(pointer: UnsafeMutablePointer<T>) {
    // Nothing to do
  }


  public func fetchUnswapped<T>(from addr: Address,
    into buffer: UnsafeMutableBufferPointer<T>) throws {
    return try source.fetch(from: addr, into: buffer)
  }
  public func fetchUnswapped<T>(from addr: Address,
    into pointer: UnsafeMutablePointer<T>) throws {
    return try source.fetch(from: addr, into: pointer)
  }
  public func fetchUnswapped<T>(from addr: Address, count: Int, as type: T.Type) throws -> [T] {
    return try source.fetch(from: addr, count: count, as: type)
  }
  public func fetchUnswapped<T>(from addr: Address, as type: T.Type) throws -> T {
    return try source.fetch(from: addr, as: type)
  }

  public func fetch<T>(from addr: Address,
                into buffer: UnsafeMutableBufferPointer<T>) throws {
    try fetchUnswapped(from: addr, into: buffer)
    swapIfRequired(buffer: buffer)
  }
  public func fetch<T>(from addr: Address,
                into pointer: UnsafeMutablePointer<T>) throws {
    try fetchUnswapped(from: addr, into: pointer)
    swapIfRequired(pointer: pointer)
  }
  public func fetch<T>(from addr: Address, count: Int, as type: T.Type) throws -> [T]{
    var result = try fetchUnswapped(from: addr, count: count, as: type)
    swapIfRequired(array: &result)
    return result
  }
  public func fetch<T>(from addr: Address, as type: T.Type) throws -> T {
    return swapIfRequired(try fetchUnswapped(from: addr, as: type))
  }
}

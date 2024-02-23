//===--- StorageViewIndex.swift -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

extension StorageView where Element: ~Copyable & ~Escapable {
  @frozen
  public struct Index {
    @usableFromInline let _allocation: UnsafeRawPointer
    @usableFromInline let _rawValue: UnsafeRawPointer

    @inlinable @inline(__always)
    internal init(allocation: UnsafeRawPointer, rawValue: UnsafeRawPointer) {
      (_allocation, _rawValue) = (allocation, rawValue)
    }

    @inlinable @inline(__always)
    internal init(_rawStart: UnsafeRawPointer) {
      self.init(allocation: _rawStart, rawValue: _rawStart)
    }
  }
}

extension StorageView.Index where Element: ~Copyable {

  @inlinable @inline(__always)
  var isAligned: Bool {
    (Int(bitPattern: _rawValue) & (MemoryLayout<Element>.alignment-1)) == 0
  }
}

@available(*, unavailable)
extension StorageView.Index: Sendable {}

extension StorageView.Index: Equatable where Element: ~Copyable & ~Escapable {
  public static func == (lhs: Self, rhs: Self) -> Bool {
    // note: if we don't define this function, then `Strideable` will define it.
    (lhs._allocation == rhs._allocation) && (lhs._rawValue == rhs._rawValue)
  }
}

extension StorageView.Index: Hashable where Element: ~Copyable & ~Escapable{}

extension StorageView.Index: Strideable where Element: ~Copyable {
  public typealias Stride = Int

  @inlinable @inline(__always)
  public func distance(to other: Self) -> Int {
    precondition(_allocation == other._allocation)
    let bytes = _rawValue.distance(to: other._rawValue)
    let (q, r) = bytes.quotientAndRemainder(dividingBy: MemoryLayout<Element>.stride)
    precondition(r == 0)
    return q
  }

  @inlinable @inline(__always)
  public func advanced(by n: Int) -> Self {
    .init(
      allocation: _allocation,
      rawValue: _rawValue.advanced(by: n &* MemoryLayout<Element>.stride)
    )
  }
}

extension StorageView.Index: Comparable where Element: ~Copyable & ~Escapable {
  @inlinable @inline(__always)
  public static func <(lhs: Self, rhs: Self) -> Bool {
    return lhs._rawValue < rhs._rawValue
  }
}

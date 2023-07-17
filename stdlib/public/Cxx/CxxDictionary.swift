//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A C++ type that represents a dictionary.
///
/// C++ standard library types such as `std::map` and `std::unordered_map`
/// conform to this protocol.
public protocol CxxDictionary<Key, Value> {
  associatedtype Key
  associatedtype Value
  associatedtype RawIterator: UnsafeCxxInputIterator
    where RawIterator.Pointee: CxxPair<Key, Value>

  /// Do not implement this function manually in Swift.
  func __findUnsafe(_ key: Key) -> RawIterator

  /// Do not implement this function manually in Swift.
  func __endUnsafe() -> RawIterator
}

extension CxxDictionary {
  @inlinable
  public subscript(key: Key) -> Value? {
    get {
      let iter = __findUnsafe(key)
      guard iter != __endUnsafe() else {
        return nil
      }
      return iter.pointee.second
    }
  }
}

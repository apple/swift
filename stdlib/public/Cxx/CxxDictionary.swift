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
  associatedtype Element: CxxPair<Key, Value>
  associatedtype RawIterator: UnsafeCxxInputIterator
    where RawIterator.Pointee == Element
  associatedtype RawMutableIterator: UnsafeCxxMutableInputIterator
    where RawMutableIterator.Pointee == Element
  associatedtype Size: BinaryInteger
  associatedtype InsertionResult

  /// Do not implement this function manually in Swift.
  func __findUnsafe(_ key: Key) -> RawIterator

  /// Do not implement this function manually in Swift.
  mutating func __findMutatingUnsafe(_ key: Key) -> RawMutableIterator

  /// Do not implement this function manually in Swift.
  @discardableResult
  mutating func __insertUnsafe(_ element: Element) -> InsertionResult

  /// Do not implement this function manually in Swift.
  @discardableResult
  mutating func erase(_ key: Key) -> Size

  /// Do not implement this function manually in Swift.
  func __endUnsafe() -> RawIterator

  /// Do not implement this function manually in Swift.
  mutating func __endMutatingUnsafe() -> RawMutableIterator
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
    set(newValue) {
      guard let newValue = newValue else {
        self.erase(key)
        return
      }
      var iter = self.__findMutatingUnsafe(key)
      if iter != self.__endMutatingUnsafe() {
        // This key already exists in the dictionary.
        iter.pointee.second = newValue
      } else {
        // Create a std::pair<key_type, mapped_type>.
        let keyValuePair = Element(first: key, second: newValue)
        self.__insertUnsafe(keyValuePair)
      }
    }
  }
}

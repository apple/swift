//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

extension String {
  // FIXME(strings): at least temporarily remove it to see where it was applied
  public init(_ substring: Substring) {
    self = String(substring._slice)

  }
}

public struct Substring : RangeReplaceableCollection, BidirectionalCollection {
  public typealias Index = String.Index
  public typealias IndexDistance = String.IndexDistance

  internal var _slice: RangeReplaceableBidirectionalSlice<String>

  public init() {
    _slice = RangeReplaceableBidirectionalSlice()
  }

  public init(_base base: String, _ bounds: Range<Index>) {
    _slice = RangeReplaceableBidirectionalSlice(base: base, bounds: bounds)
  }

  internal init(_base base: String, _ bounds: ClosedRange<Index>) {
    self.init(_base: base, base._makeHalfOpen(bounds))
  }

  public var startIndex: Index { return _slice.startIndex }
  public var endIndex: Index { return _slice.endIndex }

  public func index(after i: Index) -> Index {
    _precondition(i < endIndex, "Cannot increment beyond endIndex")
    _precondition(i >= startIndex, "Cannot increment an invalid index")
    // FIXME(strings): slice types currently lack necessary bound checks
    return _slice.index(after: i)
  }

  public func index(before i: Index) -> Index {
    _precondition(i <= endIndex, "Cannot decrement an invalid index")
    _precondition(i > startIndex, "Cannot decrement beyond startIndex")
    // FIXME(strings): slice types currently lack necessary bound checks
    return _slice.index(before: i)
  }

  public func index(_ i: Index, offsetBy n: IndexDistance) -> Index {
    let result = _slice.index(i, offsetBy: n)
    // FIXME(strings): slice types currently lack necessary bound checks
    _precondition(
      (_slice._startIndex ..< _slice.endIndex).contains(result),
      "Operation results in an invalid index")
    return result
  }

  public func index(
    _ i: Index, offsetBy n: IndexDistance, limitedBy limit: Index
  ) -> Index? {
    let result = _slice.index(i, offsetBy: n, limitedBy: limit)
    // FIXME(strings): slice types currently lack necessary bound checks
    _precondition(result.map {
        (_slice._startIndex ..< _slice.endIndex).contains($0)
      } ?? true,
      "Operation results in an invalid index")
    return result
  }

  public func distance(from start: Index, to end: Index) -> IndexDistance {
    return _slice.distance(from: start, to: end)
  }

  public subscript(i: Index) -> Character {
    return _slice[i]
  }

  public subscript(bounds: Range<Index>) -> Substring {
    let subSlice = _slice[bounds]
    return Substring(_base: _slice._base,
      subSlice.startIndex ..< subSlice.endIndex)
  }

  public mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) where C : Collection, C.Iterator.Element == Iterator.Element {
    // FIXME(strings): slice types currently lack necessary bound checks
    _slice.replaceSubrange(bounds, with: newElements)
  }

  public mutating func replaceSubrange(
    _ bounds: Range<Index>, with newElements: Substring
  ) {
    replaceSubrange(bounds, with: newElements._slice)
  }

  public mutating func replaceSubrange(
    _ bounds: ClosedRange<Index>, with newElements: Substring
  ) {
    replaceSubrange(bounds, with: newElements._slice)
  }

}

extension Substring : Equatable {
  public static func ==(lhs: Substring, rhs: Substring) -> Bool {
    return String(lhs) == String(rhs)
  }

  // These are not Equatable requirements, but sufficiently similar to be in
  // this extension.
  // FIXME(strings): should be gone if/when an implicit conversion from/to
  // String is available.
  // FIXME(ABI):
  public static func ==(lhs: String, rhs: Substring) -> Bool {
    return lhs == String(rhs)
  }

  public static func ==(lhs: Substring, rhs: String) -> Bool {
    return String(lhs) == rhs
  }

  public static func !=(lhs: String, rhs: Substring) -> Bool {
    return lhs != String(rhs)
  }

  public static func !=(lhs: Substring, rhs: String) -> Bool {
    return String(lhs) != rhs
  }
}

extension Substring : Comparable {
  public static func <(lhs: Substring, rhs: Substring) -> Bool {
    return String(lhs) < String(rhs)
  }
}

extension Substring : Hashable {
  public var hashValue : Int {
    return String(self).hashValue
  }
}

extension Substring {
  public typealias UTF8Index = String.ValidUTF8View.Index

  public var utf8: String.ValidUTF8View {
    get {
      return String(self).utf8
    }
  }

  public typealias UTF16Index = String.ValidUTF16View.Index

  public var utf16: String.ValidUTF16View {
    get {
      return String(self).utf16
    }
  }

  // public typealias ${ViewPrefix}Index = String.Valid${ViewPrefix}View.Index

  public var unicodeScalars: String.UnicodeScalarView {
    get {
      return String(self).unicodeScalars
    }
  }

  public var characters: String.Characters {
    get {
      return String(self).characters
    }
  }
}

extension String {
  // Now that String conforms to Collection, we need to disambiguate between:
  // - init<T>(_ value: T) where T : LosslessStringConvertible
  // - init<S>(_ characters: S) where S : Sequence, S.Iterator.Element == Character
  // Cannot simply do init(_: String) as that would itself be ambiguous with
  // init?(_ description: String)
  public init<
    T : LosslessStringConvertible & Sequence
  >(_ other: T)
  where T.Iterator.Element == Character {
    self = String(other.description)
  }
}

extension String {
  @available(swift, obsoleted: 4)
  public subscript(bounds: Range<Index>) -> String {
    return String(characters[bounds])
  }

  @available(swift, obsoleted: 4)
  public subscript(bounds: ClosedRange<Index>) -> String {
    return String(characters[bounds])
  }
}

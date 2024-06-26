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

/// A collection that supports backward as well as forward traversal.
///
/// Bidirectional collections offer traversal backward from any valid index,
/// not including a collection's `startIndex`. Bidirectional collections can
/// therefore offer additional operations, such as a `last` property that
/// provides efficient access to the last element and a `reversed()` method
/// that presents the elements in reverse order. In addition, bidirectional
/// collections have more efficient implementations of some sequence and
/// collection methods, such as `suffix(_:)`.
///
/// Conforming to the BidirectionalCollection Protocol
/// ==================================================
///
/// To add `BidirectionalProtocol` conformance to your custom types, implement
/// the `index(before:)` method in addition to the requirements of the
/// `Collection` protocol.
///
/// Indices that are moved forward and backward in a bidirectional collection
/// move by the same amount in each direction. That is, for any valid index `i`
/// into a bidirectional collection `c`:
///
/// - If `i >= c.startIndex && i < c.endIndex`, then
///   `c.index(before: c.index(after: i)) == i`.
/// - If `i > c.startIndex && i <= c.endIndex`, then
///   `c.index(after: c.index(before: i)) == i`.
///
/// Valid indices are exactly those indices that are reachable from the
/// collection's `startIndex` by repeated applications of `index(after:)`, up
/// to, and including, the `endIndex`.
public protocol BidirectionalCollection<Element>: Collection
where SubSequence: BidirectionalCollection, Indices: BidirectionalCollection {
  // FIXME: Only needed for associated type inference.
  override associatedtype Element
  override associatedtype Index
  override associatedtype SubSequence
  override associatedtype Indices

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  /// - Returns: The index value immediately before `i`.
  func index(before i: Index) -> Index

  /// Replaces the given index with its predecessor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  func formIndex(before i: inout Index)

  /// Returns the position immediately after the given index.
  ///
  /// The successor of an index must be well defined. For an index `i` into a
  /// collection `c`, calling `c.index(after: i)` returns the same index every
  /// time.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  /// - Returns: The index value immediately after `i`.
  override func index(after i: Index) -> Index

  /// Replaces the given index with its successor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  override func formIndex(after i: inout Index)

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  ///
  ///     let s = "Swift"
  ///     let i = s.index(s.startIndex, offsetBy: 4)
  ///     print(s[i])
  ///     // Prints "t"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`. `distance` must not be negative
  ///     unless the collection conforms to the `BidirectionalCollection`
  ///     protocol.
  /// - Returns: An index offset by `distance` from the index `i`. If
  ///   `distance` is positive, this is the same value as the result of
  ///   `distance` calls to `index(after:)`. If `distance` is negative, this
  ///   is the same value as the result of `abs(distance)` calls to
  ///   `index(before:)`.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the absolute
  ///   value of `distance`.
  @_nonoverride func index(_ i: Index, offsetBy distance: Int) -> Index

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  /// The operation doesn't require going beyond the limiting `s.endIndex`
  /// value, so it succeeds.
  ///
  ///     let s = "Swift"
  ///     if let i = s.index(s.startIndex, offsetBy: 4, limitedBy: s.endIndex) {
  ///         print(s[i])
  ///     }
  ///     // Prints "t"
  ///
  /// The next example attempts to retrieve an index six positions from
  /// `s.startIndex` but fails, because that distance is beyond the index
  /// passed as `limit`.
  ///
  ///     let j = s.index(s.startIndex, offsetBy: 6, limitedBy: s.endIndex)
  ///     print(j)
  ///     // Prints "nil"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection, unless the index passed as `limit` prevents offsetting
  /// beyond those bounds.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`. `distance` must not be negative
  ///     unless the collection conforms to the `BidirectionalCollection`
  ///     protocol.
  ///   - limit: A valid index of the collection to use as a limit. If
  ///     `distance > 0`, a limit that is less than `i` has no effect.
  ///     Likewise, if `distance < 0`, a limit that is greater than `i` has no
  ///     effect.
  /// - Returns: An index offset by `distance` from the index `i`, unless that
  ///   index would be beyond `limit` in the direction of movement. In that
  ///   case, the method returns `nil`.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the absolute
  ///   value of `distance`.
  @_nonoverride func index(
    _ i: Index, offsetBy distance: Int, limitedBy limit: Index
  ) -> Index?

  /// Returns the distance between two indices.
  ///
  /// Unless the collection conforms to the `BidirectionalCollection` protocol,
  /// `start` must be less than or equal to `end`.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection. If `end` is equal to
  ///     `start`, the result is zero.
  /// - Returns: The distance between `start` and `end`. The result can be
  ///   negative only if the collection conforms to the
  ///   `BidirectionalCollection` protocol.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the
  ///   resulting distance.
  @_nonoverride func distance(from start: Index, to end: Index) -> Int

  /// The indices that are valid for subscripting the collection, in ascending
  /// order.
  ///
  /// A collection's `indices` property can hold a strong reference to the
  /// collection itself, causing the collection to be non-uniquely referenced.
  /// If you mutate the collection while iterating over its indices, a strong
  /// reference can cause an unexpected copy of the collection. To avoid the
  /// unexpected copy, use the `index(after:)` method starting with
  /// `startIndex` to produce indices instead.
  ///
  ///     var c = MyFancyCollection([10, 20, 30, 40, 50])
  ///     var i = c.startIndex
  ///     while i != c.endIndex {
  ///         c[i] /= 5
  ///         i = c.index(after: i)
  ///     }
  ///     // c == MyFancyCollection([2, 4, 6, 8, 10])
  override var indices: Indices { get }
  
  /// Accesses a contiguous subrange of the collection's elements.
  ///
  /// The accessed slice uses the same indices for the same elements as the
  /// original collection uses. Always use the slice's `startIndex` property
  /// instead of assuming that its indices start at a particular value.
  ///
  /// This example demonstrates getting a slice of an array of strings, finding
  /// the index of one of the strings in the slice, and then using that index
  /// in the original array.
  ///
  ///     let streets = ["Adams", "Bryant", "Channing", "Douglas", "Evarts"]
  ///     let streetsSlice = streets[2 ..< streets.endIndex]
  ///     print(streetsSlice)
  ///     // Prints "["Channing", "Douglas", "Evarts"]"
  ///
  ///     let index = streetsSlice.firstIndex(of: "Evarts")    // 4
  ///     print(streets[index!])
  ///     // Prints "Evarts"
  ///
  /// - Parameter bounds: A range of the collection's indices. The bounds of
  ///   the range must be valid indices of the collection.
  ///
  /// - Complexity: O(1)
  override subscript(bounds: Range<Index>) -> SubSequence { get }

  // FIXME: Only needed for associated type inference.
  @_borrowed
  override subscript(position: Index) -> Element { get }
  override var startIndex: Index { get }
  override var endIndex: Index { get }
}

/// Default implementation for bidirectional collections.
extension BidirectionalCollection {

  @inlinable // protocol-only
  @inline(__always)
  public func formIndex(before i: inout Index) {
    i = index(before: i)
  }

  @inlinable // protocol-only
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    return _index(i, offsetBy: distance)
  }

  @inlinable // protocol-only
  internal func _index(_ i: Index, offsetBy distance: Int) -> Index {
    if distance >= 0 {
      return _advanceForward(i, by: distance)
    }
    var i = i
    for _ in stride(from: 0, to: distance, by: -1) {
      formIndex(before: &i)
    }
    return i
  }

  @inlinable // protocol-only
  public func index(
    _ i: Index, offsetBy distance: Int, limitedBy limit: Index
  ) -> Index? {
    return _index(i, offsetBy: distance, limitedBy: limit)
  }

  @inlinable // protocol-only
  internal func _index(
    _ i: Index, offsetBy distance: Int, limitedBy limit: Index
  ) -> Index? {
    if distance >= 0 {
      return _advanceForward(i, by: distance, limitedBy: limit)
    }
    var i = i
    for _ in stride(from: 0, to: distance, by: -1) {
      guard i > limit else { return nil }
      formIndex(before: &i)
    }
    return i
  }

  @inlinable // protocol-only
  public func distance(from start: Index, to end: Index) -> Int {
    return _distance(from: start, to: end)
  }

  @inlinable // protocol-only
  internal func _distance(from start: Index, to end: Index) -> Int {
    var start = start
    var count = 0

    if start < end {
      while start < end {
        count += 1
        formIndex(after: &start)
      }
    }
    else if start > end {
      while start > end {
        count -= 1
        formIndex(before: &start)
      }
    }

    return count
  }
}

extension BidirectionalCollection where SubSequence == Self {
  /// Removes and returns the last element of the collection.
  ///
  /// You can use `popLast()` to remove the last element of a collection that
  /// might be empty. The `removeLast()` method must be used only on a
  /// nonempty collection.
  ///
  /// - Returns: The last element of the collection if the collection has one
  ///   or more elements; otherwise, `nil`.
  ///
  /// - Complexity: O(1)
  @inlinable // protocol-only
  public mutating func popLast() -> Element? {
    guard !isEmpty else { return nil }
    let element = last!
    self = self[startIndex..<index(before: endIndex)]
    return element
  }

  /// Removes and returns the last element of the collection.
  ///
  /// The collection must not be empty. To remove the last element of a
  /// collection that might be empty, use the `popLast()` method instead.
  ///
  /// - Returns: The last element of the collection.
  ///
  /// - Complexity: O(1)
  @inlinable // protocol-only
  @discardableResult
  public mutating func removeLast() -> Element {
    let element = last!
    self = self[startIndex..<index(before: endIndex)]
    return element
  }

  /// Removes the given number of elements from the end of the collection.
  ///
  /// - Parameter k: The number of elements to remove. `k` must be greater
  ///   than or equal to zero, and must be less than or equal to the number of
  ///   elements in the collection.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the number of
  ///   elements to remove.
  @inlinable // protocol-only
  public mutating func removeLast(_ k: Int) {
    if k == 0 { return }
    _precondition(k >= 0, "Number of elements to remove should be non-negative")
    guard let end = index(endIndex, offsetBy: -k, limitedBy: startIndex)
    else {
      _preconditionFailure(
        "Can't remove more items from a collection than it contains")
    }
    self = self[startIndex..<end]
  }
}

extension BidirectionalCollection {
  /// Returns a subsequence containing all but the specified number of final
  /// elements.
  ///
  /// If the number of elements to drop exceeds the number of elements in the
  /// collection, the result is an empty subsequence.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.dropLast(2))
  ///     // Prints "[1, 2, 3]"
  ///     print(numbers.dropLast(10))
  ///     // Prints "[]"
  ///
  /// - Parameter k: The number of elements to drop off the end of the
  ///   collection. `k` must be greater than or equal to zero.
  /// - Returns: A subsequence that leaves off `k` elements from the end.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the number of
  ///   elements to drop.
  @inlinable // protocol-only
  public __consuming func dropLast(_ k: Int) -> SubSequence {
    _precondition(
      k >= 0, "Can't drop a negative number of elements from a collection")
    let end = index(
      endIndex,
      offsetBy: -k,
      limitedBy: startIndex) ?? startIndex
    return self[startIndex..<end]
  }

  /// Returns a subsequence, up to the given maximum length, containing the
  /// final elements of the collection.
  ///
  /// If the maximum length exceeds the number of elements in the collection,
  /// the result contains the entire collection.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.suffix(2))
  ///     // Prints "[4, 5]"
  ///     print(numbers.suffix(10))
  ///     // Prints "[1, 2, 3, 4, 5]"
  ///
  /// - Parameter maxLength: The maximum number of elements to return.
  ///   `maxLength` must be greater than or equal to zero.
  /// - Returns: A subsequence terminating at the end of the collection with at
  ///   most `maxLength` elements.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is equal to
  ///   `maxLength`.
  @inlinable // protocol-only
  public __consuming func suffix(_ maxLength: Int) -> SubSequence {
    _precondition(
      maxLength >= 0,
      "Can't take a suffix of negative length from a collection")
    let start = index(
      endIndex,
      offsetBy: -maxLength,
      limitedBy: startIndex) ?? startIndex
    return self[start..<endIndex]
  }
}


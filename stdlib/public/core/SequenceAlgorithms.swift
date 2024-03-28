//===--- SequenceAlgorithms.swift -----------------------------*- swift -*-===//
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

//===----------------------------------------------------------------------===//
// enumerated()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns a sequence of pairs (*n*, *x*), where *n* represents a
  /// consecutive integer starting at zero and *x* represents an element of
  /// the sequence.
  ///
  /// This example enumerates the characters of the string "Swift" and prints
  /// each character along with its place in the string.
  ///
  ///     for (n, c) in "Swift".enumerated() {
  ///         print("\(n): '\(c)'")
  ///     }
  ///     // Prints "0: 'S'"
  ///     // Prints "1: 'w'"
  ///     // Prints "2: 'i'"
  ///     // Prints "3: 'f'"
  ///     // Prints "4: 't'"
  ///
  /// When you enumerate a collection, the integer part of each pair is a counter
  /// for the enumeration, but is not necessarily the index of the paired value.
  /// These counters can be used as indices only in instances of zero-based,
  /// integer-indexed collections, such as `Array` and `ContiguousArray`. For
  /// other collections the counters may be out of range or of the wrong type
  /// to use as an index. To iterate over the elements of a collection with its
  /// indices, use the `zip(_:_:)` function.
  ///
  /// This example iterates over the indices and elements of a set, building a
  /// list consisting of indices of names with five or fewer letters.
  ///
  ///     let names: Set = ["Sofia", "Camilla", "Martina", "Mateo", "Nicolás"]
  ///     var shorterIndices: [Set<String>.Index] = []
  ///     for (i, name) in zip(names.indices, names) {
  ///         if name.count <= 5 {
  ///             shorterIndices.append(i)
  ///         }
  ///     }
  ///
  /// Now that the `shorterIndices` array holds the indices of the shorter
  /// names in the `names` set, you can use those indices to access elements in
  /// the set.
  ///
  ///     for i in shorterIndices {
  ///         print(names[i])
  ///     }
  ///     // Prints "Sofia"
  ///     // Prints "Mateo"
  ///
  /// - Returns: A sequence of pairs enumerating the sequence.
  ///
  /// - Complexity: O(1)
  @inlinable // protocol-only
  public func enumerated() -> EnumeratedSequence<Self> {
    return EnumeratedSequence(_base: self)
  }
}

//===----------------------------------------------------------------------===//
// min(), max()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns the minimum element in the sequence, using the given predicate as
  /// the comparison between elements.
  ///
  /// The predicate must be a *strict weak ordering* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areInIncreasingOrder(a, a)` is always `false`. (Irreflexivity)
  /// - If `areInIncreasingOrder(a, b)` and `areInIncreasingOrder(b, c)` are
  ///   both `true`, then `areInIncreasingOrder(a, c)` is also
  ///   `true`. (Transitive comparability)
  /// - Two elements are *incomparable* if neither is ordered before the other
  ///   according to the predicate. If `a` and `b` are incomparable, and `b`
  ///   and `c` are incomparable, then `a` and `c` are also incomparable.
  ///   (Transitive incomparability)
  ///
  /// This example shows how to use the `min(by:)` method on a
  /// dictionary to find the key-value pair with the lowest value.
  ///
  ///     let hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///     let leastHue = hues.min { a, b in a.value < b.value }
  ///     print(leastHue)
  ///     // Prints "Optional((key: "Coral", value: 16))"
  ///
  /// - Parameter areInIncreasingOrder: A predicate that returns `true`
  ///   if its first argument should be ordered before its second
  ///   argument; otherwise, `false`.
  /// - Returns: The sequence's minimum element, according to
  ///   `areInIncreasingOrder`. If the sequence has no elements, returns
  ///   `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable // protocol-only
  @warn_unqualified_access
  public func min(
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows -> Element? {
    var it = makeIterator()
    guard var result = it.next() else { return nil }
    while let e = it.next() {
      if try areInIncreasingOrder(e, result) { result = e }
    }
    return result
  }

  /// Returns the maximum element in the sequence, using the given predicate
  /// as the comparison between elements.
  ///
  /// The predicate must be a *strict weak ordering* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areInIncreasingOrder(a, a)` is always `false`. (Irreflexivity)
  /// - If `areInIncreasingOrder(a, b)` and `areInIncreasingOrder(b, c)` are
  ///   both `true`, then `areInIncreasingOrder(a, c)` is also
  ///   `true`. (Transitive comparability)
  /// - Two elements are *incomparable* if neither is ordered before the other
  ///   according to the predicate. If `a` and `b` are incomparable, and `b`
  ///   and `c` are incomparable, then `a` and `c` are also incomparable.
  ///   (Transitive incomparability)
  ///
  /// This example shows how to use the `max(by:)` method on a
  /// dictionary to find the key-value pair with the highest value.
  ///
  ///     let hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///     let greatestHue = hues.max { a, b in a.value < b.value }
  ///     print(greatestHue)
  ///     // Prints "Optional((key: "Heliotrope", value: 296))"
  ///
  /// - Parameter areInIncreasingOrder:  A predicate that returns `true` if its
  ///   first argument should be ordered before its second argument;
  ///   otherwise, `false`.
  /// - Returns: The sequence's maximum element if the sequence is not empty;
  ///   otherwise, `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable // protocol-only
  @warn_unqualified_access
  public func max(
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows -> Element? {
    var it = makeIterator()
    guard var result = it.next() else { return nil }
    while let e = it.next() {
      if try areInIncreasingOrder(result, e) { result = e }
    }
    return result
  }
}

extension Sequence where Element: Comparable {
  /// Returns the minimum element in the sequence.
  ///
  /// This example finds the smallest value in an array of height measurements.
  ///
  ///     let heights = [67.5, 65.7, 64.3, 61.1, 58.5, 60.3, 64.9]
  ///     let lowestHeight = heights.min()
  ///     print(lowestHeight)
  ///     // Prints "Optional(58.5)"
  ///
  /// - Returns: The sequence's minimum element. If the sequence has no
  ///   elements, returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @warn_unqualified_access
  public func min() -> Element? {
    return self.min(by: <)
  }

  /// Returns the maximum element in the sequence.
  ///
  /// This example finds the largest value in an array of height measurements.
  ///
  ///     let heights = [67.5, 65.7, 64.3, 61.1, 58.5, 60.3, 64.9]
  ///     let greatestHeight = heights.max()
  ///     print(greatestHeight)
  ///     // Prints "Optional(67.5)"
  ///
  /// - Returns: The sequence's maximum element. If the sequence has no
  ///   elements, returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @warn_unqualified_access
  public func max() -> Element? {
    return self.max(by: <)
  }
}

//===----------------------------------------------------------------------===//
// starts(with:)
//===----------------------------------------------------------------------===//

extension Sequence  {
  /// Returns a Boolean value indicating whether the initial elements of the
  /// sequence are equivalent to the elements in another sequence, using
  /// the given predicate as the equivalence test.
  ///
  /// The predicate must be an *equivalence relation* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areEquivalent(a, a)` is always `true`. (Reflexivity)
  /// - `areEquivalent(a, b)` implies `areEquivalent(b, a)`. (Symmetry)
  /// - If `areEquivalent(a, b)` and `areEquivalent(b, c)` are both `true`, then
  ///   `areEquivalent(a, c)` is also `true`. (Transitivity)
  ///
  /// - Parameters:
  ///   - possiblePrefix: A sequence to compare to this sequence.
  ///   - areEquivalent: A predicate that returns `true` if its two arguments
  ///     are equivalent; otherwise, `false`.
  /// - Returns: `true` if the initial elements of the sequence are equivalent
  ///   to the elements of `possiblePrefix`; otherwise, `false`. If
  ///   `possiblePrefix` has no elements, the return value is `true`.
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `possiblePrefix`.
  @inlinable
  public func starts<PossiblePrefix: Sequence>(
    with possiblePrefix: PossiblePrefix,
    by areEquivalent: (Element, PossiblePrefix.Element) throws -> Bool
  ) rethrows -> Bool {
    var possiblePrefixIterator = possiblePrefix.makeIterator()
    for e0 in self {
      if let e1 = possiblePrefixIterator.next() {
        if try !areEquivalent(e0, e1) {
          return false
        }
      }
      else {
        return true
      }
    }
    return possiblePrefixIterator.next() == nil
  }
}

extension Sequence where Element: Equatable {
  /// Returns a Boolean value indicating whether the initial elements of the
  /// sequence are the same as the elements in another sequence.
  ///
  /// This example tests whether one countable range begins with the elements
  /// of another countable range.
  ///
  ///     let a = 1...3
  ///     let b = 1...10
  ///
  ///     print(b.starts(with: a))
  ///     // Prints "true"
  ///
  /// Passing a sequence with no elements or an empty collection as
  /// `possiblePrefix` always results in `true`.
  ///
  ///     print(b.starts(with: []))
  ///     // Prints "true"
  ///
  /// - Parameter possiblePrefix: A sequence to compare to this sequence.
  /// - Returns: `true` if the initial elements of the sequence are the same as
  ///   the elements of `possiblePrefix`; otherwise, `false`. If
  ///   `possiblePrefix` has no elements, the return value is `true`.
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `possiblePrefix`.
  @inlinable
  public func starts<PossiblePrefix: Sequence>(
    with possiblePrefix: PossiblePrefix
  ) -> Bool where PossiblePrefix.Element == Element {
    return self.starts(with: possiblePrefix, by: ==)
  }
}

//===----------------------------------------------------------------------===//
// elementsEqual()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns a Boolean value indicating whether this sequence and another
  /// sequence contain equivalent elements in the same order, using the given
  /// predicate as the equivalence test.
  ///
  /// At least one of the sequences must be finite.
  ///
  /// The predicate must be an *equivalence relation* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areEquivalent(a, a)` is always `true`. (Reflexivity)
  /// - `areEquivalent(a, b)` implies `areEquivalent(b, a)`. (Symmetry)
  /// - If `areEquivalent(a, b)` and `areEquivalent(b, c)` are both `true`, then
  ///   `areEquivalent(a, c)` is also `true`. (Transitivity)
  ///
  /// - Parameters:
  ///   - other: A sequence to compare to this sequence.
  ///   - areEquivalent: A predicate that returns `true` if its two arguments
  ///     are equivalent; otherwise, `false`.
  /// - Returns: `true` if this sequence and `other` contain equivalent items,
  ///   using `areEquivalent` as the equivalence test; otherwise, `false.`
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `other`.
  @inlinable
  public func elementsEqual<OtherSequence: Sequence>(
    _ other: OtherSequence,
    by areEquivalent: (Element, OtherSequence.Element) throws -> Bool
  ) rethrows -> Bool {
    var iter1 = self.makeIterator()
    var iter2 = other.makeIterator()
    while true {
      switch (iter1.next(), iter2.next()) {
      case let (e1?, e2?):
        if try !areEquivalent(e1, e2) {
          return false
        }
      case (_?, nil), (nil, _?): return false
      case (nil, nil):           return true
      }
    }
  }
}

extension Sequence where Element: Equatable {
  /// Returns a Boolean value indicating whether this sequence and another
  /// sequence contain the same elements in the same order.
  ///
  /// At least one of the sequences must be finite.
  ///
  /// This example tests whether one countable range shares the same elements
  /// as another countable range and an array.
  ///
  ///     let a = 1...3
  ///     let b = 1...10
  ///
  ///     print(a.elementsEqual(b))
  ///     // Prints "false"
  ///     print(a.elementsEqual([1, 2, 3]))
  ///     // Prints "true"
  ///
  /// - Parameter other: A sequence to compare to this sequence.
  /// - Returns: `true` if this sequence and `other` contain the same elements
  ///   in the same order.
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `other`.
  @inlinable
  public func elementsEqual<OtherSequence: Sequence>(
    _ other: OtherSequence
  ) -> Bool where OtherSequence.Element == Element {
    return self.elementsEqual(other, by: ==)
  }
}

//===----------------------------------------------------------------------===//
// lexicographicallyPrecedes()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns a Boolean value indicating whether the sequence precedes another
  /// sequence in a lexicographical (dictionary) ordering, using the given
  /// predicate to compare elements.
  ///
  /// The predicate must be a *strict weak ordering* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areInIncreasingOrder(a, a)` is always `false`. (Irreflexivity)
  /// - If `areInIncreasingOrder(a, b)` and `areInIncreasingOrder(b, c)` are
  ///   both `true`, then `areInIncreasingOrder(a, c)` is also
  ///   `true`. (Transitive comparability)
  /// - Two elements are *incomparable* if neither is ordered before the other
  ///   according to the predicate. If `a` and `b` are incomparable, and `b`
  ///   and `c` are incomparable, then `a` and `c` are also incomparable.
  ///   (Transitive incomparability)
  ///
  /// - Parameters:
  ///   - other: A sequence to compare to this sequence.
  ///   - areInIncreasingOrder:  A predicate that returns `true` if its first
  ///     argument should be ordered before its second argument; otherwise,
  ///     `false`.
  /// - Returns: `true` if this sequence precedes `other` in a dictionary
  ///   ordering as ordered by `areInIncreasingOrder`; otherwise, `false`.
  ///
  /// - Note: This method implements the mathematical notion of lexicographical
  ///   ordering, which has no connection to Unicode.  If you are sorting
  ///   strings to present to the end user, use `String` APIs that perform
  ///   localized comparison instead.
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `other`.
  @inlinable
  public func lexicographicallyPrecedes<OtherSequence: Sequence>(
    _ other: OtherSequence,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows -> Bool
  where OtherSequence.Element == Element {
    var iter1 = self.makeIterator()
    var iter2 = other.makeIterator()
    while true {
      guard let e1 = iter1.next() else {
        return iter2.next() != nil
      }
      guard let e2 = iter2.next() else {
        return false
      }
      if try areInIncreasingOrder(e1, e2) {
        return true
      }
      if try areInIncreasingOrder(e2, e1) {
        return false
      }
    }
  }
}

extension Sequence where Element: Comparable {
  /// Returns a Boolean value indicating whether the sequence precedes another
  /// sequence in a lexicographical (dictionary) ordering, using the
  /// less-than operator (`<`) to compare elements.
  ///
  /// This example uses the `lexicographicallyPrecedes` method to test which
  /// array of integers comes first in a lexicographical ordering.
  ///
  ///     let a = [1, 2, 2, 2]
  ///     let b = [1, 2, 3, 4]
  ///
  ///     print(a.lexicographicallyPrecedes(b))
  ///     // Prints "true"
  ///     print(b.lexicographicallyPrecedes(b))
  ///     // Prints "false"
  ///
  /// - Parameter other: A sequence to compare to this sequence.
  /// - Returns: `true` if this sequence precedes `other` in a dictionary
  ///   ordering; otherwise, `false`.
  ///
  /// - Note: This method implements the mathematical notion of lexicographical
  ///   ordering, which has no connection to Unicode.  If you are sorting
  ///   strings to present to the end user, use `String` APIs that
  ///   perform localized comparison.
  ///
  /// - Complexity: O(*m*), where *m* is the lesser of the length of the
  ///   sequence and the length of `other`.
  @inlinable
  public func lexicographicallyPrecedes<OtherSequence: Sequence>(
    _ other: OtherSequence
  ) -> Bool where OtherSequence.Element == Element {
    return self.lexicographicallyPrecedes(other, by: <)
  }
}

//===----------------------------------------------------------------------===//
// contains()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns a Boolean value indicating whether the sequence contains an
  /// element that satisfies the given predicate.
  ///
  /// You can use the predicate to check for an element of a type that
  /// doesn't conform to the `Equatable` protocol, such as the
  /// `HTTPResponse` enumeration in this example.
  ///
  ///     enum HTTPResponse {
  ///         case ok
  ///         case error(Int)
  ///     }
  ///
  ///     let lastThreeResponses: [HTTPResponse] = [.ok, .ok, .error(404)]
  ///     let hadError = lastThreeResponses.contains { element in
  ///         if case .error = element {
  ///             return true
  ///         } else {
  ///             return false
  ///         }
  ///     }
  ///     // 'hadError' == true
  ///
  /// Alternatively, a predicate can be satisfied by a range of `Equatable`
  /// elements or a general condition. This example shows how you can check an
  /// array for an expense greater than $100.
  ///
  ///     let expenses = [21.37, 55.21, 9.32, 10.18, 388.77, 11.41]
  ///     let hasBigPurchase = expenses.contains { $0 > 100 }
  ///     // 'hasBigPurchase' == true
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence
  ///   as its argument and returns a Boolean value that indicates whether
  ///   the passed element represents a match.
  /// - Returns: `true` if the sequence contains an element that satisfies
  ///   `predicate`; otherwise, `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func contains(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Bool {
    for e in self {
      if try predicate(e) {
        return true
      }
    }
    return false
  }

  /// Returns a Boolean value indicating whether every element of a sequence
  /// satisfies a given predicate.
  ///
  /// The following code uses this method to test whether all the names in an
  /// array have at least five characters:
  ///
  ///     let names = ["Sofia", "Camilla", "Martina", "Mateo", "Nicolás"]
  ///     let allHaveAtLeastFive = names.allSatisfy({ $0.count >= 5 })
  ///     // allHaveAtLeastFive == true
  ///
  /// If the sequence is empty, this method returns `true`.
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence
  ///   as its argument and returns a Boolean value that indicates whether
  ///   the passed element satisfies a condition.
  /// - Returns: `true` if the sequence contains only elements that satisfy
  ///   `predicate`; otherwise, `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func allSatisfy(
    _ predicate: (Element) throws -> Bool
  ) rethrows -> Bool {
    return try !contains { try !predicate($0) }
  }
}

extension Sequence where Element: Equatable {
  /// Returns a Boolean value indicating whether the sequence contains the
  /// given element.
  ///
  /// This example checks to see whether a favorite actor is in an array
  /// storing a movie's cast.
  ///
  ///     let cast = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     print(cast.contains("Marlon"))
  ///     // Prints "true"
  ///     print(cast.contains("James"))
  ///     // Prints "false"
  ///
  /// - Parameter element: The element to find in the sequence.
  /// - Returns: `true` if the element was found in the sequence; otherwise,
  ///   `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func contains(_ element: Element) -> Bool {
    if let result = _customContainsEquatableElement(element) {
      return result
    } else {
      return self.contains { $0 == element }
    }
  }
}

//===----------------------------------------------------------------------===//
// count(where:)
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns the number of elements in the sequence that satisfy the given
  /// predicate.
  ///
  /// You can use this method to count the number of elements that pass a test.
  /// The following example finds the number of names that are fewer than
  /// five characters long:
  ///
  ///     let names = ["Jacqueline", "Ian", "Amy", "Juan", "Soroush", "Tiffany"]
  ///     let shortNameCount = names.count(where: { $0.count < 5 })
  ///     // shortNameCount == 3
  ///
  /// To find the number of times a specific element appears in the sequence,
  /// use the equal to operator (`==`) in the closure to test for a match.
  ///
  ///     let birds = ["duck", "duck", "duck", "duck", "goose"]
  ///     let duckCount = birds.count(where: { $0 == "duck" })
  ///     // duckCount == 4
  ///
  /// The sequence must be finite.
  ///
  /// - Parameter predicate: A closure that takes each element of the sequence
  ///   as its argument and returns a Boolean value indicating whether
  ///   the element should be included in the count.
  /// - Returns: The number of elements in the sequence that satisfy the given
  ///   predicate.
  @_alwaysEmitIntoClient
  public func count<E>(
    where predicate: (Element) throws(E) -> Bool
  ) throws(E) -> Int {
    var count = 0
    for e in self {
      count += try predicate(e) ? 1 : 0
    }
    return count
  }
}

//===----------------------------------------------------------------------===//
// reduce()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns the result of combining the elements of the sequence using the
  /// given closure.
  ///
  /// Use the `reduce(_:_:)` method to produce a single value from the elements
  /// of an entire sequence. For example, you can use this method on an array
  /// of numbers to find their sum or product.
  ///
  /// The `nextPartialResult` closure is called sequentially with an
  /// accumulating value initialized to `initialResult` and each element of
  /// the sequence. This example shows how to find the sum of an array of
  /// numbers.
  ///
  ///     let numbers = [1, 2, 3, 4]
  ///     let numberSum = numbers.reduce(0, { x, y in
  ///         x + y
  ///     })
  ///     // numberSum == 10
  ///
  /// When `numbers.reduce(_:_:)` is called, the following steps occur:
  ///
  /// 1. The `nextPartialResult` closure is called with `initialResult`---`0`
  ///    in this case---and the first element of `numbers`, returning the sum:
  ///    `1`.
  /// 2. The closure is called again repeatedly with the previous call's return
  ///    value and each element of the sequence.
  /// 3. When the sequence is exhausted, the last value returned from the
  ///    closure is returned to the caller.
  ///
  /// If the sequence has no elements, `nextPartialResult` is never executed
  /// and `initialResult` is the result of the call to `reduce(_:_:)`.
  ///
  /// - Parameters:
  ///   - initialResult: The value to use as the initial accumulating value.
  ///     `initialResult` is passed to `nextPartialResult` the first time the
  ///     closure is executed.
  ///   - nextPartialResult: A closure that combines an accumulating value and
  ///     an element of the sequence into a new accumulating value, to be used
  ///     in the next call of the `nextPartialResult` closure or returned to
  ///     the caller.
  /// - Returns: The final accumulated value. If the sequence has no elements,
  ///   the result is `initialResult`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func reduce<Result>(
    _ initialResult: Result,
    _ nextPartialResult:
      (_ partialResult: Result, Element) throws -> Result
  ) rethrows -> Result {
    var accumulator = initialResult
    for element in self {
      accumulator = try nextPartialResult(accumulator, element)
    }
    return accumulator
  }

  /// Returns the result of combining the elements of the sequence using the
  /// given closure.
  ///
  /// Use the `reduce(into:_:)` method to produce a single value from the
  /// elements of an entire sequence. For example, you can use this method on an
  /// array of integers to filter adjacent equal entries or count frequencies.
  ///
  /// This method is preferred over `reduce(_:_:)` for efficiency when the
  /// result is a copy-on-write type, for example an Array or a Dictionary.
  ///
  /// The `updateAccumulatingResult` closure is called sequentially with a
  /// mutable accumulating value initialized to `initialResult` and each element
  /// of the sequence. This example shows how to build a dictionary of letter
  /// frequencies of a string.
  ///
  ///     let letters = "abracadabra"
  ///     let letterCount = letters.reduce(into: [:]) { counts, letter in
  ///         counts[letter, default: 0] += 1
  ///     }
  ///     // letterCount == ["a": 5, "b": 2, "r": 2, "c": 1, "d": 1]
  ///
  /// When `letters.reduce(into:_:)` is called, the following steps occur:
  ///
  /// 1. The `updateAccumulatingResult` closure is called with the initial
  ///    accumulating value---`[:]` in this case---and the first character of
  ///    `letters`, modifying the accumulating value by setting `1` for the key
  ///    `"a"`.
  /// 2. The closure is called again repeatedly with the updated accumulating
  ///    value and each element of the sequence.
  /// 3. When the sequence is exhausted, the accumulating value is returned to
  ///    the caller.
  ///
  /// If the sequence has no elements, `updateAccumulatingResult` is never
  /// executed and `initialResult` is the result of the call to
  /// `reduce(into:_:)`.
  ///
  /// - Parameters:
  ///   - initialResult: The value to use as the initial accumulating value.
  ///   - updateAccumulatingResult: A closure that updates the accumulating
  ///     value with an element of the sequence.
  /// - Returns: The final accumulated value. If the sequence has no elements,
  ///   the result is `initialResult`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func reduce<Result>(
    into initialResult: __owned Result,
    _ updateAccumulatingResult:
      (_ partialResult: inout Result, Element) throws -> ()
  ) rethrows -> Result {
    var accumulator = initialResult
    for element in self {
      try updateAccumulatingResult(&accumulator, element)
    }
    return accumulator
  }
}

//===----------------------------------------------------------------------===//
// reversed()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns an array containing the elements of this sequence in reverse
  /// order.
  ///
  /// The sequence must be finite.
  ///
  /// - Returns: An array containing the elements of this sequence in
  ///   reverse order.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public __consuming func reversed() -> [Element] {
    // FIXME(performance): optimize to 1 pass?  But Array(self) can be
    // optimized to a memcpy() sometimes.  Those cases are usually collections,
    // though.
    var result = Array(self)
    let count = result.count
    for i in 0..<count/2 {
      result.swapAt(i, count - ((i + 1) as Int))
    }
    return result
  }
}

//===----------------------------------------------------------------------===//
// flatMap()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns an array containing the concatenated results of calling the
  /// given transformation with each element of this sequence.
  ///
  /// Use this method to receive a single-level collection when your
  /// transformation produces a sequence or collection for each element.
  ///
  /// In this example, note the difference in the result of using `map` and
  /// `flatMap` with a transformation that returns an array.
  ///
  ///     let numbers = [1, 2, 3, 4]
  ///
  ///     let mapped = numbers.map { Array(repeating: $0, count: $0) }
  ///     // [[1], [2, 2], [3, 3, 3], [4, 4, 4, 4]]
  ///
  ///     let flatMapped = numbers.flatMap { Array(repeating: $0, count: $0) }
  ///     // [1, 2, 2, 3, 3, 3, 4, 4, 4, 4]
  ///
  /// In fact, `s.flatMap(transform)`  is equivalent to
  /// `Array(s.map(transform).joined())`.
  ///
  /// - Parameter transform: A closure that accepts an element of this
  ///   sequence as its argument and returns a sequence or collection.
  /// - Returns: The resulting flattened array.
  ///
  /// - Complexity: O(*m* + *n*), where *n* is the length of this sequence
  ///   and *m* is the length of the result.
  @inlinable
  public func flatMap<SegmentOfResult: Sequence>(
    _ transform: (Element) throws -> SegmentOfResult
  ) rethrows -> [SegmentOfResult.Element] {
    var result: [SegmentOfResult.Element] = []
    for element in self {
      result.append(contentsOf: try transform(element))
    }
    return result
  }
}

extension Sequence {
  /// Returns an array containing the non-`nil` results of calling the given
  /// transformation with each element of this sequence.
  ///
  /// Use this method to receive an array of non-optional values when your
  /// transformation produces an optional value.
  ///
  /// In this example, note the difference in the result of using `map` and
  /// `compactMap` with a transformation that returns an optional `Int` value.
  ///
  ///     let possibleNumbers = ["1", "2", "three", "///4///", "5"]
  ///
  ///     let mapped: [Int?] = possibleNumbers.map { str in Int(str) }
  ///     // [1, 2, nil, nil, 5]
  ///
  ///     let compactMapped: [Int] = possibleNumbers.compactMap { str in Int(str) }
  ///     // [1, 2, 5]
  ///
  /// - Parameter transform: A closure that accepts an element of this
  ///   sequence as its argument and returns an optional value.
  /// - Returns: An array of the non-`nil` results of calling `transform`
  ///   with each element of the sequence.
  ///
  /// - Complexity: O(*n*), where *n* is the length of this sequence.
  @inlinable // protocol-only
  public func compactMap<ElementOfResult>(
    _ transform: (Element) throws -> ElementOfResult?
  ) rethrows -> [ElementOfResult] {
    return try _compactMap(transform)
  }

  // The implementation of compactMap accepting a closure with an optional result.
  // Factored out into a separate function in order to be used in multiple
  // overloads.
  @inlinable // protocol-only
  @inline(__always)
  public func _compactMap<ElementOfResult>(
    _ transform: (Element) throws -> ElementOfResult?
  ) rethrows -> [ElementOfResult] {
    var result: [ElementOfResult] = []
    for element in self {
      if let newElement = try transform(element) {
        result.append(newElement)
      }
    }
    return result
  }
}

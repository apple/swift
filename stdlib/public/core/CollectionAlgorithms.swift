//===--- CollectionAlgorithms.swift ---------------------------------------===//
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

//===----------------------------------------------------------------------===//
// last
//===----------------------------------------------------------------------===//

extension BidirectionalCollection {
  /// The last element of the collection.
  ///
  /// If the collection is empty, the value of this property is `nil`.
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     if let lastNumber = numbers.last {
  ///         print(lastNumber)
  ///     }
  ///     // Prints "50"
  ///
  /// - Complexity: O(1)
  @inlinable
  public var last: Element? {
    return isEmpty ? nil : self[index(before: endIndex)]
  }
}

//===----------------------------------------------------------------------===//
// firstIndex(of:)/firstIndex(where:)
//===----------------------------------------------------------------------===//

extension Collection where Element: Equatable {
  /// Returns the first index where the specified value appears in the
  /// collection.
  ///
  /// After using `firstIndex(of:)` to find the position of a particular element
  /// in a collection, you can use it to access the element by subscripting.
  /// This example shows how you can modify one of the names in an array of
  /// students.
  ///
  ///     var students = ["Ben", "Ivy", "Jordell", "Maxime"]
  ///     if let i = students.firstIndex(of: "Maxime") {
  ///         students[i] = "Max"
  ///     }
  ///     print(students)
  ///     // Prints "["Ben", "Ivy", "Jordell", "Max"]"
  ///
  /// - Parameter element: An element to search for in the collection.
  /// - Returns: The first index where `element` is found. If `element` is not
  ///   found in the collection, returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public func firstIndex(of element: Element) -> Index? {
    if let result = _customIndexOfEquatableElement(element) {
      return result
    }
    return firstIndex(where: { $0 == element })
  }
}

extension Collection {
  /// Returns the first index in which an element of the collection satisfies
  /// the given predicate.
  ///
  /// You can use the predicate to find an element of a type that doesn't
  /// conform to the `Equatable` protocol or to find an element that matches
  /// particular criteria. Here's an example that finds a student name that
  /// begins with the letter "A":
  ///
  ///     let students = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     if let i = students.firstIndex(where: { $0.hasPrefix("A") }) {
  ///         print("\(students[i]) starts with 'A'!")
  ///     }
  ///     // Prints "Abena starts with 'A'!"
  ///
  /// - Parameter predicate: A closure that takes an element as its argument
  ///   and returns a Boolean value that indicates whether the passed element
  ///   represents a match.
  /// - Returns: The index of the first element for which `predicate` returns
  ///   `true`. If no elements in the collection satisfy the given predicate,
  ///   returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public func firstIndex(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Index? {
    var i = self.startIndex
    while i != self.endIndex {
      if try predicate(self[i]) {
        return i
      }
      self.formIndex(after: &i)
    }
    return nil
  }
}

//===----------------------------------------------------------------------===//
// lastIndex(of:)/lastIndex(where:)
//===----------------------------------------------------------------------===//

extension BidirectionalCollection {
  /// Returns the last element of the sequence that satisfies the given
  /// predicate.
  ///
  /// This example uses the `last(where:)` method to find the last
  /// negative number in an array of integers:
  ///
  ///     let numbers = [3, 7, 4, -2, 9, -6, 10, 1]
  ///     if let lastNegative = numbers.last(where: { $0 < 0 }) {
  ///         print("The last negative number is \(lastNegative).")
  ///     }
  ///     // Prints "The last negative number is -6."
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence as
  ///   its argument and returns a Boolean value indicating whether the
  ///   element is a match.
  /// - Returns: The last element of the sequence that satisfies `predicate`,
  ///   or `nil` if there is no element that satisfies `predicate`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public func last(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Element? {
    return try lastIndex(where: predicate).map { self[$0] }
  }

  /// Returns the index of the last element in the collection that matches the
  /// given predicate.
  ///
  /// You can use the predicate to find an element of a type that doesn't
  /// conform to the `Equatable` protocol or to find an element that matches
  /// particular criteria. This example finds the index of the last name that
  /// begins with the letter *A:*
  ///
  ///     let students = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     if let i = students.lastIndex(where: { $0.hasPrefix("A") }) {
  ///         print("\(students[i]) starts with 'A'!")
  ///     }
  ///     // Prints "Akosua starts with 'A'!"
  ///
  /// - Parameter predicate: A closure that takes an element as its argument
  ///   and returns a Boolean value that indicates whether the passed element
  ///   represents a match.
  /// - Returns: The index of the last element in the collection that matches
  ///   `predicate`, or `nil` if no elements match.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public func lastIndex(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Index? {
    var i = endIndex
    while i != startIndex {
      formIndex(before: &i)
      if try predicate(self[i]) {
        return i
      }
    }
    return nil
  }
}

extension BidirectionalCollection where Element: Equatable {
  /// Returns the last index where the specified value appears in the
  /// collection.
  ///
  /// After using `lastIndex(of:)` to find the position of the last instance of
  /// a particular element in a collection, you can use it to access the
  /// element by subscripting. This example shows how you can modify one of
  /// the names in an array of students.
  ///
  ///     var students = ["Ben", "Ivy", "Jordell", "Ben", "Maxime"]
  ///     if let i = students.lastIndex(of: "Ben") {
  ///         students[i] = "Benjamin"
  ///     }
  ///     print(students)
  ///     // Prints "["Ben", "Ivy", "Jordell", "Benjamin", "Max"]"
  ///
  /// - Parameter element: An element to search for in the collection.
  /// - Returns: The last index where `element` is found. If `element` is not
  ///   found in the collection, this method returns `nil`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public func lastIndex(of element: Element) -> Index? {
    if let result = _customLastIndexOfEquatableElement(element) {
      return result
    }
    return lastIndex(where: { $0 == element })
  }
}

//===----------------------------------------------------------------------===//
// indices(where:) / indices(of:)
//===----------------------------------------------------------------------===//

#if !$Embedded
extension Collection {
  /// Returns the indices of all the elements that match the given predicate.
  ///
  /// For example, you can use this method to find all the places that a
  /// vowel occurs in a string.
  ///
  ///     let str = "Fresh cheese in a breeze"
  ///     let vowels: Set<Character> = ["a", "e", "i", "o", "u"]
  ///     let allTheVowels = str.indices(where: { vowels.contains($0) })
  ///     // str[allTheVowels].count == 9
  ///
  /// - Parameter predicate: A closure that takes an element as its argument
  ///   and returns a Boolean value that indicates whether the passed element
  ///   represents a match.
  /// - Returns: A set of the indices of the elements for which `predicate`
  ///   returns `true`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public func indices(
    where predicate: (Element) throws -> Bool
  ) rethrows -> RangeSet<Index> {
    var result: [Range<Index>] = []
    var end = startIndex
    while let begin = try self[end...].firstIndex(where: predicate) {
      end = try self[begin...].prefix(while: predicate).endIndex
      result.append(begin ..< end)

      guard end < self.endIndex else {
        break
      }
      self.formIndex(after: &end)
    }

    return RangeSet(_orderedRanges: result)
  }
}

extension Collection where Element: Equatable {
  /// Returns the indices of all the elements that are equal to the given
  /// element.
  ///
  /// For example, you can use this method to find all the places that a
  /// particular letter occurs in a string.
  ///
  ///     let str = "Fresh cheese in a breeze"
  ///     let allTheEs = str.indices(of: "e")
  ///     // str[allTheEs].count == 7
  ///
  /// - Parameter element: An element to look for in the collection.
  /// - Returns: A set of the indices of the elements that are equal to
  ///   `element`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public func indices(of element: Element) -> RangeSet<Index> {
    indices(where: { $0 == element })
  }
}
#endif

//===----------------------------------------------------------------------===//
// partition(by:)
//===----------------------------------------------------------------------===//

extension MutableCollection {
  /// Reorders the elements of the collection such that all the elements
  /// that match the given predicate are after all the elements that don't
  /// match.
  ///
  /// After partitioning a collection, there is a pivot index `p` where
  /// no element before `p` satisfies the `belongsInSecondPartition`
  /// predicate and every element at or after `p` satisfies
  /// `belongsInSecondPartition`. This operation isn't guaranteed to be
  /// stable, so the relative ordering of elements within the partitions might
  /// change.
  ///
  /// In the following example, an array of numbers is partitioned by a
  /// predicate that matches elements greater than 30.
  ///
  ///     var numbers = [30, 40, 20, 30, 30, 60, 10]
  ///     let p = numbers.partition(by: { $0 > 30 })
  ///     // p == 5
  ///     // numbers == [30, 10, 20, 30, 30, 60, 40]
  ///
  /// The `numbers` array is now arranged in two partitions. The first
  /// partition, `numbers[..<p]`, is made up of the elements that
  /// are not greater than 30. The second partition, `numbers[p...]`,
  /// is made up of the elements that *are* greater than 30.
  ///
  ///     let first = numbers[..<p]
  ///     // first == [30, 10, 20, 30, 30]
  ///     let second = numbers[p...]
  ///     // second == [60, 40]
  ///
  /// Note that the order of elements in both partitions changed.
  /// That is, `40` appears before `60` in the original collection,
  /// but, after calling `partition(by:)`, `60` appears before `40`.
  ///
  /// - Parameter belongsInSecondPartition: A predicate used to partition
  ///   the collection. All elements satisfying this predicate are ordered
  ///   after all elements not satisfying it.
  /// - Returns: The index of the first element in the reordered collection
  ///   that matches `belongsInSecondPartition`. If no elements in the
  ///   collection match `belongsInSecondPartition`, the returned index is
  ///   equal to the collection's `endIndex`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func partition(
    by belongsInSecondPartition: (Element) throws -> Bool
  ) rethrows -> Index {
    return try _halfStablePartition(isSuffixElement: belongsInSecondPartition)
  }

  /// Moves all elements satisfying `isSuffixElement` into a suffix of the
  /// collection, returning the start position of the resulting suffix.
  ///
  /// - Complexity: O(*n*) where n is the length of the collection.
  @inlinable
  internal mutating func _halfStablePartition(
    isSuffixElement: (Element) throws -> Bool
  ) rethrows -> Index {
    guard var i = try firstIndex(where: isSuffixElement)
    else { return endIndex }
    
    var j = index(after: i)
    while j != endIndex {
      if try !isSuffixElement(self[j]) { swapAt(i, j); formIndex(after: &i) }
      formIndex(after: &j)
    }
    return i
  }  
}

extension MutableCollection where Self: BidirectionalCollection {
  /// Reorders the elements of the collection such that all the elements
  /// that match the given predicate are after all the elements that don't
  /// match.
  ///
  /// After partitioning a collection, there is a pivot index `p` where
  /// no element before `p` satisfies the `belongsInSecondPartition`
  /// predicate and every element at or after `p` satisfies
  /// `belongsInSecondPartition`. This operation isn't guaranteed to be
  /// stable, so the relative ordering of elements within the partitions might
  /// change.
  ///
  /// In the following example, an array of numbers is partitioned by a
  /// predicate that matches elements greater than 30.
  ///
  ///     var numbers = [30, 40, 20, 30, 30, 60, 10]
  ///     let p = numbers.partition(by: { $0 > 30 })
  ///     // p == 5
  ///     // numbers == [30, 10, 20, 30, 30, 60, 40]
  ///
  /// The `numbers` array is now arranged in two partitions. The first
  /// partition, `numbers[..<p]`, is made up of the elements that
  /// are not greater than 30. The second partition, `numbers[p...]`,
  /// is made up of the elements that *are* greater than 30.
  ///
  ///     let first = numbers[..<p]
  ///     // first == [30, 10, 20, 30, 30]
  ///     let second = numbers[p...]
  ///     // second == [60, 40]
  ///
  /// Note that the order of elements in both partitions changed.
  /// That is, `40` appears before `60` in the original collection,
  /// but, after calling `partition(by:)`, `60` appears before `40`.
  ///
  /// - Parameter belongsInSecondPartition: A predicate used to partition
  ///   the collection. All elements satisfying this predicate are ordered
  ///   after all elements not satisfying it.
  /// - Returns: The index of the first element in the reordered collection
  ///   that matches `belongsInSecondPartition`. If no elements in the
  ///   collection match `belongsInSecondPartition`, the returned index is
  ///   equal to the collection's `endIndex`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func partition(
    by belongsInSecondPartition: (Element) throws -> Bool
  ) rethrows -> Index {
    let maybeOffset = try withContiguousMutableStorageIfAvailable {
      (bufferPointer) -> Int in
      let unsafeBufferPivot = try bufferPointer._partitionImpl(
        by: belongsInSecondPartition)
      return unsafeBufferPivot - bufferPointer.startIndex
    }
    if let offset = maybeOffset {
      return index(startIndex, offsetBy: offset)
    } else {
      return try _partitionImpl(by: belongsInSecondPartition)
    }
  }

  @inlinable
  internal mutating func _partitionImpl(
    by belongsInSecondPartition: (Element) throws -> Bool
  ) rethrows -> Index {
    var lo = startIndex
    var hi = endIndex
    while true {
      // Invariants at this point:
      //
      // * `lo <= hi`
      // * all elements in `startIndex ..< lo` belong in the first partition
      // * all elements in `hi ..< endIndex` belong in the second partition

      // Find next element from `lo` that may not be in the right place.
      while true {
        guard lo < hi else { return lo }
        if try belongsInSecondPartition(self[lo]) { break }
        formIndex(after: &lo)
      }

      // Find next element down from `hi` that we can swap `lo` with.
      while true {
        formIndex(before: &hi)
        guard lo < hi else { return lo }
        if try !belongsInSecondPartition(self[hi]) { break }
      }

      swapAt(lo, hi)
      formIndex(after: &lo)
    }
  }
}

//===----------------------------------------------------------------------===//
// _indexedStablePartition / _partitioningIndex
//===----------------------------------------------------------------------===//

extension MutableCollection {
  /// Moves all elements at the indices satisfying `belongsInSecondPartition`
  /// into a suffix of the collection, preserving their relative order, and
  /// returns the start of the resulting suffix.
  ///
  /// - Complexity: O(*n* log *n*) where *n* is the number of elements.
  /// - Precondition:
  ///   `n == distance(from: range.lowerBound, to: range.upperBound)`
  internal mutating func _indexedStablePartition(
    count n: Int,
    range: Range<Index>,
    by belongsInSecondPartition: (Index) throws-> Bool
  ) rethrows -> Index {
    if n == 0 { return range.lowerBound }
    if n == 1 {
      return try belongsInSecondPartition(range.lowerBound)
        ? range.lowerBound
        : range.upperBound
    }
    let h = n / 2, i = index(range.lowerBound, offsetBy: h)
    let j = try _indexedStablePartition(
      count: h,
      range: range.lowerBound..<i,
      by: belongsInSecondPartition)
    let k = try _indexedStablePartition(
      count: n - h,
      range: i..<range.upperBound,
      by: belongsInSecondPartition)
    return _rotate(in: j..<k, shiftingToStart: i)
  }
}

//===----------------------------------------------------------------------===//
// _partitioningIndex(where:)
//===----------------------------------------------------------------------===//

extension Collection {
  /// Returns the index of the first element in the collection that matches
  /// the predicate.
  ///
  /// The collection must already be partitioned according to the predicate.
  /// That is, there should be an index `i` where for every element in
  /// `collection[..<i]` the predicate is `false`, and for every element
  /// in `collection[i...]` the predicate is `true`.
  ///
  /// - Parameter predicate: A predicate that partitions the collection.
  /// - Returns: The index of the first element in the collection for which
  ///   `predicate` returns `true`.
  ///
  /// - Complexity: O(log *n*), where *n* is the length of this collection if
  ///   the collection conforms to `RandomAccessCollection`, otherwise O(*n*).
  internal func _partitioningIndex(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Index {
    var n = count
    var l = startIndex
    
    while n > 0 {
      let half = n / 2
      let mid = index(l, offsetBy: half)
      if try predicate(self[mid]) {
        n = half
      } else {
        l = index(after: mid)
        n -= half + 1
      }
    }
    return l
  }
}

//===----------------------------------------------------------------------===//
// shuffled()/shuffle()
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns the elements of the sequence, shuffled using the given generator
  /// as a source for randomness.
  ///
  /// You use this method to randomize the elements of a sequence when you are
  /// using a custom random number generator. For example, you can shuffle the
  /// numbers between `0` and `9` by calling the `shuffled(using:)` method on
  /// that range:
  ///
  ///     let numbers = 0...9
  ///     let shuffledNumbers = numbers.shuffled(using: &myGenerator)
  ///     // shuffledNumbers == [8, 9, 4, 3, 2, 6, 7, 0, 5, 1]
  ///
  /// - Parameter generator: The random number generator to use when shuffling
  ///   the sequence.
  /// - Returns: An array of this sequence's elements in a shuffled order.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  /// - Note: The algorithm used to shuffle a sequence may change in a future
  ///   version of Swift. If you're passing a generator that results in the
  ///   same shuffled order each time you run your program, that sequence may
  ///   change when your program is compiled using a different version of
  ///   Swift.
  @inlinable
  public func shuffled<T: RandomNumberGenerator>(
    using generator: inout T
  ) -> [Element] {
    var result = ContiguousArray(self)
    result.shuffle(using: &generator)
    return Array(result)
  }
  
  /// Returns the elements of the sequence, shuffled.
  ///
  /// For example, you can shuffle the numbers between `0` and `9` by calling
  /// the `shuffled()` method on that range:
  ///
  ///     let numbers = 0...9
  ///     let shuffledNumbers = numbers.shuffled()
  ///     // shuffledNumbers == [1, 7, 6, 2, 8, 9, 4, 3, 5, 0]
  ///
  /// This method is equivalent to calling `shuffled(using:)`, passing in the
  /// system's default random generator.
  ///
  /// - Returns: A shuffled array of this sequence's elements.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func shuffled() -> [Element] {
    var g = SystemRandomNumberGenerator()
    return shuffled(using: &g)
  }
}

extension MutableCollection where Self: RandomAccessCollection {
  /// Shuffles the collection in place, using the given generator as a source
  /// for randomness.
  ///
  /// You use this method to randomize the elements of a collection when you
  /// are using a custom random number generator. For example, you can use the
  /// `shuffle(using:)` method to randomly reorder the elements of an array.
  ///
  ///     var names = ["Alejandro", "Camila", "Diego", "Luciana", "Luis", "Sofía"]
  ///     names.shuffle(using: &myGenerator)
  ///     // names == ["Sofía", "Alejandro", "Camila", "Luis", "Diego", "Luciana"]
  ///
  /// - Parameter generator: The random number generator to use when shuffling
  ///   the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  /// - Note: The algorithm used to shuffle a collection may change in a future
  ///   version of Swift. If you're passing a generator that results in the
  ///   same shuffled order each time you run your program, that sequence may
  ///   change when your program is compiled using a different version of
  ///   Swift.
  @inlinable
  public mutating func shuffle<T: RandomNumberGenerator>(
    using generator: inout T
  ) {
    guard count > 1 else { return }
    var amount = count
    var currentIndex = startIndex
    while amount > 1 {
      let random = Int.random(in: 0 ..< amount, using: &generator)
      amount -= 1
      swapAt(
        currentIndex,
        index(currentIndex, offsetBy: random)
      )
      formIndex(after: &currentIndex)
    }
  }
  
  /// Shuffles the collection in place.
  ///
  /// Use the `shuffle()` method to randomly reorder the elements of an array.
  ///
  ///     var names = ["Alejandro", "Camila", "Diego", "Luciana", "Luis", "Sofía"]
  ///     names.shuffle()
  ///     // names == ["Luis", "Camila", "Luciana", "Sofía", "Alejandro", "Diego"]
  ///
  /// This method is equivalent to calling `shuffle(using:)`, passing in the
  /// system's default random generator.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func shuffle() {
    var g = SystemRandomNumberGenerator()
    shuffle(using: &g)
  }
}

//===--- RangeReplaceableCollection.swift ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// A Collection protocol with replaceSubrange.
//
//===----------------------------------------------------------------------===//

/// A collection that supports replacement of an arbitrary subrange of elements
/// with the elements of another collection.
///
/// Range-replaceable collections provide operations that insert and remove
/// elements. For example, you can add elements to an array of strings by
/// calling any of the inserting or appending operations that the
/// `RangeReplaceableCollection` protocol defines.
///
///     var bugs = ["Aphid", "Damselfly"]
///     bugs.append("Earwig")
///     bugs.insert(contentsOf: ["Bumblebee", "Cicada"], at: 1)
///     print(bugs)
///     // Prints "["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]"
///
/// Likewise, `RangeReplaceableCollection` types can remove one or more
/// elements using a single operation.
///
///     bugs.removeLast()
///     bugs.removeSubrange(1...2)
///     print(bugs)
///     // Prints "["Aphid", "Damselfly"]"
///
///     bugs.removeAll()
///     print(bugs)
///     // Prints "[]"
///
/// Lastly, use the eponymous `replaceSubrange(_:with:)` method to replace
/// a subrange of elements with the contents of another collection. Here,
/// three elements in the middle of an array of integers are replaced by the
/// five elements of a `Repeated<Int>` instance.
///
///      var nums = [10, 20, 30, 40, 50]
///      nums.replaceSubrange(1...3, with: repeatElement(1, count: 5))
///      print(nums)
///      // Prints "[10, 1, 1, 1, 1, 1, 50]"
///
/// Conforming to the RangeReplaceableCollection Protocol
/// =====================================================
///
/// To add `RangeReplaceableCollection` conformance to your custom collection,
/// add an empty initializer and the `replaceSubrange(_:with:)` method to your
/// custom type. `RangeReplaceableCollection` provides default implementations
/// of all its other methods using this initializer and method. For example,
/// the `removeSubrange(_:)` method is implemented by calling
/// `replaceSubrange(_:with:)` with an empty collection for the `newElements`
/// parameter. You can override any of the protocol's required methods to
/// provide your own custom implementation.
public protocol RangeReplaceableCollection<Element>: Collection
where SubSequence: RangeReplaceableCollection {
  // FIXME: Associated type inference requires this.
  override associatedtype SubSequence

  //===--- Fundamental Requirements ---------------------------------------===//

  /// Creates a new, empty collection.
  init()

  /// Replaces the specified subrange of elements with the given collection.
  ///
  /// This method has the effect of removing the specified range of elements
  /// from the collection and inserting the new elements at the same location.
  /// The number of new elements need not match the number of elements being
  /// removed.
  ///
  /// In this example, three elements in the middle of an array of integers are
  /// replaced by the five elements of a `Repeated<Int>` instance.
  ///
  ///      var nums = [10, 20, 30, 40, 50]
  ///      nums.replaceSubrange(1...3, with: repeatElement(1, count: 5))
  ///      print(nums)
  ///      // Prints "[10, 1, 1, 1, 1, 1, 50]"
  ///
  /// If you pass a zero-length range as the `subrange` parameter, this method
  /// inserts the elements of `newElements` at `subrange.startIndex`. Calling
  /// the `insert(contentsOf:at:)` method instead is preferred.
  ///
  /// Likewise, if you pass a zero-length collection as the `newElements`
  /// parameter, this method removes the elements in the given subrange
  /// without replacement. Calling the `removeSubrange(_:)` method instead is
  /// preferred.
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameters:
  ///   - subrange: The subrange of the collection to replace. The bounds of
  ///     the range must be valid indices of the collection.
  ///   - newElements: The new elements to add to the collection.
  ///
  /// - Complexity: O(*n* + *m*), where *n* is length of this collection and
  ///   *m* is the length of `newElements`. If the call to this method simply
  ///   appends the contents of `newElements` to the collection, this method is
  ///   equivalent to `append(contentsOf:)`.
  mutating func replaceSubrange<C>(
    _ subrange: Range<Index>,
    with newElements: __owned C
  ) where C: Collection, C.Element == Element

  /// Prepares the collection to store the specified number of elements, when
  /// doing so is appropriate for the underlying type.
  ///
  /// If you are adding a known number of elements to a collection, use this
  /// method to avoid multiple reallocations. A type that conforms to
  /// `RangeReplaceableCollection` can choose how to respond when this method
  /// is called. Depending on the type, it may make sense to allocate more or
  /// less storage than requested, or to take no action at all.
  ///
  /// - Parameter n: The requested number of elements to store.
  mutating func reserveCapacity(_ n: Int)

  //===--- Derivable Requirements -----------------------------------------===//

  /// Creates a new collection containing the specified number of a single,
  /// repeated value.
  ///
  /// The following example creates an array initialized with five strings
  /// containing the letter *Z*.
  ///
  ///     let fiveZs = Array(repeating: "Z", count: 5)
  ///     print(fiveZs)
  ///     // Prints "["Z", "Z", "Z", "Z", "Z"]"
  ///
  /// - Parameters:
  ///   - repeatedValue: The element to repeat.
  ///   - count: The number of times to repeat the value passed in the
  ///     `repeating` parameter. `count` must be zero or greater.
  init(repeating repeatedValue: Element, count: Int)

  /// Creates a new instance of a collection containing the elements of a
  /// sequence.
  ///
  /// - Parameter elements: The sequence of elements for the new collection.
  ///   `elements` must be finite.
  init<S: Sequence>(_ elements: S)
    where S.Element == Element

  /// Adds an element to the end of the collection.
  ///
  /// If the collection does not have sufficient capacity for another element,
  /// additional storage is allocated before appending `newElement`. The
  /// following example adds a new number to an array of integers:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(100)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 100]"
  ///
  /// - Parameter newElement: The element to append to the collection.
  ///
  /// - Complexity: O(1) on average, over many calls to `append(_:)` on the
  ///   same collection.
  mutating func append(_ newElement: __owned Element)

  /// Adds the elements of a sequence or collection to the end of this
  /// collection.
  ///
  /// The collection being appended to allocates any additional necessary
  /// storage to hold the new elements.
  ///
  /// The following example appends the elements of a `Range<Int>` instance to
  /// an array of integers:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the collection.
  ///
  /// - Complexity: O(*m*), where *m* is the length of `newElements`.
  mutating func append<S: Sequence>(contentsOf newElements: __owned S)
    where S.Element == Element
  // FIXME(ABI)#166 (Evolution): Consider replacing .append(contentsOf) with +=
  // suggestion in SE-91

  /// Inserts a new element into the collection at the specified position.
  ///
  /// The new element is inserted before the element currently at the
  /// specified index. If you pass the collection's `endIndex` property as
  /// the `index` parameter, the new element is appended to the
  /// collection.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(100, at: 3)
  ///     numbers.insert(200, at: numbers.endIndex)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 4, 5, 200]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter newElement: The new element to insert into the collection.
  /// - Parameter i: The position at which to insert the new element.
  ///   `index` must be a valid index into the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection. If
  ///   `i == endIndex`, this method is equivalent to `append(_:)`.
  mutating func insert(_ newElement: __owned Element, at i: Index)

  /// Inserts the elements of a sequence into the collection at the specified
  /// position.
  ///
  /// The new elements are inserted before the element currently at the
  /// specified index. If you pass the collection's `endIndex` property as the
  /// `index` parameter, the new elements are appended to the collection.
  ///
  /// Here's an example of inserting a range of integers into an array of the
  /// same type:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(contentsOf: 100...103, at: 3)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 101, 102, 103, 4, 5]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter newElements: The new elements to insert into the collection.
  /// - Parameter i: The position at which to insert the new elements. `index`
  ///   must be a valid index of the collection.
  ///
  /// - Complexity: O(*n* + *m*), where *n* is length of this collection and
  ///   *m* is the length of `newElements`. If `i == endIndex`, this method
  ///   is equivalent to `append(contentsOf:)`.
  mutating func insert<S: Collection>(contentsOf newElements: __owned S, at i: Index)
    where S.Element == Element

  /// Removes and returns the element at the specified position.
  ///
  /// All the elements following the specified position are moved to close the
  /// gap. This example removes the middle element from an array of
  /// measurements.
  ///
  ///     var measurements = [1.2, 1.5, 2.9, 1.2, 1.6]
  ///     let removed = measurements.remove(at: 2)
  ///     print(measurements)
  ///     // Prints "[1.2, 1.5, 1.2, 1.6]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter i: The position of the element to remove. `index` must be
  ///   a valid index of the collection that is not equal to the collection's
  ///   end index.
  /// - Returns: The removed element.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @discardableResult
  mutating func remove(at i: Index) -> Element

  /// Removes the specified subrange of elements from the collection.
  ///
  ///     var bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
  ///     bugs.removeSubrange(1...3)
  ///     print(bugs)
  ///     // Prints "["Aphid", "Earwig"]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter bounds: The subrange of the collection to remove. The bounds
  ///   of the range must be valid indices of the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  mutating func removeSubrange(_ bounds: Range<Index>)

  /// Customization point for `removeLast()`.  Implement this function if you
  /// want to replace the default implementation.
  ///
  /// The collection must not be empty.
  ///
  /// - Returns: A non-nil value if the operation was performed.
  mutating func _customRemoveLast() -> Element?

  /// Customization point for `removeLast(_:)`.  Implement this function if you
  /// want to replace the default implementation.
  ///
  /// - Parameter n: The number of elements to remove from the collection.
  ///   `n` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  /// - Returns: `true` if the operation was performed.
  mutating func _customRemoveLast(_ n: Int) -> Bool

  /// Removes and returns the first element of the collection.
  ///
  /// The collection must not be empty.
  ///
  ///     var bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
  ///     bugs.removeFirst()
  ///     print(bugs)
  ///     // Prints "["Bumblebee", "Cicada", "Damselfly", "Earwig"]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Returns: The removed element.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @discardableResult
  mutating func removeFirst() -> Element

  /// Removes the specified number of elements from the beginning of the
  /// collection.
  ///
  ///     var bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
  ///     bugs.removeFirst(3)
  ///     print(bugs)
  ///     // Prints "["Damselfly", "Earwig"]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter k: The number of elements to remove from the collection.
  ///   `k` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  mutating func removeFirst(_ k: Int)

  /// Removes all elements from the collection.
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter keepCapacity: Pass `true` to request that the collection
  ///   avoid releasing its storage. Retaining the collection's storage can
  ///   be a useful optimization when you're planning to grow the collection
  ///   again. The default value is `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  mutating func removeAll(keepingCapacity keepCapacity: Bool /*= false*/)

  /// Removes all the elements that satisfy the given predicate.
  ///
  /// Use this method to remove every element in a collection that meets
  /// particular criteria. The order of the remaining elements is preserved.
  /// This example removes all the odd values from an
  /// array of numbers:
  ///
  ///     var numbers = [5, 6, 7, 8, 9, 10, 11]
  ///     numbers.removeAll(where: { $0 % 2 != 0 })
  ///     // numbers == [6, 8, 10]
  ///
  /// - Parameter shouldBeRemoved: A closure that takes an element of the
  ///   sequence as its argument and returns a Boolean value indicating
  ///   whether the element should be removed from the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  mutating func removeAll(
    where shouldBeRemoved: (Element) throws -> Bool) rethrows

  // FIXME: Associated type inference requires these.
  @_borrowed
  override subscript(position: Index) -> Element { get }
  override subscript(bounds: Range<Index>) -> SubSequence { get }
}

//===----------------------------------------------------------------------===//
// Default implementations for RangeReplaceableCollection
//===----------------------------------------------------------------------===//

extension RangeReplaceableCollection {
  /// Creates a new collection containing the specified number of a single,
  /// repeated value.
  ///
  /// Here's an example of creating an array initialized with five strings
  /// containing the letter *Z*.
  ///
  ///     let fiveZs = Array(repeating: "Z", count: 5)
  ///     print(fiveZs)
  ///     // Prints "["Z", "Z", "Z", "Z", "Z"]"
  ///
  /// - Parameters:
  ///   - repeatedValue: The element to repeat.
  ///   - count: The number of times to repeat the value passed in the
  ///     `repeating` parameter. `count` must be zero or greater.
  @inlinable
  public init(repeating repeatedValue: Element, count: Int) {
    self.init()
    if count != 0 {
      let elements = Repeated(_repeating: repeatedValue, count: count)
      append(contentsOf: elements)
    }
  }

  /// Creates a new instance of a collection containing the elements of a
  /// sequence.
  ///
  /// - Parameter elements: The sequence of elements for the new collection.
  @inlinable
  public init<S: Sequence>(_ elements: S)
    where S.Element == Element {
    self.init()
    append(contentsOf: elements)
  }

  /// Adds an element to the end of the collection.
  ///
  /// If the collection does not have sufficient capacity for another element,
  /// additional storage is allocated before appending `newElement`. The
  /// following example adds a new number to an array of integers:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(100)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 100]"
  ///
  /// - Parameter newElement: The element to append to the collection.
  ///
  /// - Complexity: O(1) on average, over many calls to `append(_:)` on the
  ///   same collection.
  @inlinable
  public mutating func append(_ newElement: __owned Element) {
    insert(newElement, at: endIndex)
  }

  /// Adds the elements of a sequence or collection to the end of this
  /// collection.
  ///
  /// The collection being appended to allocates any additional necessary
  /// storage to hold the new elements.
  ///
  /// The following example appends the elements of a `Range<Int>` instance to
  /// an array of integers:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the collection.
  ///
  /// - Complexity: O(*m*), where *m* is the length of `newElements`.
  @inlinable
  public mutating func append<S: Sequence>(contentsOf newElements: __owned S)
    where S.Element == Element {
    
    let done:Void? = newElements.withContiguousStorageIfAvailable {
      replaceSubrange(endIndex..<endIndex, with: $0)
    }
      
    if done == nil {
      for element in newElements {
        append(element)
      }
    }
  }

  /// Inserts a new element into the collection at the specified position.
  ///
  /// The new element is inserted before the element currently at the
  /// specified index. If you pass the collection's `endIndex` property as
  /// the `index` parameter, the new element is appended to the
  /// collection.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(100, at: 3)
  ///     numbers.insert(200, at: numbers.endIndex)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 4, 5, 200]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter newElement: The new element to insert into the collection.
  /// - Parameter i: The position at which to insert the new element.
  ///   `index` must be a valid index into the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection. If
  ///   `i == endIndex`, this method is equivalent to `append(_:)`.
  @inlinable
  public mutating func insert(
    _ newElement: __owned Element, at i: Index
  ) {
    replaceSubrange(i..<i, with: CollectionOfOne(newElement))
  }

  /// Inserts the elements of a sequence into the collection at the specified
  /// position.
  ///
  /// The new elements are inserted before the element currently at the
  /// specified index. If you pass the collection's `endIndex` property as the
  /// `index` parameter, the new elements are appended to the collection.
  ///
  /// Here's an example of inserting a range of integers into an array of the
  /// same type:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(contentsOf: 100...103, at: 3)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 101, 102, 103, 4, 5]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter newElements: The new elements to insert into the collection.
  /// - Parameter i: The position at which to insert the new elements. `index`
  ///   must be a valid index of the collection.
  ///
  /// - Complexity: O(*n* + *m*), where *n* is length of this collection and
  ///   *m* is the length of `newElements`. If `i == endIndex`, this method
  ///   is equivalent to `append(contentsOf:)`.
  @inlinable
  public mutating func insert<C: Collection>(
    contentsOf newElements: __owned C, at i: Index
  ) where C.Element == Element {
    replaceSubrange(i..<i, with: newElements)
  }

  /// Removes and returns the element at the specified position.
  ///
  /// All the elements following the specified position are moved to close the
  /// gap. This example removes the middle element from an array of
  /// measurements.
  ///
  ///     var measurements = [1.2, 1.5, 2.9, 1.2, 1.6]
  ///     let removed = measurements.remove(at: 2)
  ///     print(measurements)
  ///     // Prints "[1.2, 1.5, 1.2, 1.6]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter position: The position of the element to remove. `position`
  ///   must be a valid index of the collection that is not equal to the
  ///   collection's end index.
  /// - Returns: The removed element.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  @discardableResult
  public mutating func remove(at position: Index) -> Element {
    _precondition(!isEmpty, "Can't remove from an empty collection")
    let result: Element = self[position]
    replaceSubrange(position..<index(after: position), with: EmptyCollection())
    return result
  }

  /// Removes the elements in the specified subrange from the collection.
  ///
  /// All the elements following the specified position are moved to close the
  /// gap. This example removes three elements from the middle of an array of
  /// measurements.
  ///
  ///     var measurements = [1.2, 1.5, 2.9, 1.2, 1.5]
  ///     measurements.removeSubrange(1..<4)
  ///     print(measurements)
  ///     // Prints "[1.2, 1.5]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter bounds: The range of the collection to be removed. The
  ///   bounds of the range must be valid indices of the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeSubrange(_ bounds: Range<Index>) {
    replaceSubrange(bounds, with: EmptyCollection())
  }

  /// Removes the specified number of elements from the beginning of the
  /// collection.
  ///
  ///     var bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
  ///     bugs.removeFirst(3)
  ///     print(bugs)
  ///     // Prints "["Damselfly", "Earwig"]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter k: The number of elements to remove from the collection.
  ///   `k` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeFirst(_ k: Int) {
    if k == 0 { return }
    _precondition(k >= 0, "Number of elements to remove should be non-negative")
    guard let end = index(startIndex, offsetBy: k, limitedBy: endIndex) else {
      _preconditionFailure(
        "Can't remove more items from a collection than it has")
    }
    removeSubrange(startIndex..<end)
  }

  /// Removes and returns the first element of the collection.
  ///
  /// The collection must not be empty.
  ///
  ///     var bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
  ///     bugs.removeFirst()
  ///     print(bugs)
  ///     // Prints "["Bumblebee", "Cicada", "Damselfly", "Earwig"]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Returns: The removed element.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  @discardableResult
  public mutating func removeFirst() -> Element {
    _precondition(!isEmpty,
      "Can't remove first element from an empty collection")
    let firstElement = first!
    removeFirst(1)
    return firstElement
  }

  /// Removes all elements from the collection.
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter keepCapacity: Pass `true` to request that the collection
  ///   avoid releasing its storage. Retaining the collection's storage can
  ///   be a useful optimization when you're planning to grow the collection
  ///   again. The default value is `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    if !keepCapacity {
      self = Self()
    }
    else {
      replaceSubrange(startIndex..<endIndex, with: EmptyCollection())
    }
  }

  /// Prepares the collection to store the specified number of elements, when
  /// doing so is appropriate for the underlying type.
  ///
  /// If you will be adding a known number of elements to a collection, use
  /// this method to avoid multiple reallocations. A type that conforms to
  /// `RangeReplaceableCollection` can choose how to respond when this method
  /// is called. Depending on the type, it may make sense to allocate more or
  /// less storage than requested or to take no action at all.
  ///
  /// - Parameter n: The requested number of elements to store.
  @inlinable
  public mutating func reserveCapacity(_ n: Int) {}
}

extension RangeReplaceableCollection where SubSequence == Self {
  /// Removes and returns the first element of the collection.
  ///
  /// The collection must not be empty.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Returns: The first element of the collection.
  ///
  /// - Complexity: O(1)
  @inlinable
  @discardableResult
  public mutating func removeFirst() -> Element {
    _precondition(!isEmpty, "Can't remove items from an empty collection")
    let element = first!
    self = self[index(after: startIndex)..<endIndex]
    return element
  }

  /// Removes the specified number of elements from the beginning of the
  /// collection.
  ///
  /// Attempting to remove more elements than exist in the collection
  /// triggers a runtime error.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Parameter k: The number of elements to remove from the collection.
  ///   `k` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  ///
  /// - Complexity: O(1) if the collection conforms to
  ///   `RandomAccessCollection`; otherwise, O(*k*), where *k* is the specified
  ///   number of elements.
  @inlinable
  public mutating func removeFirst(_ k: Int) {
    if k == 0 { return }
    _precondition(k >= 0, "Number of elements to remove should be non-negative")
    guard let idx = index(startIndex, offsetBy: k, limitedBy: endIndex) else {
      _preconditionFailure(
        "Can't remove more items from a collection than it contains")
    }
    self = self[idx..<endIndex]
  }
}

extension RangeReplaceableCollection {
  /// Replaces the specified subrange of elements with the given collection.
  ///
  /// This method has the effect of removing the specified range of elements
  /// from the collection and inserting the new elements at the same location.
  /// The number of new elements need not match the number of elements being
  /// removed.
  ///
  /// In this example, three elements in the middle of an array of integers are
  /// replaced by the five elements of a `Repeated<Int>` instance.
  ///
  ///      var nums = [10, 20, 30, 40, 50]
  ///      nums.replaceSubrange(1...3, with: repeatElement(1, count: 5))
  ///      print(nums)
  ///      // Prints "[10, 1, 1, 1, 1, 1, 50]"
  ///
  /// If you pass a zero-length range as the `subrange` parameter, this method
  /// inserts the elements of `newElements` at `subrange.startIndex`. Calling
  /// the `insert(contentsOf:at:)` method instead is preferred.
  ///
  /// Likewise, if you pass a zero-length collection as the `newElements`
  /// parameter, this method removes the elements in the given subrange
  /// without replacement. Calling the `removeSubrange(_:)` method instead is
  /// preferred.
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameters:
  ///   - subrange: The subrange of the collection to replace. The bounds of
  ///     the range must be valid indices of the collection.
  ///   - newElements: The new elements to add to the collection.
  ///
  /// - Complexity: O(*n* + *m*), where *n* is length of this collection and
  ///   *m* is the length of `newElements`. If the call to this method simply
  ///   appends the contents of `newElements` to the collection, the complexity
  ///   is O(*m*).
  @inlinable
  public mutating func replaceSubrange<C: Collection, R: RangeExpression>(
    _ subrange: R,
    with newElements: __owned C
  ) where C.Element == Element, R.Bound == Index {
    self.replaceSubrange(subrange.relative(to: self), with: newElements)
  }

  // This unavailable default implementation of
  // `replaceSubrange<C: Collection>(_: Range<Index>, with: C)` prevents
  // incomplete RangeReplaceableCollection implementations from satisfying
  // the protocol through the use of the generic convenience implementation
  // `replaceSubrange<C: Collection, R: RangeExpression>(_: R, with: C)`,
  // If that were the case, at runtime the implementation generic over
  // `RangeExpression` would call itself in an infinite recursion
  // due to the absence of a better option.
  @available(*, unavailable)
  @_alwaysEmitIntoClient
  public mutating func replaceSubrange<C>(
    _ subrange: Range<Index>,
    with newElements: C
  ) where C: Collection, C.Element == Element {
    fatalError()
  }

  /// Removes the elements in the specified subrange from the collection.
  ///
  /// All the elements following the specified position are moved to close the
  /// gap. This example removes three elements from the middle of an array of
  /// measurements.
  ///
  ///     var measurements = [1.2, 1.5, 2.9, 1.2, 1.5]
  ///     measurements.removeSubrange(1..<4)
  ///     print(measurements)
  ///     // Prints "[1.2, 1.5]"
  ///
  /// Calling this method may invalidate any existing indices for use with this
  /// collection.
  ///
  /// - Parameter bounds: The range of the collection to be removed. The
  ///   bounds of the range must be valid indices of the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeSubrange<R: RangeExpression>(
    _ bounds: R
  ) where R.Bound == Index  {
    removeSubrange(bounds.relative(to: self))
  }
}

extension RangeReplaceableCollection {
  @inlinable
  public mutating func _customRemoveLast() -> Element? {
    return nil
  }

  @inlinable
  public mutating func _customRemoveLast(_ n: Int) -> Bool {
    return false
  }
}

extension RangeReplaceableCollection
  where Self: BidirectionalCollection, SubSequence == Self {

  @inlinable
  public mutating func _customRemoveLast() -> Element? {
    let element = last!
    self = self[startIndex..<index(before: endIndex)]
    return element
  }

  @inlinable
  public mutating func _customRemoveLast(_ n: Int) -> Bool {
    guard let end = index(endIndex, offsetBy: -n, limitedBy: startIndex)
    else {
      _preconditionFailure(
        "Can't remove more items from a collection than it contains")
    }
    self = self[startIndex..<end]
    return true
  }
}

extension RangeReplaceableCollection where Self: BidirectionalCollection {
  /// Removes and returns the last element of the collection.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Returns: The last element of the collection if the collection is not
  /// empty; otherwise, `nil`.
  ///
  /// - Complexity: O(1)
  @inlinable
  public mutating func popLast() -> Element? {
    if isEmpty { return nil }
    // duplicate of removeLast logic below, to avoid redundant precondition
    if let result = _customRemoveLast() { return result }
    return remove(at: index(before: endIndex))
  }

  /// Removes and returns the last element of the collection.
  ///
  /// The collection must not be empty.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Returns: The last element of the collection.
  ///
  /// - Complexity: O(1)
  @inlinable
  @discardableResult
  public mutating func removeLast() -> Element {
    _precondition(!isEmpty, "Can't remove last element from an empty collection")
    // NOTE if you change this implementation, change popLast above as well
    // AND change the tie-breaker implementations in the next extension
    if let result = _customRemoveLast() { return result }
    return remove(at: index(before: endIndex))
  }

  /// Removes the specified number of elements from the end of the
  /// collection.
  ///
  /// Attempting to remove more elements than exist in the collection
  /// triggers a runtime error.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Parameter k: The number of elements to remove from the collection.
  ///   `k` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  ///
  /// - Complexity: O(*k*), where *k* is the specified number of elements.
  @inlinable
  public mutating func removeLast(_ k: Int) {
    if k == 0 { return }
    _precondition(k >= 0, "Number of elements to remove should be non-negative")
    if _customRemoveLast(k) {
      return
    }
    let end = endIndex
    guard let start = index(end, offsetBy: -k, limitedBy: startIndex)
    else {
      _preconditionFailure(
        "Can't remove more items from a collection than it contains")
    }

    removeSubrange(start..<end)
  }
}

/// Ambiguity breakers.
extension RangeReplaceableCollection
where Self: BidirectionalCollection, SubSequence == Self {
  /// Removes and returns the last element of the collection.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Returns: The last element of the collection if the collection is not
  /// empty; otherwise, `nil`.
  ///
  /// - Complexity: O(1)
  @inlinable
  public mutating func popLast() -> Element? {
    if isEmpty { return nil }
    // duplicate of removeLast logic below, to avoid redundant precondition
    if let result = _customRemoveLast() { return result }
    return remove(at: index(before: endIndex))
  }

  /// Removes and returns the last element of the collection.
  ///
  /// The collection must not be empty.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Returns: The last element of the collection.
  ///
  /// - Complexity: O(1)
  @inlinable
  @discardableResult
  public mutating func removeLast() -> Element {
    _precondition(!isEmpty, "Can't remove last element from an empty collection")
    // NOTE if you change this implementation, change popLast above as well
    if let result = _customRemoveLast() { return result }
    return remove(at: index(before: endIndex))
  }

  /// Removes the specified number of elements from the end of the
  /// collection.
  ///
  /// Attempting to remove more elements than exist in the collection
  /// triggers a runtime error.
  ///
  /// Calling this method may invalidate all saved indices of this
  /// collection. Do not rely on a previously stored index value after
  /// altering a collection with any operation that can change its length.
  ///
  /// - Parameter k: The number of elements to remove from the collection.
  ///   `k` must be greater than or equal to zero and must not exceed the
  ///   number of elements in the collection.
  ///
  /// - Complexity: O(*k*), where *k* is the specified number of elements.
  @inlinable
  public mutating func removeLast(_ k: Int) {
    if k == 0 { return }
    _precondition(k >= 0, "Number of elements to remove should be non-negative")
    if _customRemoveLast(k) {
      return
    }
    let end = endIndex
    guard let start = index(end, offsetBy: -k, limitedBy: startIndex)
    else {
      _preconditionFailure(
        "Can't remove more items from a collection than it contains")
    }
    removeSubrange(start..<end)
  }
}

extension RangeReplaceableCollection {
  /// Creates a new collection by concatenating the elements of a collection and
  /// a sequence.
  ///
  /// The two arguments must have the same `Element` type. For example, you can
  /// concatenate the elements of an integer array and a `Range<Int>` instance.
  ///
  ///     let numbers = [1, 2, 3, 4]
  ///     let moreNumbers = numbers + (5...10)
  ///     print(moreNumbers)
  ///     // Prints "[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]"
  ///
  /// The resulting collection has the type of the argument on the left-hand
  /// side. In the example above, `moreNumbers` has the same type as `numbers`,
  /// which is `[Int]`.
  ///
  /// - Parameters:
  ///   - lhs: A range-replaceable collection.
  ///   - rhs: A collection or finite sequence.
  @inlinable
  public static func + <
    Other: Sequence
  >(lhs: Self, rhs: Other) -> Self
  where Element == Other.Element {
    var lhs = lhs
    // FIXME: what if lhs is a reference type?  This will mutate it.
    lhs.append(contentsOf: rhs)
    return lhs
  }

  /// Creates a new collection by concatenating the elements of a sequence and a
  /// collection.
  ///
  /// The two arguments must have the same `Element` type. For example, you can
  /// concatenate the elements of a `Range<Int>` instance and an integer array.
  ///
  ///     let numbers = [7, 8, 9, 10]
  ///     let moreNumbers = (1...6) + numbers
  ///     print(moreNumbers)
  ///     // Prints "[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]"
  ///
  /// The resulting collection has the type of argument on the right-hand side.
  /// In the example above, `moreNumbers` has the same type as `numbers`, which
  /// is `[Int]`.
  ///
  /// - Parameters:
  ///   - lhs: A collection or finite sequence.
  ///   - rhs: A range-replaceable collection.
  @inlinable
  public static func + <
    Other: Sequence
  >(lhs: Other, rhs: Self) -> Self
  where Element == Other.Element {
    var result = Self()
    result.reserveCapacity(rhs.count + lhs.underestimatedCount)
    result.append(contentsOf: lhs)
    result.append(contentsOf: rhs)
    return result
  }

  /// Appends the elements of a sequence to a range-replaceable collection.
  ///
  /// Use this operator to append the elements of a sequence to the end of
  /// range-replaceable collection with same `Element` type. This example
  /// appends the elements of a `Range<Int>` instance to an array of integers.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers += 10...15
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameters:
  ///   - lhs: The array to append to.
  ///   - rhs: A collection or finite sequence.
  ///
  /// - Complexity: O(*m*), where *m* is the length of the right-hand-side
  ///   argument.
  @inlinable
  public static func += <
    Other: Sequence
  >(lhs: inout Self, rhs: Other)
  where Element == Other.Element {
    lhs.append(contentsOf: rhs)
  }

  /// Creates a new collection by concatenating the elements of two collections.
  ///
  /// The two arguments must have the same `Element` type. For example, you can
  /// concatenate the elements of two integer arrays.
  ///
  ///     let lowerNumbers = [1, 2, 3, 4]
  ///     let higherNumbers: ContiguousArray = [5, 6, 7, 8, 9, 10]
  ///     let allNumbers = lowerNumbers + higherNumbers
  ///     print(allNumbers)
  ///     // Prints "[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]"
  ///
  /// The resulting collection has the type of the argument on the left-hand
  /// side. In the example above, `moreNumbers` has the same type as `numbers`,
  /// which is `[Int]`.
  ///
  /// - Parameters:
  ///   - lhs: A range-replaceable collection.
  ///   - rhs: Another range-replaceable collection.
  @inlinable
  public static func + <
    Other: RangeReplaceableCollection
  >(lhs: Self, rhs: Other) -> Self
  where Element == Other.Element {
    var lhs = lhs
    // FIXME: what if lhs is a reference type?  This will mutate it.
    lhs.append(contentsOf: rhs)
    return lhs
  }
}


extension RangeReplaceableCollection {
  /// Returns a new collection of the same type containing, in order, the
  /// elements of the original collection that satisfy the given predicate.
  ///
  /// In this example, `filter(_:)` is used to include only names shorter than
  /// five characters.
  ///
  ///     let cast = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     let shortNames = cast.filter { $0.count < 5 }
  ///     print(shortNames)
  ///     // Prints "["Kim", "Karl"]"
  ///
  /// - Parameter isIncluded: A closure that takes an element of the
  ///   sequence as its argument and returns a Boolean value indicating
  ///   whether the element should be included in the returned collection.
  /// - Returns: A collection of the elements that `isIncluded` allowed.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  @available(swift, introduced: 4.0)
  public __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> Self {
    var result = Self()
    for element in self where try isIncluded(element) {
      result.append(element)
    }
    return result
  }
}

extension RangeReplaceableCollection where Self: MutableCollection {
  /// Removes all the elements that satisfy the given predicate.
  ///
  /// Use this method to remove every element in a collection that meets
  /// particular criteria. The order of the remaining elements is preserved.
  /// This example removes all the odd values from an
  /// array of numbers:
  ///
  ///     var numbers = [5, 6, 7, 8, 9, 10, 11]
  ///     numbers.removeAll(where: { $0 % 2 != 0 })
  ///     // numbers == [6, 8, 10]
  ///
  /// - Parameter shouldBeRemoved: A closure that takes an element of the
  ///   sequence as its argument and returns a Boolean value indicating
  ///   whether the element should be removed from the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeAll(
    where shouldBeRemoved: (Element) throws -> Bool
  ) rethrows {
    let suffixStart = try _halfStablePartition(isSuffixElement: shouldBeRemoved)
    removeSubrange(suffixStart...)
  }
}

extension RangeReplaceableCollection {
  /// Removes all the elements that satisfy the given predicate.
  ///
  /// Use this method to remove every element in a collection that meets
  /// particular criteria. The order of the remaining elements is preserved.
  /// This example removes all the vowels from a string:
  ///
  ///     var phrase = "The rain in Spain stays mainly in the plain."
  ///
  ///     let vowels: Set<Character> = ["a", "e", "i", "o", "u"]
  ///     phrase.removeAll(where: { vowels.contains($0) })
  ///     // phrase == "Th rn n Spn stys mnly n th pln."
  ///
  /// - Parameter shouldBeRemoved: A closure that takes an element of the
  ///   sequence as its argument and returns a Boolean value indicating
  ///   whether the element should be removed from the collection.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the collection.
  @inlinable
  public mutating func removeAll(
    where shouldBeRemoved: (Element) throws -> Bool
  ) rethrows {
    self = try filter { try !shouldBeRemoved($0) }
  }
}

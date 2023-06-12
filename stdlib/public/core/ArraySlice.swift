//===--- ArraySlice.swift -------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  - `ArraySlice<Element>` presents an arbitrary subsequence of some
//    contiguous sequence of `Element`s.
//
//===----------------------------------------------------------------------===//

/// A slice of an `Array`, `ContiguousArray`, or `ArraySlice` instance.
///
/// The `ArraySlice` type makes it fast and efficient for you to perform
/// operations on sections of a larger array. Instead of copying over the
/// elements of a slice to new storage, an `ArraySlice` instance presents a
/// view onto the storage of a larger array. And because `ArraySlice`
/// presents the same interface as `Array`, you can generally perform the
/// same operations on a slice as you could on the original array.
///
/// For more information about using arrays, see `Array` and `ContiguousArray`,
/// with which `ArraySlice` shares most properties and methods.
///
/// Slices Are Views onto Arrays
/// ============================
///
/// For example, suppose you have an array holding the number of absences
/// from each class during a session.
///
///     let absences = [0, 2, 0, 4, 0, 3, 1, 0]
///
/// You want to compare the absences in the first half of the session with
/// those in the second half. To do so, start by creating two slices of the
/// `absences` array.
///
///     let midpoint = absences.count / 2
///
///     let firstHalf = absences[..<midpoint]
///     let secondHalf = absences[midpoint...]
///
/// Neither the `firstHalf` nor `secondHalf` slices allocate any new storage
/// of their own. Instead, each presents a view onto the storage of the
/// `absences` array.
///
/// You can call any method on the slices that you might have called on the
/// `absences` array. To learn which half had more absences, use the
/// `reduce(_:_:)` method to calculate each sum.
///
///     let firstHalfSum = firstHalf.reduce(0, +)
///     let secondHalfSum = secondHalf.reduce(0, +)
///
///     if firstHalfSum > secondHalfSum {
///         print("More absences in the first half.")
///     } else {
///         print("More absences in the second half.")
///     }
///     // Prints "More absences in the first half."
///
/// - Important: Long-term storage of `ArraySlice` instances is discouraged. A
///   slice holds a reference to the entire storage of a larger array, not
///   just to the portion it presents, even after the original array's lifetime
///   ends. Long-term storage of a slice may therefore prolong the lifetime of
///   elements that are no longer otherwise accessible, which can appear to be
///   memory and object leakage.
///
/// Slices Maintain Indices
/// =======================
///
/// Unlike `Array` and `ContiguousArray`, the starting index for an
/// `ArraySlice` instance isn't always zero. Slices maintain the same
/// indices of the larger array for the same elements, so the starting
/// index of a slice depends on how it was created, letting you perform
/// index-based operations on either a full array or a slice.
///
/// Sharing indices between collections and their subsequences is an important
/// part of the design of Swift's collection algorithms. Suppose you are
/// tasked with finding the first two days with absences in the session. To
/// find the indices of the two days in question, follow these steps:
///
/// 1) Call `firstIndex(where:)` to find the index of the first element in the
///    `absences` array that is greater than zero.
/// 2) Create a slice of the `absences` array starting after the index found in
///    step 1.
/// 3) Call `firstIndex(where:)` again, this time on the slice created in step
///    2. Where in some languages you might pass a starting index into an
///    `indexOf` method to find the second day, in Swift you perform the same
///    operation on a slice of the original array.
/// 4) Print the results using the indices found in steps 1 and 3 on the
///    original `absences` array.
///
/// Here's an implementation of those steps:
///
///     if let i = absences.firstIndex(where: { $0 > 0 }) {                 // 1
///         let absencesAfterFirst = absences[(i + 1)...]                   // 2
///         if let j = absencesAfterFirst.firstIndex(where: { $0 > 0 }) {   // 3
///             print("The first day with absences had \(absences[i]).")    // 4
///             print("The second day with absences had \(absences[j]).")
///         }
///     }
///     // Prints "The first day with absences had 2."
///     // Prints "The second day with absences had 4."
///
/// In particular, note that `j`, the index of the second day with absences,
/// was found in a slice of the original array and then used to access a value
/// in the original `absences` array itself.
///
/// - Note: To safely reference the starting and ending indices of a slice,
///   always use the `startIndex` and `endIndex` properties instead of
///   specific values.
@frozen
public struct ArraySlice<Element>: _DestructorSafeContainer {
  @usableFromInline
  internal typealias _Buffer = _SliceBuffer<Element>

  @usableFromInline
  internal var _buffer: _Buffer

  /// Initialization from an existing buffer does not have "array.init"
  /// semantics because the caller may retain an alias to buffer.
  @inlinable
  internal init(_buffer: _Buffer) {
    self._buffer = _buffer
  }

  /// Initialization from an existing buffer does not have "array.init"
  /// semantics because the caller may retain an alias to buffer.
  @inlinable
  internal init(_buffer buffer: _ContiguousArrayBuffer<Element>) {
    self.init(_buffer: _Buffer(_buffer: buffer, shiftedToStartIndex: 0))
  }
}

//===--- private helpers---------------------------------------------------===//
extension ArraySlice {
  /// Returns `true` if the array is native and does not need a deferred
  /// type check.  May be hoisted by the optimizer, which means its
  /// results may be stale by the time they are used if there is an
  /// inout violation in user code.
  @inlinable
  @_semantics("array.props.isNativeTypeChecked")
  public // @testable
  func _hoistableIsNativeTypeChecked() -> Bool {
   return _buffer.arrayPropertyIsNativeTypeChecked
  }

  @inlinable
  @_semantics("array.get_count")
  internal func _getCount() -> Int {
    return _buffer.count
  }

  @inlinable
  @_semantics("array.get_capacity")
  internal func _getCapacity() -> Int {
    return _buffer.capacity
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeMutableAndUnique() {
    if _slowPath(!_buffer.beginCOWMutation()) {
      _buffer = _Buffer(copying: _buffer)
    }
  }
  
  /// Marks the end of a mutation.
  ///
  /// After a call to `_endMutation` the buffer must not be mutated until a call
  /// to `_makeMutableAndUnique`.
  @_alwaysEmitIntoClient
  @_semantics("array.end_mutation")
  internal mutating func _endMutation() {
    _buffer.endCOWMutation()
  }

  /// Check that the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  @inlinable
  @inline(__always)
  internal func _checkSubscript_native(_ index: Int) {
    _buffer._checkValidSubscript(index)
  }

  /// Check that the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  @inlinable
  @_semantics("array.check_subscript")
  public // @testable
  func _checkSubscript(
    _ index: Int, wasNativeTypeChecked: Bool
  ) -> _DependenceToken {
    _buffer._checkValidSubscript(index)
    return _DependenceToken()
  }

  /// Check that the specified `index` is valid, i.e. `0 ≤ index ≤ count`.
  @inlinable
  @_semantics("array.check_index")
  internal func _checkIndex(_ index: Int) {
    _precondition(index <= endIndex, "ArraySlice index is out of range")
    _precondition(index >= startIndex, "ArraySlice index is out of range (before startIndex)")
  }

  @_semantics("array.get_element")
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public // @testable
  func _getElement(
    _ index: Int,
    wasNativeTypeChecked: Bool,
    matchingSubscriptCheck: _DependenceToken
  ) -> Element {
#if false
    return _buffer.getElement(index, wasNativeTypeChecked: wasNativeTypeChecked)
#else
    return _buffer.getElement(index)
#endif
  }

  @inlinable
  @_semantics("array.get_element_address")
  internal func _getElementAddress(_ index: Int) -> UnsafeMutablePointer<Element> {
    return _buffer.subscriptBaseAddress + index
  }
}

extension ArraySlice: _ArrayProtocol {
  /// The total number of elements that the array can contain without
  /// allocating new storage.
  ///
  /// Every array reserves a specific amount of memory to hold its contents.
  /// When you add elements to an array and that array begins to exceed its
  /// reserved capacity, the array allocates a larger region of memory and
  /// copies its elements into the new storage. The new storage is a multiple
  /// of the old storage's size. This exponential growth strategy means that
  /// appending an element happens in constant time, averaging the performance
  /// of many append operations. Append operations that trigger reallocation
  /// have a performance cost, but they occur less and less often as the array
  /// grows larger.
  ///
  /// The following example creates an array of integers from an array literal,
  /// then appends the elements of another collection. Before appending, the
  /// array allocates new storage that is large enough store the resulting
  /// elements.
  ///
  ///     var numbers = [10, 20, 30, 40, 50]
  ///     // numbers.count == 5
  ///     // numbers.capacity == 5
  ///
  ///     numbers.append(contentsOf: stride(from: 60, through: 100, by: 10))
  ///     // numbers.count == 10
  ///     // numbers.capacity == 10
  @inlinable
  public var capacity: Int {
    return _getCapacity()
  }

  /// An object that guarantees the lifetime of this array's elements.
  @inlinable
  public // @testable
  var _owner: AnyObject? {
    return _buffer.owner
  }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, `nil`.
  @inlinable
  public var _baseAddressIfContiguous: UnsafeMutablePointer<Element>? {
    @inline(__always) // FIXME(TODO: JIRA): Hack around test failure
    get { return _buffer.firstElementAddressIfContiguous }
  }

  @inlinable
  internal var _baseAddress: UnsafeMutablePointer<Element> {
    return _buffer.firstElementAddress
  }
}

extension ArraySlice: RandomAccessCollection, MutableCollection {
  /// The index type for arrays, `Int`.
  ///
  /// `ArraySlice` instances are not always indexed from zero. Use `startIndex`
  /// and `endIndex` as the bounds for any element access, instead of `0` and
  /// `count`.
  public typealias Index = Int

  /// The type that represents the indices that are valid for subscripting an
  /// array, in ascending order.
  public typealias Indices = Range<Int>

  /// The type that allows iteration over an array's elements.
  public typealias Iterator = IndexingIterator<ArraySlice>

  /// The position of the first element in a nonempty array.
  ///
  /// `ArraySlice` instances are not always indexed from zero. Use `startIndex`
  /// and `endIndex` as the bounds for any element access, instead of `0` and
  /// `count`.
  ///
  /// If the array is empty, `startIndex` is equal to `endIndex`.
  @inlinable
  public var startIndex: Int {
    return _buffer.startIndex
  }

  /// The array's "past the end" position---that is, the position one greater
  /// than the last valid subscript argument.
  ///
  /// When you need a range that includes the last element of an array, use the
  /// half-open range operator (`..<`) with `endIndex`. The `..<` operator
  /// creates a range that doesn't include the upper bound, so it's always
  /// safe to use with `endIndex`. For example:
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     if let i = numbers.firstIndex(of: 30) {
  ///         print(numbers[i ..< numbers.endIndex])
  ///     }
  ///     // Prints "[30, 40, 50]"
  ///
  /// If the array is empty, `endIndex` is equal to `startIndex`.
  @inlinable
  public var endIndex: Int {
    return _buffer.endIndex
  }

  /// Returns the position immediately after the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  /// - Returns: The index immediately after `i`.
  @inlinable
  public func index(after i: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i + 1
  }

  /// Replaces the given index with its successor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  @inlinable
  public func formIndex(after i: inout Int) {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    i += 1
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  /// - Returns: The index immediately before `i`.
  @inlinable
  public func index(before i: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i - 1
  }

  /// Replaces the given index with its predecessor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  @inlinable
  public func formIndex(before i: inout Int) {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    i -= 1
  }

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The following example obtains an index advanced four positions from an
  /// array's starting index and then prints the element at that position.
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     let i = numbers.index(numbers.startIndex, offsetBy: 4)
  ///     print(numbers[i])
  ///     // Prints "50"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the array.
  ///   - distance: The distance to offset `i`.
  /// - Returns: An index offset by `distance` from the index `i`. If
  ///   `distance` is positive, this is the same value as the result of
  ///   `distance` calls to `index(after:)`. If `distance` is negative, this
  ///   is the same value as the result of `abs(distance)` calls to
  ///   `index(before:)`.
  @inlinable
  public func index(_ i: Int, offsetBy distance: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i + distance
  }

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// The following example obtains an index advanced four positions from an
  /// array's starting index and then prints the element at that position. The
  /// operation doesn't require going beyond the limiting `numbers.endIndex`
  /// value, so it succeeds.
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     if let i = numbers.index(numbers.startIndex,
  ///                              offsetBy: 4,
  ///                              limitedBy: numbers.endIndex) {
  ///         print(numbers[i])
  ///     }
  ///     // Prints "50"
  ///
  /// The next example attempts to retrieve an index ten positions from
  /// `numbers.startIndex`, but fails, because that distance is beyond the
  /// index passed as `limit`.
  ///
  ///     let j = numbers.index(numbers.startIndex,
  ///                           offsetBy: 10,
  ///                           limitedBy: numbers.endIndex)
  ///     print(j)
  ///     // Prints "nil"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection, unless the index passed as `limit` prevents offsetting
  /// beyond those bounds.
  ///
  /// - Parameters:
  ///   - i: A valid index of the array.
  ///   - distance: The distance to offset `i`.
  ///   - limit: A valid index of the collection to use as a limit. If
  ///     `distance > 0`, `limit` has no effect if it is less than `i`.
  ///     Likewise, if `distance < 0`, `limit` has no effect if it is greater
  ///     than `i`.
  /// - Returns: An index offset by `distance` from the index `i`, unless that
  ///   index would be beyond `limit` in the direction of movement. In that
  ///   case, the method returns `nil`.
  ///
  /// - Complexity: O(1)
  @inlinable
  public func index(
    _ i: Int, offsetBy distance: Int, limitedBy limit: Int
  ) -> Int? {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    let l = limit - i
    if distance > 0 ? l >= 0 && l < distance : l <= 0 && distance < l {
      return nil
    }
    return i + distance
  }

  /// Returns the distance between two indices.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection. If `end` is equal to
  ///     `start`, the result is zero.
  /// - Returns: The distance between `start` and `end`.
  @inlinable
  public func distance(from start: Int, to end: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return end - start
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Int, bounds: Range<Int>) {
    // NOTE: This method is a no-op for performance reasons.
  }

  @inlinable
  public func _failEarlyRangeCheck(_ range: Range<Int>, bounds: Range<Int>) {
    // NOTE: This method is a no-op for performance reasons.
  }

  /// Accesses the element at the specified position.
  ///
  /// The following example uses indexed subscripting to update an array's
  /// second element. After assigning the new value (`"Butler"`) at a specific
  /// position, that value is immediately available at that same position.
  ///
  ///     var streets = ["Adams", "Bryant", "Channing", "Douglas", "Evarts"]
  ///     streets[1] = "Butler"
  ///     print(streets[1])
  ///     // Prints "Butler"
  ///
  /// - Parameter index: The position of the element to access. `index` must be
  ///   greater than or equal to `startIndex` and less than `endIndex`.
  ///
  /// - Complexity: Reading an element from an array is O(1). Writing is O(1)
  ///   unless the array's storage is shared with another array or uses a
  ///   bridged `NSArray` instance as its storage, in which case writing is
  ///   O(*n*), where *n* is the length of the array.
  @inlinable
  public subscript(index: Int) -> Element {
    get {
      // This call may be hoisted or eliminated by the optimizer.  If
      // there is an inout violation, this value may be stale so needs to be
      // checked again below.
      let wasNativeTypeChecked = _hoistableIsNativeTypeChecked()

      // Make sure the index is in range and wasNativeTypeChecked is
      // still valid.
      let token = _checkSubscript(
        index, wasNativeTypeChecked: wasNativeTypeChecked)

      return _getElement(
        index, wasNativeTypeChecked: wasNativeTypeChecked,
        matchingSubscriptCheck: token)
    }
    _modify {
      _makeMutableAndUnique() // makes the array native, too
      _checkSubscript_native(index)
      let address = _buffer.subscriptBaseAddress + index
      defer { _endMutation() }
      yield &address.pointee
    }
  }

  /// Accesses a contiguous subrange of the array's elements.
  ///
  /// The returned `ArraySlice` instance uses the same indices for the same
  /// elements as the original array. In particular, that slice, unlike an
  /// array, may have a nonzero `startIndex` and an `endIndex` that is not
  /// equal to `count`. Always use the slice's `startIndex` and `endIndex`
  /// properties instead of assuming that its indices start or end at a
  /// particular value.
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
  ///     let i = streetsSlice.firstIndex(of: "Evarts")    // 4
  ///     print(streets[i!])
  ///     // Prints "Evarts"
  ///
  /// - Parameter bounds: A range of integers. The bounds of the range must be
  ///   valid indices of the array.
  @inlinable
  public subscript(bounds: Range<Int>) -> ArraySlice<Element> {
    get {
      _checkIndex(bounds.lowerBound)
      _checkIndex(bounds.upperBound)
      return ArraySlice(_buffer: _buffer[bounds])
    }
    set(rhs) {
      _checkIndex(bounds.lowerBound)
      _checkIndex(bounds.upperBound)
      // If the replacement buffer has same identity, and the ranges match,
      // then this was a pinned in-place modification, nothing further needed.
      if self[bounds]._buffer.identity != rhs._buffer.identity
      || bounds != rhs.startIndex..<rhs.endIndex {
        self.replaceSubrange(bounds, with: rhs)
      }
    }
  }

  /// The number of elements in the array.
  @inlinable
  public var count: Int {
    return _getCount()
  }
}

extension ArraySlice: ExpressibleByArrayLiteral {
  /// Creates an array from the given array literal.
  ///
  /// Do not call this initializer directly. It is used by the compiler when
  /// you use an array literal. Instead, create a new array by using an array
  /// literal as its value. To do this, enclose a comma-separated list of
  /// values in square brackets.
  ///
  /// Here, an array of strings is created from an array literal holding only
  /// strings:
  ///
  ///     let ingredients: ArraySlice =
  ///           ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///
  /// - Parameter elements: A variadic list of elements of the new array.
  @inlinable
  public init(arrayLiteral elements: Element...) {
    self.init(_buffer: ContiguousArray(elements)._buffer)
  }
}

extension ArraySlice: RangeReplaceableCollection {
  /// Creates a new, empty array.
  ///
  /// This is equivalent to initializing with an empty array literal.
  /// For example:
  ///
  ///     var emptyArray = Array<Int>()
  ///     print(emptyArray.isEmpty)
  ///     // Prints "true"
  ///
  ///     emptyArray = []
  ///     print(emptyArray.isEmpty)
  ///     // Prints "true"
  @inlinable
  @_semantics("array.init.empty")
  public init() {
    _buffer = _Buffer()
  }

  /// Creates an array containing the elements of a sequence.
  ///
  /// You can use this initializer to create an array from any other type that
  /// conforms to the `Sequence` protocol. For example, you might want to
  /// create an array with the integers from 1 through 7. Use this initializer
  /// around a range instead of typing all those numbers in an array literal.
  ///
  ///     let numbers = Array(1...7)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 6, 7]"
  ///
  /// You can also use this initializer to convert a complex sequence or
  /// collection type back to an array. For example, the `keys` property of
  /// a dictionary isn't an array with its own storage, it's a collection
  /// that maps its elements from the dictionary only when they're
  /// accessed, saving the time and space needed to allocate an array. If
  /// you need to pass those keys to a method that takes an array, however,
  /// use this initializer to convert that list from its type of
  /// `LazyMapCollection<Dictionary<String, Int>, Int>` to a simple
  /// `[String]`.
  ///
  ///     func cacheImagesWithNames(names: [String]) {
  ///         // custom image loading and caching
  ///      }
  ///
  ///     let namedHues: [String: Int] = ["Vermillion": 18, "Magenta": 302,
  ///             "Gold": 50, "Cerise": 320]
  ///     let colorNames = Array(namedHues.keys)
  ///     cacheImagesWithNames(colorNames)
  ///
  ///     print(colorNames)
  ///     // Prints "["Gold", "Cerise", "Magenta", "Vermillion"]"
  ///
  /// - Parameter s: The sequence of elements to turn into an array.
  @inlinable
  public init<S: Sequence>(_ s: S)
    where S.Element == Element {

    self.init(_buffer: s._copyToContiguousArray()._buffer)
  }

  /// Creates a new array containing the specified number of a single, repeated
  /// value.
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
  @_semantics("array.init")
  public init(repeating repeatedValue: Element, count: Int) {
    _precondition(count >= 0, "Can't construct ArraySlice with count < 0")
    if count > 0 {
      _buffer = ArraySlice._allocateBufferUninitialized(minimumCapacity: count)
      _buffer.count = count
      var p = _buffer.firstElementAddress
      for _ in 0..<count {
        p.initialize(to: repeatedValue)
        p += 1
      }
    } else {
      _buffer = _Buffer()
    }
    _endMutation()
  }

  @inline(never)
  @usableFromInline
  internal static func _allocateBufferUninitialized(
    minimumCapacity: Int
  ) -> _Buffer {
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: 0, minimumCapacity: minimumCapacity)
    return _Buffer(_buffer: newBuffer, shiftedToStartIndex: 0)
  }

  /// Construct a ArraySlice of `count` uninitialized elements.
  @inlinable
  @_semantics("array.init")
  internal init(_uninitializedCount count: Int) {
    _precondition(count >= 0, "Can't construct ArraySlice with count < 0")
    // Note: Sinking this constructor into an else branch below causes an extra
    // Retain/Release.
    _buffer = _Buffer()
    if count > 0 {
      // Creating a buffer instead of calling reserveCapacity saves doing an
      // unnecessary uniqueness check. We disable inlining here to curb code
      // growth.
      _buffer = ArraySlice._allocateBufferUninitialized(minimumCapacity: count)
      _buffer.count = count
    }
    // Can't store count here because the buffer might be pointing to the
    // shared empty array.
    _endMutation()
  }

  /// Entry point for `Array` literal construction; builds and returns
  /// a ArraySlice of `count` uninitialized elements.
  @inlinable
  @_semantics("array.uninitialized")
  internal static func _allocateUninitialized(
    _ count: Int
  ) -> (ArraySlice, UnsafeMutablePointer<Element>) {
    let result = ArraySlice(_uninitializedCount: count)
    return (result, result._buffer.firstElementAddress)
  }

  //===--- basic mutations ------------------------------------------------===//

  /// Reserves enough space to store the specified number of elements.
  ///
  /// If you are adding a known number of elements to an array, use this method
  /// to avoid multiple reallocations. This method ensures that the array has
  /// unique, mutable, contiguous storage, with space allocated for at least
  /// the requested number of elements.
  ///
  /// Calling the `reserveCapacity(_:)` method on an array with bridged storage
  /// triggers a copy to contiguous storage even if the existing storage
  /// has room to store `minimumCapacity` elements.
  ///
  /// For performance reasons, the size of the newly allocated storage might be
  /// greater than the requested capacity. Use the array's `capacity` property
  /// to determine the size of the new storage.
  ///
  /// Preserving an Array's Geometric Growth Strategy
  /// ===============================================
  ///
  /// If you implement a custom data structure backed by an array that grows
  /// dynamically, naively calling the `reserveCapacity(_:)` method can lead
  /// to worse than expected performance. Arrays need to follow a geometric
  /// allocation pattern for appending elements to achieve amortized
  /// constant-time performance. The `Array` type's `append(_:)` and
  /// `append(contentsOf:)` methods take care of this detail for you, but
  /// `reserveCapacity(_:)` allocates only as much space as you tell it to
  /// (padded to a round value), and no more. This avoids over-allocation, but
  /// can result in insertion not having amortized constant-time performance.
  ///
  /// The following code declares `values`, an array of integers, and the
  /// `addTenQuadratic()` function, which adds ten more values to the `values`
  /// array on each call.
  ///
  ///       var values: [Int] = [0, 1, 2, 3]
  ///
  ///       // Don't use 'reserveCapacity(_:)' like this
  ///       func addTenQuadratic() {
  ///           let newCount = values.count + 10
  ///           values.reserveCapacity(newCount)
  ///           for n in values.count..<newCount {
  ///               values.append(n)
  ///           }
  ///       }
  ///
  /// The call to `reserveCapacity(_:)` increases the `values` array's capacity
  /// by exactly 10 elements on each pass through `addTenQuadratic()`, which
  /// is linear growth. Instead of having constant time when averaged over
  /// many calls, the function may decay to performance that is linear in
  /// `values.count`. This is almost certainly not what you want.
  ///
  /// In cases like this, the simplest fix is often to simply remove the call
  /// to `reserveCapacity(_:)`, and let the `append(_:)` method grow the array
  /// for you.
  ///
  ///       func addTen() {
  ///           let newCount = values.count + 10
  ///           for n in values.count..<newCount {
  ///               values.append(n)
  ///           }
  ///       }
  ///
  /// If you need more control over the capacity of your array, implement your
  /// own geometric growth strategy, passing the size you compute to
  /// `reserveCapacity(_:)`.
  ///
  /// - Parameter minimumCapacity: The requested number of elements to store.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the array.
  @inlinable
  @_semantics("array.mutate_unknown")
  public mutating func reserveCapacity(_ minimumCapacity: Int) {
    if !_buffer.beginCOWMutation() || _buffer.capacity < minimumCapacity {
      let newBuffer = _ContiguousArrayBuffer<Element>(
        _uninitializedCount: count, minimumCapacity: minimumCapacity)

      _buffer._copyContents(
        subRange: _buffer.indices,
        initializing: newBuffer.firstElementAddress)
      _buffer = _Buffer(
        _buffer: newBuffer, shiftedToStartIndex: _buffer.startIndex)
    }
    _internalInvariant(capacity >= minimumCapacity)
    _endMutation()
  }

  /// Copy the contents of the current buffer to a new unique mutable buffer.
  /// The count of the new buffer is set to `oldCount`, the capacity of the
  /// new buffer is big enough to hold 'oldCount' + 1 elements.
  @inline(never)
  @inlinable // @specializable
  internal mutating func _copyToNewBuffer(oldCount: Int) {
    let newCount = oldCount &+ 1
    var newBuffer = _buffer._forceCreateUniqueMutableBuffer(
      countForNewBuffer: oldCount, minNewCapacity: newCount)
    _buffer._arrayOutOfPlaceUpdate(
      &newBuffer, oldCount, 0)
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeUniqueAndReserveCapacityIfNotUnique() {
    if _slowPath(!_buffer.beginCOWMutation()) {
      _copyToNewBuffer(oldCount: _buffer.count)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _reserveCapacityAssumingUniqueBuffer(oldCount: Int) {
    // Due to make_mutable hoisting the situation can arise where we hoist
    // _makeMutableAndUnique out of loop and use it to replace
    // _makeUniqueAndReserveCapacityIfNotUnique that precedes this call. If the
    // array was empty _makeMutableAndUnique does not replace the empty array
    // buffer by a unique buffer (it just replaces it by the empty array
    // singleton).
    // This specific case is okay because we will make the buffer unique in this
    // function because we request a capacity > 0 and therefore _copyToNewBuffer
    // will be called creating a new buffer.
    let capacity = _buffer.capacity
    _internalInvariant(capacity == 0 || _buffer.isMutableAndUniquelyReferenced())

    if _slowPath(oldCount &+ 1 > capacity) {
      _copyToNewBuffer(oldCount: oldCount)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _appendElementAssumeUniqueAndCapacity(
    _ oldCount: Int,
    newElement: __owned Element
  ) {
    _internalInvariant(_buffer.isMutableAndUniquelyReferenced())
    _internalInvariant(_buffer.capacity >= _buffer.count &+ 1)

    _buffer.count = oldCount &+ 1
    (_buffer.firstElementAddress + oldCount).initialize(to: newElement)
  }

  /// Adds a new element at the end of the array.
  ///
  /// Use this method to append a single element to the end of a mutable array.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(100)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 100]"
  ///
  /// Because arrays increase their allocated capacity using an exponential
  /// strategy, appending a single element to an array is an O(1) operation
  /// when averaged over many calls to the `append(_:)` method. When an array
  /// has additional capacity and is not sharing its storage with another
  /// instance, appending an element is O(1). When an array needs to
  /// reallocate storage before appending or its storage is shared with
  /// another copy, appending is O(*n*), where *n* is the length of the array.
  ///
  /// - Parameter newElement: The element to append to the array.
  ///
  /// - Complexity: O(1) on average, over many calls to `append(_:)` on the
  ///   same array.
  @inlinable
  @_semantics("array.append_element")
  public mutating func append(_ newElement: __owned Element) {
    _makeUniqueAndReserveCapacityIfNotUnique()
    let oldCount = _getCount()
    _reserveCapacityAssumingUniqueBuffer(oldCount: oldCount)
    _appendElementAssumeUniqueAndCapacity(oldCount, newElement: newElement)
    _endMutation()
  }

  /// Adds the elements of a sequence to the end of the array.
  ///
  /// Use this method to append the elements of a sequence to the end of this
  /// array. This example appends the elements of a `Range<Int>` instance
  /// to an array of integers.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the array.
  ///
  /// - Complexity: O(*m*) on average, where *m* is the length of
  ///   `newElements`, over many calls to `append(contentsOf:)` on the same
  ///   array.
  @inlinable
  @_semantics("array.append_contentsOf")
  public mutating func append<S: Sequence>(contentsOf newElements: __owned S)
    where S.Element == Element {

    let newElementsCount = newElements.underestimatedCount
    reserveCapacityForAppend(newElementsCount: newElementsCount)
    _ = _buffer.beginCOWMutation()

    let oldCount = self.count
    let startNewElements = _buffer.firstElementAddress + oldCount
    let buf = UnsafeMutableBufferPointer(
                start: startNewElements, 
                count: self.capacity - oldCount)

    let (remainder,writtenUpTo) = buf.initialize(from: newElements)
    
    // trap on underflow from the sequence's underestimate:
    let writtenCount = buf.distance(from: buf.startIndex, to: writtenUpTo)
    _precondition(newElementsCount <= writtenCount,
      "newElements.underestimatedCount was an overestimate")
    // can't check for overflow as sequences can underestimate

    // This check prevents a data race writing to _swiftEmptyArrayStorage
    if writtenCount > 0 {
      _buffer.count += writtenCount
    }

    if writtenUpTo == buf.endIndex {
      // there may be elements that didn't fit in the existing buffer,
      // append them in slow sequence-only mode
      _buffer._arrayAppendSequence(IteratorSequence(remainder))
    }
    _endMutation()
  }

  @inlinable
  @_semantics("array.reserve_capacity_for_append")
  internal mutating func reserveCapacityForAppend(newElementsCount: Int) {
    let oldCount = self.count
    let oldCapacity = self.capacity
    let newCount = oldCount + newElementsCount

    // Ensure uniqueness, mutability, and sufficient storage.  Note that
    // for consistency, we need unique self even if newElements is empty.
    self.reserveCapacity(
      newCount > oldCapacity ?
      Swift.max(newCount, _growArrayCapacity(oldCapacity))
      : newCount)
  }

  @inlinable
  public mutating func _customRemoveLast() -> Element? {
    _precondition(count > 0, "Can't removeLast from an empty ArraySlice")
    // FIXME(performance): if `self` is uniquely referenced, we should remove
    // the element as shown below (this will deallocate the element and
    // decrease memory use).  If `self` is not uniquely referenced, the code
    // below will make a copy of the storage, which is wasteful.  Instead, we
    // should just shrink the view without allocating new storage.
    let i = endIndex
    // We don't check for overflow in `i - 1` because `i` is known to be
    // positive.
    let result = self[i &- 1]
    self.replaceSubrange((i &- 1)..<i, with: EmptyCollection())
    return result
  }
  
  /// Removes and returns the element at the specified position.
  ///
  /// All the elements following the specified position are moved up to
  /// close the gap.
  ///
  ///     var measurements: [Double] = [1.1, 1.5, 2.9, 1.2, 1.5, 1.3, 1.2]
  ///     let removed = measurements.remove(at: 2)
  ///     print(measurements)
  ///     // Prints "[1.1, 1.5, 1.2, 1.5, 1.3, 1.2]"
  ///
  /// - Parameter index: The position of the element to remove. `index` must
  ///   be a valid index of the array.
  /// - Returns: The element at the specified index.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array.
  @inlinable
  @discardableResult
  public mutating func remove(at index: Int) -> Element {
    let result = self[index]
    self.replaceSubrange(index..<(index + 1), with: EmptyCollection())
    return result
  }


  /// Inserts a new element at the specified position.
  ///
  /// The new element is inserted before the element currently at the specified
  /// index. If you pass the array's `endIndex` property as the `index`
  /// parameter, the new element is appended to the array.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(100, at: 3)
  ///     numbers.insert(200, at: numbers.endIndex)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 4, 5, 200]"
  ///
  /// - Parameter newElement: The new element to insert into the array.
  /// - Parameter i: The position at which to insert the new element.
  ///   `index` must be a valid index of the array or equal to its `endIndex`
  ///   property.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array. If
  ///   `i == endIndex`, this method is equivalent to `append(_:)`.
  @inlinable
  public mutating func insert(_ newElement: __owned Element, at i: Int) {
    _checkIndex(i)
    self.replaceSubrange(i..<i, with: CollectionOfOne(newElement))
  }

  /// Removes all elements from the array.
  ///
  /// - Parameter keepCapacity: Pass `true` to keep the existing capacity of
  ///   the array after removing its elements. The default value is
  ///   `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array.
  @inlinable
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    if !keepCapacity {
      _buffer = _Buffer()
    }
    else if _buffer.isMutableAndUniquelyReferenced() {
      self.replaceSubrange(indices, with: EmptyCollection())
    }
    else {
      let buffer = _ContiguousArrayBuffer<Element>(
        _uninitializedCount: 0,
        minimumCapacity: capacity
      )
      _buffer = _Buffer(_buffer: buffer, shiftedToStartIndex: startIndex)
    }
  }

  //===--- algorithms -----------------------------------------------------===//

  @inlinable
  @available(*, deprecated, renamed: "withContiguousMutableStorageIfAvailable")
  public mutating func _withUnsafeMutableBufferPointerIfSupported<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withUnsafeMutableBufferPointer {
      (bufferPointer) -> R in
      return try body(&bufferPointer)
    }
  }

  @inlinable
  public mutating func withContiguousMutableStorageIfAvailable<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withUnsafeMutableBufferPointer {
      (bufferPointer) -> R in
      return try body(&bufferPointer)
    }
  }

  @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withUnsafeBufferPointer {
      (bufferPointer) -> R in
      return try body(bufferPointer)
    }
  }

  @inlinable
  public __consuming func _copyToContiguousArray() -> ContiguousArray<Element> {
    if let n = _buffer.requestNativeBuffer() {
      return ContiguousArray(_buffer: n)
    }
    return _copyCollectionToContiguousArray(self)
  }
}

#if SWIFT_ENABLE_REFLECTION
extension ArraySlice: CustomReflectable {
  /// A mirror that reflects the array.
  public var customMirror: Mirror {
    return Mirror(
      self,
      unlabeledChildren: self,
      displayStyle: .collection)
  }
}
#endif

extension ArraySlice: CustomStringConvertible, CustomDebugStringConvertible {
  /// A textual representation of the array and its elements.
  public var description: String {
    return _makeCollectionDescription()
  }

  /// A textual representation of the array and its elements, suitable for
  /// debugging.
  public var debugDescription: String {
    return _makeCollectionDescription(withTypeName: "ArraySlice")
  }
}

extension ArraySlice {
  @usableFromInline @_transparent
  internal func _cPointerArgs() -> (AnyObject?, UnsafeRawPointer?) {
    let p = _baseAddressIfContiguous
    if _fastPath(p != nil || isEmpty) {
      return (_owner, UnsafeRawPointer(p))
    }
    let n = ContiguousArray(self._buffer)._buffer
    return (n.owner, UnsafeRawPointer(n.firstElementAddress))
  }
}

extension ArraySlice {
  /// Calls a closure with a pointer to the array's contiguous storage.
  ///
  /// Often, the optimizer can eliminate bounds checks within an array
  /// algorithm, but when that fails, invoking the same algorithm on the
  /// buffer pointer passed into your closure lets you trade safety for speed.
  ///
  /// The following example shows how you can iterate over the contents of the
  /// buffer pointer:
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     let sum = numbers.withUnsafeBufferPointer { buffer -> Int in
  ///         var result = 0
  ///         for i in stride(from: buffer.startIndex, to: buffer.endIndex, by: 2) {
  ///             result += buffer[i]
  ///         }
  ///         return result
  ///     }
  ///     // 'sum' == 9
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withUnsafeBufferPointer(_:)`. Do not store or return the
  /// pointer for later use.
  ///
  /// - Parameter body: A closure with an `UnsafeBufferPointer` parameter that
  ///   points to the contiguous storage for the array.  If
  ///   `body` has a return value, that value is also used as the return value
  ///   for the `withUnsafeBufferPointer(_:)` method. The pointer argument is
  ///   valid only for the duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    return try _buffer.withUnsafeBufferPointer(body)
  }

  /// Calls the given closure with a pointer to the array's mutable contiguous
  /// storage.
  ///
  /// Often, the optimizer can eliminate bounds checks within an array
  /// algorithm, but when that fails, invoking the same algorithm on the
  /// buffer pointer passed into your closure lets you trade safety for speed.
  ///
  /// The following example shows how modifying the contents of the
  /// `UnsafeMutableBufferPointer` argument to `body` alters the contents of
  /// the array:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.withUnsafeMutableBufferPointer { buffer in
  ///         for i in stride(from: buffer.startIndex, to: buffer.endIndex - 1, by: 2) {
  ///             buffer.swapAt(i, i + 1)
  ///         }
  ///     }
  ///     print(numbers)
  ///     // Prints "[2, 1, 4, 3, 5]"
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withUnsafeMutableBufferPointer(_:)`. Do not store or
  /// return the pointer for later use.
  ///
  /// - Warning: Do not rely on anything about the array that is the target of
  ///   this method during execution of the `body` closure; it might not
  ///   appear to have its correct value. Instead, use only the
  ///   `UnsafeMutableBufferPointer` argument to `body`.
  ///
  /// - Parameter body: A closure with an `UnsafeMutableBufferPointer`
  ///   parameter that points to the contiguous storage for the array.
  ///    If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeMutableBufferPointer(_:)`
  ///   method. The pointer argument is valid only for the duration of the
  ///   method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @_semantics("array.withUnsafeMutableBufferPointer")
  @inlinable // FIXME(inline-always)
  @inline(__always) // Performance: This method should get inlined into the
  // caller such that we can combine the partial apply with the apply in this
  // function saving on allocating a closure context. This becomes unnecessary
  // once we allocate noescape closures on the stack.
  public mutating func withUnsafeMutableBufferPointer<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    let count = self.count
    // Ensure unique storage
    _makeMutableAndUnique()

    // Create an UnsafeBufferPointer that we can pass to body
    let pointer = _buffer.firstElementAddress
    var inoutBufferPointer = UnsafeMutableBufferPointer(
      start: pointer, count: count)

    defer {
      _precondition(
        inoutBufferPointer.baseAddress == pointer &&
        inoutBufferPointer.count == count,
        "ArraySlice withUnsafeMutableBufferPointer: replacing the buffer is not allowed")
      _endMutation()
      _fixLifetime(self)
    }

    // Invoke the body.
    return try body(&inoutBufferPointer)
  }

  @inlinable
  public __consuming func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator,UnsafeMutableBufferPointer<Element>.Index) {

    guard !self.isEmpty else { return (makeIterator(),buffer.startIndex) }

    // It is not OK for there to be no pointer/not enough space, as this is
    // a precondition and Array never lies about its count.
    guard var p = buffer.baseAddress
      else { _preconditionFailure("Attempt to copy contents into nil buffer pointer") }
    _precondition(self.count <= buffer.count, 
      "Insufficient space allocated to copy array contents")

    if let s = _baseAddressIfContiguous {
      p.initialize(from: s, count: self.count)
      // Need a _fixLifetime bracketing the _baseAddressIfContiguous getter
      // and all uses of the pointer it returns:
      _fixLifetime(self._owner)
    } else {
      for x in self {
        p.initialize(to: x)
        p += 1
      }
    }

    var it = IndexingIterator(_elements: self)
    it._position = endIndex
    return (it,buffer.index(buffer.startIndex, offsetBy: self.count))
  }
}

extension ArraySlice {
  /// Replaces a range of elements with the elements in the specified
  /// collection.
  ///
  /// This method has the effect of removing the specified range of elements
  /// from the array and inserting the new elements at the same location. The
  /// number of new elements need not match the number of elements being
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
  /// - Parameters:
  ///   - subrange: The subrange of the array to replace. The start and end of
  ///     a subrange must be valid indices of the array.
  ///   - newElements: The new elements to add to the array.
  ///
  /// - Complexity: O(*n* + *m*), where *n* is length of the array and
  ///   *m* is the length of `newElements`. If the call to this method simply
  ///   appends the contents of `newElements` to the array, this method is
  ///   equivalent to `append(contentsOf:)`.
  @inlinable
  @_semantics("array.mutate_unknown")
  public mutating func replaceSubrange<C>(
    _ subrange: Range<Int>,
    with newElements: __owned C
  ) where C: Collection, C.Element == Element {
    _precondition(subrange.lowerBound >= _buffer.startIndex,
      "ArraySlice replace: subrange start is before the startIndex")

    _precondition(subrange.upperBound <= _buffer.endIndex,
      "ArraySlice replace: subrange extends past the end")

    let oldCount = _buffer.count
    let eraseCount = subrange.count
    let insertCount = newElements.count
    let growth = insertCount - eraseCount

    if _buffer.beginCOWMutation() && _buffer.capacity >= oldCount + growth {
      _buffer.replaceSubrange(
        subrange, with: insertCount, elementsOf: newElements)
    } else {
      _buffer._arrayOutOfPlaceReplace(subrange, with: newElements, count: insertCount)
    }
    _endMutation()
  }
}

extension ArraySlice: Equatable where Element: Equatable {
  /// Returns a Boolean value indicating whether two arrays contain the same
  /// elements in the same order.
  ///
  /// You can use the equal-to operator (`==`) to compare any two arrays
  /// that store the same, `Equatable`-conforming element type.
  ///
  /// - Parameters:
  ///   - lhs: An array to compare.
  ///   - rhs: Another array to compare.
  @inlinable
  public static func ==(lhs: ArraySlice<Element>, rhs: ArraySlice<Element>) -> Bool {
    let lhsCount = lhs.count
    if lhsCount != rhs.count {
      return false
    }

    // Test referential equality.
    if lhsCount == 0 || lhs._buffer.identity == rhs._buffer.identity {
      return true
    }


    var streamLHS = lhs.makeIterator()
    var streamRHS = rhs.makeIterator()

    var nextLHS = streamLHS.next()
    while nextLHS != nil {
      let nextRHS = streamRHS.next()
      if nextLHS != nextRHS {
        return false
      }
      nextLHS = streamLHS.next()
    }


    return true
  }
}

extension ArraySlice: Hashable where Element: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(count) // discriminator
    for element in self {
      hasher.combine(element)
    }
  }
}

extension ArraySlice {
  /// Calls the given closure with a pointer to the underlying bytes of the
  /// array's mutable contiguous storage.
  ///
  /// The array's `Element` type must be a *trivial type*, which can be copied
  /// with just a bit-for-bit copy without any indirection or
  /// reference-counting operations. Generally, native Swift types that do not
  /// contain strong or weak references are trivial, as are imported C structs
  /// and enums.
  ///
  /// The following example copies bytes from the `byteValues` array into
  /// `numbers`, an array of `Int32`:
  ///
  ///     var numbers: [Int32] = [0, 0]
  ///     var byteValues: [UInt8] = [0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00]
  ///
  ///     numbers.withUnsafeMutableBytes { destBytes in
  ///         byteValues.withUnsafeBytes { srcBytes in
  ///             destBytes.copyBytes(from: srcBytes)
  ///         }
  ///     }
  ///     // numbers == [1, 2]
  ///
  /// - Note: This example shows the behavior on a little-endian platform.
  ///
  /// The pointer passed as an argument to `body` is valid only for the
  /// lifetime of the closure. Do not escape it from the closure for later
  /// use.
  ///
  /// - Warning: Do not rely on anything about the array that is the target of
  ///   this method during execution of the `body` closure; it might not
  ///   appear to have its correct value. Instead, use only the
  ///   `UnsafeMutableRawBufferPointer` argument to `body`.
  ///
  /// - Parameter body: A closure with an `UnsafeMutableRawBufferPointer`
  ///   parameter that points to the contiguous storage for the array.
  ///    If no such storage exists, it is created. If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeMutableBytes(_:)` method.
  ///   The argument is valid only for the duration of the closure's
  ///   execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public mutating func withUnsafeMutableBytes<R>(
    _ body: (UnsafeMutableRawBufferPointer) throws -> R
  ) rethrows -> R {
    return try self.withUnsafeMutableBufferPointer {
      return try body(UnsafeMutableRawBufferPointer($0))
    }
  }

  /// Calls the given closure with a pointer to the underlying bytes of the
  /// array's contiguous storage.
  ///
  /// The array's `Element` type must be a *trivial type*, which can be copied
  /// with just a bit-for-bit copy without any indirection or
  /// reference-counting operations. Generally, native Swift types that do not
  /// contain strong or weak references are trivial, as are imported C structs
  /// and enums.
  ///
  /// The following example copies the bytes of the `numbers` array into a
  /// buffer of `UInt8`:
  ///
  ///     var numbers: [Int32] = [1, 2, 3]
  ///     var byteBuffer: [UInt8] = []
  ///     numbers.withUnsafeBytes {
  ///         byteBuffer.append(contentsOf: $0)
  ///     }
  ///     // byteBuffer == [1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0]
  ///
  /// - Note: This example shows the behavior on a little-endian platform.
  ///
  /// - Parameter body: A closure with an `UnsafeRawBufferPointer` parameter
  ///   that points to the contiguous storage for the array.
  ///    If no such storage exists, it is created. If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeBytes(_:)` method. The
  ///   argument is valid only for the duration of the closure's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public func withUnsafeBytes<R>(
    _ body: (UnsafeRawBufferPointer) throws -> R
  ) rethrows -> R {
    return try self.withUnsafeBufferPointer {
      try body(UnsafeRawBufferPointer($0))
    }
  }
}

extension ArraySlice {
  @inlinable
  public // @testable
  init(_startIndex: Int) {
    self.init(
      _buffer: _Buffer(
        _buffer: ContiguousArray()._buffer,
        shiftedToStartIndex: _startIndex))
  }
}

extension ArraySlice: @unchecked Sendable
  where Element: Sendable { }

#if INTERNAL_CHECKS_ENABLED
extension ArraySlice {
  // This allows us to test the `_copyContents` implementation in
  // `_SliceBuffer`. (It's like `_copyToContiguousArray` but it always makes a
  // copy.)
  @_alwaysEmitIntoClient
  public func _copyToNewArray() -> [Element] {
    Array(unsafeUninitializedCapacity: self.count) { buffer, count in
      var (it, c) = self._buffer._copyContents(initializing: buffer)
      _precondition(it.next() == nil)
      count = c
    }
  }
}
#endif

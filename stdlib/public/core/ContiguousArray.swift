//===--- ContiguousArray.swift --------------------------------*- swift -*-===//
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
//  Three generic, mutable array-like types with value semantics.
//
//  - `ContiguousArray<Element>` is a fast, contiguous array of `Element` with
//    a known backing store.
//
//===----------------------------------------------------------------------===//

/// A contiguously stored array.
///
/// The `ContiguousArray` type is a specialized array that always stores its
/// elements in a contiguous region of memory. This contrasts with `Array`,
/// which can store its elements in either a contiguous region of memory or an
/// `NSArray` instance if its `Element` type is a class or `@objc` protocol.
///
/// If your array's `Element` type is a class or `@objc` protocol and you do
/// not need to bridge the array to `NSArray` or pass the array to Objective-C
/// APIs, using `ContiguousArray` may be more efficient and have more
/// predictable performance than `Array`. If the array's `Element` type is a
/// struct or enumeration, `Array` and `ContiguousArray` should have similar
/// efficiency.
///
/// For more information about using arrays, see `Array` and `ArraySlice`, with
/// which `ContiguousArray` shares most properties and methods.
@frozen
@_eagerMove
public struct ContiguousArray<Element>: _DestructorSafeContainer {
  @usableFromInline
  internal typealias _Buffer = _ContiguousArrayBuffer<Element>

  @usableFromInline
  internal var _buffer: _Buffer

  /// Initialization from an existing buffer does not have "array.init"
  /// semantics because the caller may retain an alias to buffer.
  @inlinable
  internal init(_buffer: _Buffer) {
    self._buffer = _buffer
  }
}

//===--- private helpers---------------------------------------------------===//
extension ContiguousArray {
  @inlinable
  @inline(__always)
  @_semantics("array.get_count")
  internal func _getCount() -> Int {
    return _buffer.immutableCount
  }

  @inlinable
  @_semantics("array.get_capacity")
  internal func _getCapacity() -> Int {
    return _buffer.immutableCapacity
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeMutableAndUnique() {
    if _slowPath(!_buffer.beginCOWMutation()) {
      _buffer = _buffer._consumeAndCreateNew()
    }
  }

  /// Marks the end of an Array mutation.
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
  ///
  /// - Precondition: The buffer must be uniquely referenced and native.
  @_alwaysEmitIntoClient
  @_semantics("array.check_subscript")
  internal func _checkSubscript_mutating(_ index: Int) {
    _buffer._checkValidSubscriptMutating(index)
  }

  /// Check that the specified `index` is valid, i.e. `0 ≤ index ≤ count`.
  @inlinable
  @_semantics("array.check_index")
  internal func _checkIndex(_ index: Int) {
    _precondition(index <= endIndex, "ContiguousArray index is out of range")
    _precondition(index >= startIndex, "Negative ContiguousArray index is out of range")
  }

  @inlinable
  @_semantics("array.get_element_address")
  internal func _getElementAddress(_ index: Int) -> UnsafeMutablePointer<Element> {
    return _buffer.firstElementAddress + index
  }
}

extension ContiguousArray: _ArrayProtocol {
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

extension ContiguousArray: RandomAccessCollection, MutableCollection {
  /// The index type for arrays, `Int`.
  public typealias Index = Int

  /// The type that represents the indices that are valid for subscripting an
  /// array, in ascending order.
  public typealias Indices = Range<Int>

  /// The type that allows iteration over an array's elements.
  public typealias Iterator = IndexingIterator<ContiguousArray>

  /// The position of the first element in a nonempty array.
  ///
  /// For an instance of `ContiguousArray`, `startIndex` is always zero. If the array
  /// is empty, `startIndex` is equal to `endIndex`.
  @inlinable
  public var startIndex: Int {
    return 0
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
  public var endIndex: Int {
    @inlinable
    @inline(__always)
    get {
      return _getCount()
    }
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
  ///   unless the array's storage is shared with another array, in which case
  ///   writing is O(*n*), where *n* is the length of the array.
  @inlinable
  public subscript(index: Int) -> Element {
    get {
      _checkSubscript_native(index)
      return _buffer.getElement(index)
    }
    _modify {
      _makeMutableAndUnique()
      _checkSubscript_mutating(index)
      let address = _buffer.mutableFirstElementAddress + index
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

extension ContiguousArray: ExpressibleByArrayLiteral {
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
  ///     let ingredients: ContiguousArray =
  ///           ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///
  /// - Parameter elements: A variadic list of elements of the new array.
  @inlinable
  public init(arrayLiteral elements: Element...) {
    self.init(_buffer: ContiguousArray(elements)._buffer)
  }
}


extension ContiguousArray: RangeReplaceableCollection {
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
  public init<S: Sequence>(_ s: S) where S.Element == Element {
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
    var p: UnsafeMutablePointer<Element>
    (self, p) = ContiguousArray._allocateUninitialized(count)
    for _ in 0..<count {
      p.initialize(to: repeatedValue)
      p += 1
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

  /// Construct a ContiguousArray of `count` uninitialized elements.
  @inlinable
  internal init(_uninitializedCount count: Int) {
    _precondition(count >= 0, "Can't construct ContiguousArray with count < 0")
    // Note: Sinking this constructor into an else branch below causes an extra
    // Retain/Release.
    _buffer = _Buffer()
    if count > 0 {
      // Creating a buffer instead of calling reserveCapacity saves doing an
      // unnecessary uniqueness check. We disable inlining here to curb code
      // growth.
      _buffer = ContiguousArray._allocateBufferUninitialized(minimumCapacity: count)
      _buffer.mutableCount = count
    }
    // Can't store count here because the buffer might be pointing to the
    // shared empty array.
  }

  /// Entry point for `Array` literal construction; builds and returns
  /// a ContiguousArray of `count` uninitialized elements.
  @inlinable
  @_semantics("array.uninitialized")
  internal static func _allocateUninitialized(
    _ count: Int
  ) -> (ContiguousArray, UnsafeMutablePointer<Element>) {
    let result = ContiguousArray(_uninitializedCount: count)
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
    _reserveCapacityImpl(minimumCapacity: minimumCapacity,
                         growForAppend: false)
    _endMutation()
  }

  /// Reserves enough space to store `minimumCapacity` elements.
  /// If a new buffer needs to be allocated and `growForAppend` is true,
  /// the new capacity is calculated using `_growArrayCapacity`.
  @_alwaysEmitIntoClient
  internal mutating func _reserveCapacityImpl(
    minimumCapacity: Int, growForAppend: Bool
  ) {
    let isUnique = _buffer.beginCOWMutation()
    if _slowPath(!isUnique || _buffer.mutableCapacity < minimumCapacity) {
      _createNewBuffer(bufferIsUnique: isUnique,
                       minimumCapacity: Swift.max(minimumCapacity, _buffer.count),
                       growForAppend: growForAppend)
    }
    _internalInvariant(_buffer.mutableCapacity >= minimumCapacity)
    _internalInvariant(_buffer.mutableCapacity == 0 || _buffer.isUniquelyReferenced())
  }

  /// Creates a new buffer, replacing the current buffer.
  ///
  /// If `bufferIsUnique` is true, the buffer is assumed to be uniquely
  /// referenced by this array and the elements are moved - instead of copied -
  /// to the new buffer.
  /// The `minimumCapacity` is the lower bound for the new capacity.
  /// If `growForAppend` is true, the new capacity is calculated using
  /// `_growArrayCapacity`.
  @_alwaysEmitIntoClient
  @inline(never)
  internal mutating func _createNewBuffer(
    bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool
  ) {
    _internalInvariant(!bufferIsUnique || _buffer.isUniquelyReferenced())
    _buffer = _buffer._consumeAndCreateNew(bufferIsUnique: bufferIsUnique,
                                           minimumCapacity: minimumCapacity,
                                           growForAppend: growForAppend)
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
      _createNewBuffer(bufferIsUnique: false,
                       minimumCapacity: count &+ 1,
                       growForAppend: true)
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
    let capacity = _buffer.mutableCapacity
    _internalInvariant(capacity == 0 || _buffer.isMutableAndUniquelyReferenced())

    if _slowPath(oldCount &+ 1 > capacity) {
      _createNewBuffer(bufferIsUnique: capacity > 0,
                       minimumCapacity: oldCount &+ 1,
                       growForAppend: true)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _appendElementAssumeUniqueAndCapacity(
    _ oldCount: Int,
    newElement: __owned Element
  ) {
    _internalInvariant(_buffer.isMutableAndUniquelyReferenced())
    _internalInvariant(_buffer.mutableCapacity >= _buffer.mutableCount &+ 1)

    _buffer.mutableCount = oldCount &+ 1
    (_buffer.mutableFirstElementAddress + oldCount).initialize(to: newElement)
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
    // Separating uniqueness check and capacity check allows hoisting the
    // uniqueness check out of a loop.
    _makeUniqueAndReserveCapacityIfNotUnique()
    let oldCount = _buffer.mutableCount
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

    defer {
      _endMutation()
    }

    let newElementsCount = newElements.underestimatedCount
    _reserveCapacityImpl(minimumCapacity: self.count + newElementsCount,
                         growForAppend: true)

    let oldCount = _buffer.mutableCount
    let startNewElements = _buffer.mutableFirstElementAddress + oldCount
    let buf = UnsafeMutableBufferPointer(
                start: startNewElements, 
                count: _buffer.mutableCapacity - oldCount)

    var (remainder,writtenUpTo) = buf.initialize(from: newElements)
    
    // trap on underflow from the sequence's underestimate:
    let writtenCount = buf.distance(from: buf.startIndex, to: writtenUpTo)
    _precondition(newElementsCount <= writtenCount, 
      "newElements.underestimatedCount was an overestimate")
    // can't check for overflow as sequences can underestimate

    // This check prevents a data race writing to _swiftEmptyArrayStorage
    if writtenCount > 0 {
      _buffer.mutableCount = _buffer.mutableCount + writtenCount
    }

    if writtenUpTo == buf.endIndex {
      // there may be elements that didn't fit in the existing buffer,
      // append them in slow sequence-only mode
      var newCount = _buffer.mutableCount
      var nextItem = remainder.next()
      while nextItem != nil {
        _reserveCapacityAssumingUniqueBuffer(oldCount: newCount)

        let currentCapacity = _buffer.mutableCapacity
        let base = _buffer.mutableFirstElementAddress

        // fill while there is another item and spare capacity
        while let next = nextItem, newCount < currentCapacity {
          (base + newCount).initialize(to: next)
          newCount += 1
          nextItem = remainder.next()
        }
        _buffer.mutableCount = newCount
      }
    }
  }

  @inlinable
  @_semantics("array.reserve_capacity_for_append")
  internal mutating func reserveCapacityForAppend(newElementsCount: Int) {
    // Ensure uniqueness, mutability, and sufficient storage.  Note that
    // for consistency, we need unique self even if newElements is empty.
    _reserveCapacityImpl(minimumCapacity: self.count + newElementsCount,
                         growForAppend: true)
    _endMutation()
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  public mutating func _customRemoveLast() -> Element? {
    _makeMutableAndUnique()
    let newCount = _buffer.mutableCount - 1
    _precondition(newCount >= 0, "Can't removeLast from an empty ContiguousArray")
    let pointer = (_buffer.mutableFirstElementAddress + newCount)
    let element = pointer.move()
    _buffer.mutableCount = newCount
    _endMutation()
    return element
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
  @_semantics("array.mutate_unknown")
  public mutating func remove(at index: Int) -> Element {
    _makeMutableAndUnique()
    let currentCount = _buffer.mutableCount
    _precondition(index < currentCount, "Index out of range")
    _precondition(index >= 0, "Index out of range")
    let newCount = currentCount - 1
    let pointer = (_buffer.mutableFirstElementAddress + index)
    let result = pointer.move()
    pointer.moveInitialize(from: pointer + 1, count: newCount - index)
    _buffer.mutableCount = newCount
    _endMutation()
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
      _buffer = _Buffer(_uninitializedCount: 0, minimumCapacity: capacity)
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
extension ContiguousArray: CustomReflectable {
  /// A mirror that reflects the array.
  public var customMirror: Mirror {
    return Mirror(
      self,
      unlabeledChildren: self,
      displayStyle: .collection)
  }
}
#endif

extension ContiguousArray: CustomStringConvertible, CustomDebugStringConvertible {
  /// A textual representation of the array and its elements.
  public var description: String {
    return _makeCollectionDescription()
  }

  /// A textual representation of the array and its elements, suitable for
  /// debugging.
  public var debugDescription: String {
    return _makeCollectionDescription(withTypeName: "ContiguousArray")
  }
}

extension ContiguousArray {
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

extension ContiguousArray {
  /// Creates an array with the specified capacity, then calls the given
  /// closure with a buffer covering the array's uninitialized memory.
  ///
  /// Inside the closure, set the `initializedCount` parameter to the number of
  /// elements that are initialized by the closure. The memory in the range
  /// `buffer[0..<initializedCount]` must be initialized at the end of the
  /// closure's execution, and the memory in the range
  /// `buffer[initializedCount...]` must be uninitialized. This postcondition
  /// must hold even if the `initializer` closure throws an error.
  ///
  /// - Note: While the resulting array may have a capacity larger than the
  ///   requested amount, the buffer passed to the closure will cover exactly
  ///   the requested number of elements.
  ///
  /// - Parameters:
  ///   - unsafeUninitializedCapacity: The number of elements to allocate
  ///     space for in the new array.
  ///   - initializer: A closure that initializes elements and sets the count
  ///     of the new array.
  ///     - Parameters:
  ///       - buffer: A buffer covering uninitialized memory with room for the
  ///         specified number of elements.
  ///       - initializedCount: The count of initialized elements in the array,
  ///         which begins as zero. Set `initializedCount` to the number of
  ///         elements you initialize.
  @_alwaysEmitIntoClient @inlinable
  public init(
    unsafeUninitializedCapacity: Int,
    initializingWith initializer: (
      _ buffer: inout UnsafeMutableBufferPointer<Element>,
      _ initializedCount: inout Int) throws -> Void
  ) rethrows {
    self = try ContiguousArray(Array(
      _unsafeUninitializedCapacity: unsafeUninitializedCapacity,
      initializingWith: initializer))
  }

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
    _makeMutableAndUnique()
    let count = _buffer.mutableCount

    // Create an UnsafeBufferPointer that we can pass to body
    let pointer = _buffer.mutableFirstElementAddress
    var inoutBufferPointer = UnsafeMutableBufferPointer(
      start: pointer, count: count)

    defer {
      _precondition(
        inoutBufferPointer.baseAddress == pointer &&
        inoutBufferPointer.count == count,
        "ContiguousArray withUnsafeMutableBufferPointer: replacing the buffer is not allowed")
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


extension ContiguousArray {
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
    _precondition(subrange.lowerBound >= self._buffer.startIndex,
      "ContiguousArray replace: subrange start is negative")

    _precondition(subrange.upperBound <= _buffer.endIndex,
      "ContiguousArray replace: subrange extends past the end")

    let eraseCount = subrange.count
    let insertCount = newElements.count
    let growth = insertCount - eraseCount

    _reserveCapacityImpl(minimumCapacity: self.count + growth,
                         growForAppend: true)
    _buffer.replaceSubrange(subrange, with: insertCount, elementsOf: newElements)
    _endMutation()
  }
}

extension ContiguousArray: Equatable where Element: Equatable {
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
  public static func ==(lhs: ContiguousArray<Element>, rhs: ContiguousArray<Element>) -> Bool {
    let lhsCount = lhs.count
    if lhsCount != rhs.count {
      return false
    }

    // Test referential equality.
    if lhsCount == 0 || lhs._buffer.identity == rhs._buffer.identity {
      return true
    }


    _internalInvariant(lhs.startIndex == 0 && rhs.startIndex == 0)
    _internalInvariant(lhs.endIndex == lhsCount && rhs.endIndex == lhsCount)

    // We know that lhs.count == rhs.count, compare element wise.
    for idx in 0..<lhsCount {
      if lhs[idx] != rhs[idx] {
        return false
      }
    }

    return true
  }
}

extension ContiguousArray: Hashable where Element: Hashable {
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

extension ContiguousArray {
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

extension ContiguousArray: @unchecked Sendable
  where Element: Sendable { }

//===--- Range.swift ------------------------------------------*- swift -*-===//
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

/// A type that can be used to slice a collection.
///
/// A type that conforms to `RangeExpression` can convert itself to a
/// `Range<Bound>` of indices within a given collection.
public protocol RangeExpression<Bound> {
  /// The type for which the expression describes a range.
  associatedtype Bound: Comparable

  /// Returns the range of indices described by this range expression within
  /// the given collection.
  ///
  /// You can use the `relative(to:)` method to convert a range expression,
  /// which could be missing one or both of its endpoints, into a concrete
  /// range that is bounded on both sides. The following example uses this
  /// method to convert a partial range up to `4` into a half-open range,
  /// using an array instance to add the range's lower bound.
  ///
  ///     let numbers = [10, 20, 30, 40, 50, 60, 70]
  ///     let upToFour = ..<4
  ///
  ///     let r1 = upToFour.relative(to: numbers)
  ///     // r1 == 0..<4
  ///
  /// The `r1` range is bounded on the lower end by `0` because that is the
  /// starting index of the `numbers` array. When the collection passed to
  /// `relative(to:)` starts with a different index, that index is used as the
  /// lower bound instead. The next example creates a slice of `numbers`
  /// starting at index `2`, and then uses the slice with `relative(to:)` to
  /// convert `upToFour` to a concrete range.
  ///
  ///     let numbersSuffix = numbers[2...]
  ///     // numbersSuffix == [30, 40, 50, 60, 70]
  ///
  ///     let r2 = upToFour.relative(to: numbersSuffix)
  ///     // r2 == 2..<4
  ///
  /// Use this method only if you need the concrete range it produces. To
  /// access a slice of a collection using a range expression, use the
  /// collection's generic subscript that uses a range expression as its
  /// parameter.
  ///
  ///     let numbersPrefix = numbers[upToFour]
  ///     // numbersPrefix == [10, 20, 30, 40]
  ///
  /// - Parameter collection: The collection to evaluate this range expression
  ///   in relation to.
  /// - Returns: A range suitable for slicing `collection`. The returned range
  ///   is *not* guaranteed to be inside the bounds of `collection`. Callers
  ///   should apply the same preconditions to the return value as they would
  ///   to a range provided directly by the user.
  func relative<C: Collection>(
    to collection: C
  ) -> Range<Bound> where C.Index == Bound
  
  /// Returns a Boolean value indicating whether the given element is contained
  /// within the range expression.
  ///
  /// - Parameter element: The element to check for containment.
  /// - Returns: `true` if `element` is contained in the range expression;
  ///   otherwise, `false`.
  func contains(_ element: Bound) -> Bool
}

extension RangeExpression {

  /// Returns a Boolean value indicating whether a value is included in a
  /// range.
  ///
  /// You can use the pattern-matching operator (`~=`) to test whether a value
  /// is included in a range. The pattern-matching operator is used
  /// internally in `case` statements for pattern matching. The following
  /// example uses the `~=` operator to test whether an integer is included in
  /// a range of single-digit numbers:
  ///
  ///     let chosenNumber = 3
  ///     if 0..<10 ~= chosenNumber {
  ///         print("\(chosenNumber) is a single digit.")
  ///     }
  ///     // Prints "3 is a single digit."
  ///
  /// - Parameters:
  ///   - pattern: A range.
  ///   - bound: A value to match against `pattern`.
  @inlinable
  public static func ~= (pattern: Self, value: Bound) -> Bool {
    return pattern.contains(value)
  }  
}

/// A half-open interval from a lower bound up to, but not including, an upper
/// bound.
///
/// You create a `Range` instance by using the half-open range operator
/// (`..<`).
///
///     let underFive = 0.0..<5.0
///
/// You can use a `Range` instance to quickly check if a value is contained in
/// a particular range of values. For example:
///
///     underFive.contains(3.14)
///     // true
///     underFive.contains(6.28)
///     // false
///     underFive.contains(5.0)
///     // false
///
/// `Range` instances can represent an empty interval, unlike `ClosedRange`.
///
///     let empty = 0.0..<0.0
///     empty.contains(0.0)
///     // false
///     empty.isEmpty
///     // true
///
/// Using a Range as a Collection of Consecutive Values
/// ----------------------------------------------------
///
/// When a range uses integers as its lower and upper bounds, or any other type
/// that conforms to the `Strideable` protocol with an integer stride, you can
/// use that range in a `for`-`in` loop or with any sequence or collection
/// method. The elements of the range are the consecutive values from its
/// lower bound up to, but not including, its upper bound.
///
///     for n in 3..<5 {
///         print(n)
///     }
///     // Prints "3"
///     // Prints "4"
///
/// Because floating-point types such as `Float` and `Double` are their own
/// `Stride` types, they cannot be used as the bounds of a countable range. If
/// you need to iterate over consecutive floating-point values, see the
/// `stride(from:to:by:)` function.
@frozen
public struct Range<Bound: Comparable> {
  /// The range's lower bound.
  ///
  /// In an empty range, `lowerBound` is equal to `upperBound`.
  public let lowerBound: Bound

  /// The range's upper bound.
  ///
  /// In an empty range, `upperBound` is equal to `lowerBound`. A `Range`
  /// instance does not contain its upper bound.
  public let upperBound: Bound

  // This works around _debugPrecondition() impacting the performance of
  // optimized code. (rdar://72246338)
  @_alwaysEmitIntoClient @inline(__always)
  internal init(_uncheckedBounds bounds: (lower: Bound, upper: Bound)) {
    self.lowerBound = bounds.lower
    self.upperBound = bounds.upper
  }

  /// Creates an instance with the given bounds.
  ///
  /// Because this initializer does not perform any checks, it should be used
  /// as an optimization only when you are absolutely certain that `lower` is
  /// less than or equal to `upper`. Using the half-open range operator
  /// (`..<`) to form `Range` instances is preferred.
  ///
  /// - Parameter bounds: A tuple of the lower and upper bounds of the range.
  @inlinable
  public init(uncheckedBounds bounds: (lower: Bound, upper: Bound)) {
    _debugPrecondition(bounds.lower <= bounds.upper,
      "Range requires lowerBound <= upperBound")
    self.init(_uncheckedBounds: (lower: bounds.lower, upper: bounds.upper))
  }

  /// Returns a Boolean value indicating whether the given element is contained
  /// within the range.
  ///
  /// Because `Range` represents a half-open range, a `Range` instance does not
  /// contain its upper bound. `element` is contained in the range if it is
  /// greater than or equal to the lower bound and less than the upper bound.
  ///
  /// - Parameter element: The element to check for containment.
  /// - Returns: `true` if `element` is contained in the range; otherwise,
  ///   `false`.
  @inlinable
  public func contains(_ element: Bound) -> Bool {
    return lowerBound <= element && element < upperBound
  }

  /// A Boolean value indicating whether the range contains no elements.
  ///
  /// An empty `Range` instance has equal lower and upper bounds.
  ///
  ///     let empty: Range = 10..<10
  ///     print(empty.isEmpty)
  ///     // Prints "true"
  @inlinable
  public var isEmpty: Bool {
    return lowerBound == upperBound
  }
}

extension Range: Sequence
where Bound: Strideable, Bound.Stride: SignedInteger {
  public typealias Element = Bound
  public typealias Iterator = IndexingIterator<Range<Bound>>
}

extension Range: Collection, BidirectionalCollection, RandomAccessCollection
where Bound: Strideable, Bound.Stride: SignedInteger
{
  /// A type that represents a position in the range.
  public typealias Index = Bound
  public typealias Indices = Range<Bound>
  public typealias SubSequence = Range<Bound>

  @inlinable
  public var startIndex: Index { return lowerBound }

  @inlinable
  public var endIndex: Index { return upperBound }

  @inlinable
  @inline(__always)
  public func index(after i: Index) -> Index {
    _failEarlyRangeCheck(i, bounds: startIndex..<endIndex)

    return i.advanced(by: 1)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    _precondition(i > lowerBound)
    _precondition(i <= upperBound)

    return i.advanced(by: -1)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    let r = i.advanced(by: numericCast(n))
    _precondition(r >= lowerBound)
    _precondition(r <= upperBound)
    return r
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return numericCast(start.distance(to: end))
  }

  /// Accesses the subsequence bounded by the given range.
  ///
  /// - Parameter bounds: A range of the range's indices. The upper and lower
  ///   bounds of the `bounds` range must be valid indices of the collection.
  @inlinable
  public subscript(bounds: Range<Index>) -> Range<Bound> {
    return bounds
  }

  /// The indices that are valid for subscripting the range, in ascending
  /// order.
  @inlinable
  public var indices: Indices {
    return self
  }

  @inlinable
  public func _customContainsEquatableElement(_ element: Element) -> Bool? {
    return lowerBound <= element && element < upperBound
  }

  @inlinable
  public func _customIndexOfEquatableElement(_ element: Bound) -> Index?? {
    return lowerBound <= element && element < upperBound ? element : nil
  }

  @inlinable
  public func _customLastIndexOfEquatableElement(_ element: Bound) -> Index?? {
    // The first and last elements are the same because each element is unique.
    return _customIndexOfEquatableElement(element)
  }

  /// Accesses the element at specified position.
  ///
  /// You can subscript a collection with any valid index other than the
  /// collection's end index. The end index refers to the position one past
  /// the last element of a collection, so it doesn't correspond with an
  /// element.
  ///
  /// - Parameter position: The position of the element to access. `position`
  ///   must be a valid index of the range, and must not equal the range's end
  ///   index.
  @inlinable
  public subscript(position: Index) -> Element {
    // FIXME: swift-3-indexing-model: tests for the range check.
    _debugPrecondition(self.contains(position), "Index out of range")
    return position
  }
}

extension Range where Bound: Strideable, Bound.Stride: SignedInteger {
  /// Creates an instance equivalent to the given `ClosedRange`.
  ///
  /// - Parameter other: A closed range to convert to a `Range` instance.
  ///
  /// An equivalent range must be representable as an instance of Range<Bound>.
  /// For example, passing a closed range with an upper bound of `Int.max`
  /// triggers a runtime error, because the resulting half-open range would
  /// require an upper bound of `Int.max + 1`, which is not representable as
  /// an `Int`.
  @inlinable // trivial-implementation
  public init(_ other: ClosedRange<Bound>) {
    let upperBound = other.upperBound.advanced(by: 1)
    self.init(_uncheckedBounds: (lower: other.lowerBound, upper: upperBound))
  }
}

extension Range: RangeExpression {
  /// Returns the range of indices described by this range expression within
  /// the given collection.
  ///
  /// - Parameter collection: The collection to evaluate this range expression
  ///   in relation to.
  /// - Returns: A range suitable for slicing `collection`. The returned range
  ///   is *not* guaranteed to be inside the bounds of `collection`. Callers
  ///   should apply the same preconditions to the return value as they would
  ///   to a range provided directly by the user.
  @inlinable // trivial-implementation
  public func relative<C: Collection>(to collection: C) -> Range<Bound>
  where C.Index == Bound {
    self
  }
}

extension Range {
  /// Returns a copy of this range clamped to the given limiting range.
  ///
  /// The bounds of the result are always limited to the bounds of `limits`.
  /// For example:
  ///
  ///     let x: Range = 0..<20
  ///     print(x.clamped(to: 10..<1000))
  ///     // Prints "10..<20"
  ///
  /// If the two ranges do not overlap, the result is an empty range within the
  /// bounds of `limits`.
  ///
  ///     let y: Range = 0..<5
  ///     print(y.clamped(to: 10..<1000))
  ///     // Prints "10..<10"
  ///
  /// - Parameter limits: The range to clamp the bounds of this range.
  /// - Returns: A new range clamped to the bounds of `limits`.
  @inlinable // trivial-implementation
  @inline(__always)
  public func clamped(to limits: Range) -> Range {
    let lower =         
      limits.lowerBound > self.lowerBound ? limits.lowerBound
          : limits.upperBound < self.lowerBound ? limits.upperBound
          : self.lowerBound
    let upper =
      limits.upperBound < self.upperBound ? limits.upperBound
          : limits.lowerBound > self.upperBound ? limits.lowerBound
          : self.upperBound
    return Range(_uncheckedBounds: (lower: lower, upper: upper))
  }
}

@_unavailableInEmbedded
extension Range: CustomStringConvertible {
  /// A textual representation of the range.
  @inlinable // trivial-implementation
  public var description: String {
    return "\(lowerBound)..<\(upperBound)"
  }
}

@_unavailableInEmbedded
extension Range: CustomDebugStringConvertible {
  /// A textual representation of the range, suitable for debugging.
  public var debugDescription: String {
    return "Range(\(String(reflecting: lowerBound))"
    + "..<\(String(reflecting: upperBound)))"
  }
}

#if SWIFT_ENABLE_REFLECTION
extension Range: CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(
      self, children: ["lowerBound": lowerBound, "upperBound": upperBound])
  }
}
#endif

extension Range: Equatable {
  /// Returns a Boolean value indicating whether two ranges are equal.
  ///
  /// Two ranges are equal when they have the same lower and upper bounds.
  /// That requirement holds even for empty ranges.
  ///
  ///     let x = 5..<15
  ///     print(x == 5..<15)
  ///     // Prints "true"
  ///
  ///     let y = 5..<5
  ///     print(y == 15..<15)
  ///     // Prints "false"
  ///
  /// - Parameters:
  ///   - lhs: A range to compare.
  ///   - rhs: Another range to compare.
  @inlinable
  public static func == (lhs: Range<Bound>, rhs: Range<Bound>) -> Bool {
    return
      lhs.lowerBound == rhs.lowerBound &&
      lhs.upperBound == rhs.upperBound
  }
}

extension Range: Hashable where Bound: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(lowerBound)
    hasher.combine(upperBound)
  }
}

@_unavailableInEmbedded
extension Range: Decodable where Bound: Decodable {
  public init(from decoder: Decoder) throws {
    var container = try decoder.unkeyedContainer()
    let lowerBound = try container.decode(Bound.self)
    let upperBound = try container.decode(Bound.self)
    guard lowerBound <= upperBound else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Range.self) with a lowerBound (\(lowerBound)) greater than upperBound (\(upperBound))"))
    }
    self.init(_uncheckedBounds: (lower: lowerBound, upper: upperBound))
  }
}

@_unavailableInEmbedded
extension Range: Encodable where Bound: Encodable {
  public func encode(to encoder: Encoder) throws {
    var container = encoder.unkeyedContainer()
    try container.encode(self.lowerBound)
    try container.encode(self.upperBound)
  }
}

/// A partial half-open interval up to, but not including, an upper bound.
///
/// You create `PartialRangeUpTo` instances by using the prefix half-open range
/// operator (prefix `..<`).
///
///     let upToFive = ..<5.0
///
/// You can use a `PartialRangeUpTo` instance to quickly check if a value is
/// contained in a particular range of values. For example:
///
///     upToFive.contains(3.14)       // true
///     upToFive.contains(6.28)       // false
///     upToFive.contains(5.0)        // false
///
/// You can use a `PartialRangeUpTo` instance of a collection's indices to
/// represent the range from the start of the collection up to, but not
/// including, the partial range's upper bound.
///
///     let numbers = [10, 20, 30, 40, 50, 60, 70]
///     print(numbers[..<3])
///     // Prints "[10, 20, 30]"
@frozen
public struct PartialRangeUpTo<Bound: Comparable> {
  public let upperBound: Bound
  
  @inlinable // trivial-implementation
  public init(_ upperBound: Bound) { self.upperBound = upperBound }
}

extension PartialRangeUpTo: RangeExpression {
  @_transparent
  public func relative<C: Collection>(to collection: C) -> Range<Bound>
  where C.Index == Bound {
    return collection.startIndex..<self.upperBound
  }
  
  @_transparent
  public func contains(_ element: Bound) -> Bool {
    return element < upperBound
  }
}

@_unavailableInEmbedded
extension PartialRangeUpTo: Decodable where Bound: Decodable {
  public init(from decoder: Decoder) throws {
    var container = try decoder.unkeyedContainer()
    try self.init(container.decode(Bound.self))
  }
}

@_unavailableInEmbedded
extension PartialRangeUpTo: Encodable where Bound: Encodable {
  public func encode(to encoder: Encoder) throws {
    var container = encoder.unkeyedContainer()
    try container.encode(self.upperBound)
  }
}

/// A partial interval up to, and including, an upper bound.
///
/// You create `PartialRangeThrough` instances by using the prefix closed range
/// operator (prefix `...`).
///
///     let throughFive = ...5.0
///
/// You can use a `PartialRangeThrough` instance to quickly check if a value is
/// contained in a particular range of values. For example:
///
///     throughFive.contains(4.0)     // true
///     throughFive.contains(5.0)     // true
///     throughFive.contains(6.0)     // false
///
/// You can use a `PartialRangeThrough` instance of a collection's indices to
/// represent the range from the start of the collection up to, and including,
/// the partial range's upper bound.
///
///     let numbers = [10, 20, 30, 40, 50, 60, 70]
///     print(numbers[...3])
///     // Prints "[10, 20, 30, 40]"
@frozen
public struct PartialRangeThrough<Bound: Comparable> {  
  public let upperBound: Bound
  
  @inlinable // trivial-implementation
  public init(_ upperBound: Bound) { self.upperBound = upperBound }
}

extension PartialRangeThrough: RangeExpression {
  @_transparent
  public func relative<C: Collection>(to collection: C) -> Range<Bound>
  where C.Index == Bound {
    return collection.startIndex..<collection.index(after: self.upperBound)
  }
  @_transparent
  public func contains(_ element: Bound) -> Bool {
    return element <= upperBound
  }
}

@_unavailableInEmbedded
extension PartialRangeThrough: Decodable where Bound: Decodable {
  public init(from decoder: Decoder) throws {
    var container = try decoder.unkeyedContainer()
    try self.init(container.decode(Bound.self))
  }
}

@_unavailableInEmbedded
extension PartialRangeThrough: Encodable where Bound: Encodable {
  public func encode(to encoder: Encoder) throws {
    var container = encoder.unkeyedContainer()
    try container.encode(self.upperBound)
  }
}

/// A partial interval extending upward from a lower bound.
///
/// You create `PartialRangeFrom` instances by using the postfix range operator
/// (postfix `...`).
///
///     let atLeastFive = 5...
///
/// You can use a partial range to quickly check if a value is contained in a
/// particular range of values. For example:
///
///     atLeastFive.contains(4)
///     // false
///     atLeastFive.contains(5)
///     // true
///     atLeastFive.contains(6)
///     // true
///
/// You can use a partial range of a collection's indices to represent the
/// range from the partial range's lower bound up to the end of the
/// collection.
///
///     let numbers = [10, 20, 30, 40, 50, 60, 70]
///     print(numbers[3...])
///     // Prints "[40, 50, 60, 70]"
///
/// Using a Partial Range as a Sequence
/// -----------------------------------
///
/// When a partial range uses integers as its lower and upper bounds, or any
/// other type that conforms to the `Strideable` protocol with an integer
/// stride, you can use that range in a `for`-`in` loop or with any sequence
/// method that doesn't require that the sequence is finite. The elements of
/// a partial range are the consecutive values from its lower bound continuing
/// upward indefinitely.
///
///     func isTheMagicNumber(_ x: Int) -> Bool {
///         return x == 3
///     }
///
///     for x in 1... {
///         if isTheMagicNumber(x) {
///             print("\(x) is the magic number!")
///             break
///         } else {
///             print("\(x) wasn't it...")
///         }
///     }
///     // "1 wasn't it..."
///     // "2 wasn't it..."
///     // "3 is the magic number!"
///
/// Because a `PartialRangeFrom` sequence counts upward indefinitely, do not
/// use one with methods that read the entire sequence before returning, such
/// as `map(_:)`, `filter(_:)`, or `suffix(_:)`. It is safe to use operations
/// that put an upper limit on the number of elements they access, such as
/// `prefix(_:)` or `dropFirst(_:)`, and operations that you can guarantee
/// will terminate, such as passing a closure you know will eventually return
/// `true` to `first(where:)`.
///
/// In the following example, the `asciiTable` sequence is made by zipping
/// together the characters in the `alphabet` string with a partial range
/// starting at 65, the ASCII value of the capital letter A. Iterating over
/// two zipped sequences continues only as long as the shorter of the two
/// sequences, so the iteration stops at the end of `alphabet`.
///
///     let alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
///     let asciiTable = zip(65..., alphabet)
///     for (code, letter) in asciiTable {
///         print(code, letter)
///     }
///     // "65 A"
///     // "66 B"
///     // "67 C"
///     // ...
///     // "89 Y"
///     // "90 Z"
///
/// The behavior of incrementing indefinitely is determined by the type of
/// `Bound`. For example, iterating over an instance of
/// `PartialRangeFrom<Int>` traps when the sequence's next value would be
/// above `Int.max`.
@frozen
public struct PartialRangeFrom<Bound: Comparable> {
  public let lowerBound: Bound

  @inlinable // trivial-implementation
  public init(_ lowerBound: Bound) { self.lowerBound = lowerBound }
}

extension PartialRangeFrom: RangeExpression {
  @_transparent
  public func relative<C: Collection>(
    to collection: C
  ) -> Range<Bound> where C.Index == Bound {
    return self.lowerBound..<collection.endIndex
  }
  @inlinable // trivial-implementation
  public func contains(_ element: Bound) -> Bool {
    return lowerBound <= element
  }
}

extension PartialRangeFrom: Sequence
  where Bound: Strideable, Bound.Stride: SignedInteger
{
  public typealias Element = Bound

  /// The iterator for a `PartialRangeFrom` instance.
  @frozen
  public struct Iterator: IteratorProtocol {
    @usableFromInline
    internal var _current: Bound
    @inlinable
    public init(_current: Bound) { self._current = _current }

    /// Advances to the next element and returns it, or `nil` if no next
    /// element exists.
    ///
    /// Once `nil` has been returned, all subsequent calls return `nil`.
    ///
    /// - Returns: The next element in the underlying sequence, if a next
    ///   element exists; otherwise, `nil`.
    @inlinable
    public mutating func next() -> Bound? {
      defer { _current = _current.advanced(by: 1) }
      return _current
    }
  }

  /// Returns an iterator for this sequence.
  @inlinable
  public __consuming func makeIterator() -> Iterator { 
    return Iterator(_current: lowerBound) 
  }
}

@_unavailableInEmbedded
extension PartialRangeFrom: Decodable where Bound: Decodable {
  public init(from decoder: Decoder) throws {
    var container = try decoder.unkeyedContainer()
    try self.init(container.decode(Bound.self))
  }
}

@_unavailableInEmbedded
extension PartialRangeFrom: Encodable where Bound: Encodable {
  public func encode(to encoder: Encoder) throws {
    var container = encoder.unkeyedContainer()
    try container.encode(self.lowerBound)
  }
}

extension Comparable {
  /// Returns a half-open range that contains its lower bound but not its upper
  /// bound.
  ///
  /// Use the half-open range operator (`..<`) to create a range of any type
  /// that conforms to the `Comparable` protocol. This example creates a
  /// `Range<Double>` from zero up to, but not including, 5.0.
  ///
  ///     let lessThanFive = 0.0..<5.0
  ///     print(lessThanFive.contains(3.14))  // Prints "true"
  ///     print(lessThanFive.contains(5.0))   // Prints "false"
  ///
  /// - Parameters:
  ///   - minimum: The lower bound for the range.
  ///   - maximum: The upper bound for the range.
  ///
  /// - Precondition: `minimum <= maximum`.
  @_transparent
  public static func ..< (minimum: Self, maximum: Self) -> Range<Self> {
    _precondition(minimum <= maximum,
      "Range requires lowerBound <= upperBound")
    return Range(_uncheckedBounds: (lower: minimum, upper: maximum))
  }

  /// Returns a partial range up to, but not including, its upper bound.
  ///
  /// Use the prefix half-open range operator (prefix `..<`) to create a
  /// partial range of any type that conforms to the `Comparable` protocol.
  /// This example creates a `PartialRangeUpTo<Double>` instance that includes
  /// any value less than `5.0`.
  ///
  ///     let upToFive = ..<5.0
  ///
  ///     upToFive.contains(3.14)       // true
  ///     upToFive.contains(6.28)       // false
  ///     upToFive.contains(5.0)        // false
  ///
  /// You can use this type of partial range of a collection's indices to
  /// represent the range from the start of the collection up to, but not
  /// including, the partial range's upper bound.
  ///
  ///     let numbers = [10, 20, 30, 40, 50, 60, 70]
  ///     print(numbers[..<3])
  ///     // Prints "[10, 20, 30]"
  ///
  /// - Parameter maximum: The upper bound for the range.
  ///
  /// - Precondition: `maximum` must compare equal to itself (i.e. cannot be NaN).
  @_transparent
  public static prefix func ..< (maximum: Self) -> PartialRangeUpTo<Self> {
    _precondition(maximum == maximum,
      "Range cannot have an unordered upper bound.")
    return PartialRangeUpTo(maximum)
  }

  /// Returns a partial range up to, and including, its upper bound.
  ///
  /// Use the prefix closed range operator (prefix `...`) to create a partial
  /// range of any type that conforms to the `Comparable` protocol. This
  /// example creates a `PartialRangeThrough<Double>` instance that includes
  /// any value less than or equal to `5.0`.
  ///
  ///     let throughFive = ...5.0
  ///
  ///     throughFive.contains(4.0)     // true
  ///     throughFive.contains(5.0)     // true
  ///     throughFive.contains(6.0)     // false
  ///
  /// You can use this type of partial range of a collection's indices to
  /// represent the range from the start of the collection up to, and
  /// including, the partial range's upper bound.
  ///
  ///     let numbers = [10, 20, 30, 40, 50, 60, 70]
  ///     print(numbers[...3])
  ///     // Prints "[10, 20, 30, 40]"
  ///
  /// - Parameter maximum: The upper bound for the range.
  ///
  /// - Precondition: `maximum` must compare equal to itself (i.e. cannot be NaN).
  @_transparent
  public static prefix func ... (maximum: Self) -> PartialRangeThrough<Self> {
    _precondition(maximum == maximum,
      "Range cannot have an unordered upper bound.")
    return PartialRangeThrough(maximum)
  }

  /// Returns a partial range extending upward from a lower bound.
  ///
  /// Use the postfix range operator (postfix `...`) to create a partial range
  /// of any type that conforms to the `Comparable` protocol. This example
  /// creates a `PartialRangeFrom<Double>` instance that includes any value
  /// greater than or equal to `5.0`.
  ///
  ///     let atLeastFive = 5.0...
  ///
  ///     atLeastFive.contains(4.0)     // false
  ///     atLeastFive.contains(5.0)     // true
  ///     atLeastFive.contains(6.0)     // true
  ///
  /// You can use this type of partial range of a collection's indices to
  /// represent the range from the partial range's lower bound up to the end
  /// of the collection.
  ///
  ///     let numbers = [10, 20, 30, 40, 50, 60, 70]
  ///     print(numbers[3...])
  ///     // Prints "[40, 50, 60, 70]"
  ///
  /// - Parameter minimum: The lower bound for the range.
  ///
  /// - Precondition: `minimum` must compare equal to itself (i.e. cannot be NaN).
  @_transparent
  public static postfix func ... (minimum: Self) -> PartialRangeFrom<Self> {
    _precondition(minimum == minimum,
      "Range cannot have an unordered lower bound.")
    return PartialRangeFrom(minimum)
  }
}

/// A range expression that represents the entire range of a collection.
///
/// You can use the unbounded range operator (`...`) to create a slice of a
/// collection that contains all of the collection's elements. Slicing with an
/// unbounded range is essentially a conversion of a collection instance into
/// its slice type.
///
/// For example, the following code declares `countLetterChanges(_:_:)`, a
/// function that finds the number of changes required to change one
/// word or phrase into another. The function uses a recursive approach to
/// perform the same comparisons on smaller and smaller pieces of the original
/// strings. In order to use recursion without making copies of the strings at
/// each step, `countLetterChanges(_:_:)` uses `Substring`, a string's slice
/// type, for its parameters.
///
///     func countLetterChanges(_ s1: Substring, _ s2: Substring) -> Int {
///         if s1.isEmpty { return s2.count }
///         if s2.isEmpty { return s1.count }
///
///         let cost = s1.first == s2.first ? 0 : 1
///
///         return min(
///             countLetterChanges(s1.dropFirst(), s2) + 1,
///             countLetterChanges(s1, s2.dropFirst()) + 1,
///             countLetterChanges(s1.dropFirst(), s2.dropFirst()) + cost)
///     }
///
/// To call `countLetterChanges(_:_:)` with two strings, use an unbounded
/// range in each string's subscript.
///
///     let word1 = "grizzly"
///     let word2 = "grisly"
///     let changes = countLetterChanges(word1[...], word2[...])
///     // changes == 2
@frozen // namespace
public enum UnboundedRange_ {
  // FIXME: replace this with a computed var named `...` when the language makes
  // that possible.

  /// Creates an unbounded range expression.
  ///
  /// The unbounded range operator (`...`) is valid only within a collection's
  /// subscript.
  public static postfix func ... (_: UnboundedRange_) -> () {
    // This function is uncallable
  }
}

/// The type of an unbounded range operator.
public typealias UnboundedRange = (UnboundedRange_)->()

extension Collection {
  /// Accesses the contiguous subrange of the collection's elements specified
  /// by a range expression.
  ///
  /// The range expression is converted to a concrete subrange relative to this
  /// collection. For example, using a `PartialRangeFrom` range expression
  /// with an array accesses the subrange from the start of the range
  /// expression until the end of the array.
  ///
  ///     let streets = ["Adams", "Bryant", "Channing", "Douglas", "Evarts"]
  ///     let streetsSlice = streets[2...]
  ///     print(streetsSlice)
  ///     // ["Channing", "Douglas", "Evarts"]
  ///
  /// The accessed slice uses the same indices for the same elements as the
  /// original collection uses. This example searches `streetsSlice` for one
  /// of the strings in the slice, and then uses that index in the original
  /// array.
  ///
  ///     let index = streetsSlice.firstIndex(of: "Evarts")    // 4
  ///     print(streets[index!])
  ///     // "Evarts"
  ///
  /// Always use the slice's `startIndex` property instead of assuming that its
  /// indices start at a particular value. Attempting to access an element by
  /// using an index outside the bounds of the slice's indices may result in a
  /// runtime error, even if that index is valid for the original collection.
  ///
  ///     print(streetsSlice.startIndex)
  ///     // 2
  ///     print(streetsSlice[2])
  ///     // "Channing"
  ///
  ///     print(streetsSlice[0])
  ///     // error: Index out of bounds
  ///
  /// - Parameter bounds: A range of the collection's indices. The bounds of
  ///   the range must be valid indices of the collection.
  ///
  /// - Complexity: O(1)
  @inlinable
  public subscript<R: RangeExpression>(r: R)
  -> SubSequence where R.Bound == Index {
    return self[r.relative(to: self)]
  }
  
  @inlinable
  public subscript(x: UnboundedRange) -> SubSequence {
    return self[startIndex...]
  }
}

extension MutableCollection {
  @inlinable
  public subscript<R: RangeExpression>(r: R) -> SubSequence
  where R.Bound == Index {
    get {
      return self[r.relative(to: self)]
    }
    set {
      self[r.relative(to: self)] = newValue
    }
  }

  @inlinable
  public subscript(x: UnboundedRange) -> SubSequence {
    get {
      return self[startIndex...]
    }
    set {
      self[startIndex...] = newValue
    }
  }
}

// TODO: enhance RangeExpression to make this generic and available on
// any expression.
extension Range {
  /// Returns a Boolean value indicating whether this range and the given range
  /// contain an element in common.
  ///
  /// This example shows two overlapping ranges:
  ///
  ///     let x: Range = 0..<20
  ///     print(x.overlaps(10...1000))
  ///     // Prints "true"
  ///
  /// Because a half-open range does not include its upper bound, the ranges
  /// in the following example do not overlap:
  ///
  ///     let y = 20..<30
  ///     print(x.overlaps(y))
  ///     // Prints "false"
  ///
  /// - Parameter other: A range to check for elements in common.
  /// - Returns: `true` if this range and `other` have at least one element in
  ///   common; otherwise, `false`.
  @inlinable
  public func overlaps(_ other: Range<Bound>) -> Bool {
    // Disjoint iff the other range is completely before or after our range.
    // Additionally either `Range` (unlike a `ClosedRange`) could be empty, in
    // which case it is disjoint with everything as overlap is defined as having
    // an element in common.
    let isDisjoint = other.upperBound <= self.lowerBound
      || self.upperBound <= other.lowerBound
      || self.isEmpty || other.isEmpty
    return !isDisjoint
  }

  @inlinable
  public func overlaps(_ other: ClosedRange<Bound>) -> Bool {
    // Disjoint iff the other range is completely before or after our range.
    // Additionally the `Range` (unlike the `ClosedRange`) could be empty, in
    // which case it is disjoint with everything as overlap is defined as having
    // an element in common.
    let isDisjoint = other.upperBound < self.lowerBound
      || self.upperBound <= other.lowerBound
      || self.isEmpty
    return !isDisjoint
  }
}

// Note: this is not for compatibility only, it is considered a useful
// shorthand. TODO: Add documentation
public typealias CountableRange<Bound: Strideable> = Range<Bound>
  where Bound.Stride: SignedInteger

// Note: this is not for compatibility only, it is considered a useful
// shorthand. TODO: Add documentation
public typealias CountablePartialRangeFrom<Bound: Strideable> = PartialRangeFrom<Bound>
  where Bound.Stride: SignedInteger

extension Range: Sendable where Bound: Sendable { }
extension PartialRangeUpTo: Sendable where Bound: Sendable { }
extension PartialRangeThrough: Sendable where Bound: Sendable { }
extension PartialRangeFrom: Sendable where Bound: Sendable { }
extension PartialRangeFrom.Iterator: Sendable where Bound: Sendable { }

extension Range where Bound == String.Index {
  @_alwaysEmitIntoClient // Swift 5.7
  internal var _encodedOffsetRange: Range<Int> {
    _internalInvariant(
      (lowerBound._canBeUTF8 && upperBound._canBeUTF8)
      || (lowerBound._canBeUTF16 && upperBound._canBeUTF16))
    return Range<Int>(
      _uncheckedBounds: (lowerBound._encodedOffset, upperBound._encodedOffset))
  }
}

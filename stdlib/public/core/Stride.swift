//===--- Stride.swift - Components for stride(...) iteration --------------===//
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

/// A type representing continuous, one-dimensional values that can be offset
/// and measured.
///
/// You can use a type that conforms to the `Strideable` protocol with the
/// `stride(from:to:by:)` and `stride(from:through:by:)` functions. For
/// example, you can use `stride(from:to:by:)` to iterate over an
/// interval of floating-point values:
///
///     for radians in stride(from: 0.0, to: .pi * 2, by: .pi / 2) {
///         let degrees = Int(radians * 180 / .pi)
///         print("Degrees: \(degrees), radians: \(radians)")
///     }
///     // Degrees: 0, radians: 0.0
///     // Degrees: 90, radians: 1.5707963267949
///     // Degrees: 180, radians: 3.14159265358979
///     // Degrees: 270, radians: 4.71238898038469
///
/// The last parameter of these functions is of the associated `Stride`
/// type---the type that represents the distance between any two instances of
/// the `Strideable` type.
///
/// Types that have an integer `Stride` can be used as the boundaries of a
/// countable range or as the lower bound of an iterable one-sided range. For
/// example, you can iterate over a range of `Int` and use sequence and
/// collection methods.
///
///     var sum = 0
///     for x in 1...100 {
///         sum += x
///     }
///     // sum == 5050
///
///     let digits = (0..<10).map(String.init)
///     // ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
///
/// Conforming to the Strideable Protocol
/// =====================================
///
/// To add `Strideable` conformance to a custom type, choose a `Stride` type
/// that can represent the distance between two instances and implement the
/// `advanced(by:)` and `distance(to:)` methods. For example, this
/// hypothetical `Date` type stores its value as the number of days before or
/// after January 1, 2000:
///
///     struct Date: Equatable, CustomStringConvertible {
///         var daysAfterY2K: Int
///
///         var description: String {
///             // ...
///         }
///     }
///
/// The `Stride` type for `Date` is `Int`, inferred from the parameter and
/// return types of `advanced(by:)` and `distance(to:)`:
///
///     extension Date: Strideable {
///         func advanced(by n: Int) -> Date {
///             var result = self
///             result.daysAfterY2K += n
///             return result
///         }
///
///         func distance(to other: Date) -> Int {
///             return other.daysAfterY2K - self.daysAfterY2K
///         }
///     }
///
/// The `Date` type can now be used with the `stride(from:to:by:)` and
/// `stride(from:through:by:)` functions and as the bounds of an iterable
/// range.
///
///     let startDate = Date(daysAfterY2K: 0)   // January 1, 2000
///     let endDate = Date(daysAfterY2K: 15)    // January 16, 2000
///
///     for date in stride(from: startDate, to: endDate, by: 7) {
///         print(date)
///     }
///     // January 1, 2000
///     // January 8, 2000
///     // January 15, 2000
///
/// - Important: The `Strideable` protocol provides default implementations for
///   the equal-to (`==`) and less-than (`<`) operators that depend on the
///   `Stride` type's implementations. If a type conforming to `Strideable` is
///   its own `Stride` type, it must provide concrete implementations of the
///   two operators to avoid infinite recursion.
public protocol Strideable: Comparable {
  /// A type that represents the distance between two values.
  associatedtype Stride: SignedNumeric, Comparable

  /// Returns the distance from this value to the given value, expressed as a 
  /// stride.
  ///
  /// If this type's `Stride` type conforms to `BinaryInteger`, then for two
  /// values `x` and `y`, and a distance `n = x.distance(to: y)`,
  /// `x.advanced(by: n) == y`. Using this method with types that have a
  /// noninteger `Stride` may result in an approximation.
  ///
  /// - Parameter other: The value to calculate the distance to.
  /// - Returns: The distance from this value to `other`.
  ///
  /// - Complexity: O(1)
  func distance(to other: Self) -> Stride

  /// Returns a value that is offset the specified distance from this value.
  ///
  /// Use the `advanced(by:)` method in generic code to offset a value by a
  /// specified distance. If you're working directly with numeric values, use
  /// the addition operator (`+`) instead of this method.
  ///
  ///     func addOne<T: Strideable>(to x: T) -> T
  ///         where T.Stride: ExpressibleByIntegerLiteral
  ///     {
  ///         return x.advanced(by: 1)
  ///     }
  ///
  ///     let x = addOne(to: 5)
  ///     // x == 6
  ///     let y = addOne(to: 3.5)
  ///     // y = 4.5
  ///
  /// If this type's `Stride` type conforms to `BinaryInteger`, then for a
  /// value `x`, a distance `n`, and a value `y = x.advanced(by: n)`,
  /// `x.distance(to: y) == n`. Using this method with types that have a
  /// noninteger `Stride` may result in an approximation.
  ///
  /// - Parameter n: The distance to advance this value.
  /// - Returns: A value that is offset from this value by `n`.
  ///
  /// - Complexity: O(1)
  func advanced(by n: Stride) -> Self

  /// `_step` is an implementation detail of Strideable; do not use it directly.
  static func _step(
    after current: (index: Int?, value: Self),
    from start: Self, by distance: Self.Stride
  ) -> (index: Int?, value: Self)
}

extension Strideable {
  @inlinable
  public static func < (x: Self, y: Self) -> Bool {
    return x.distance(to: y) > 0
  }

  @inlinable
  public static func == (x: Self, y: Self) -> Bool {
    return x.distance(to: y) == 0
  }
}

extension Strideable {
  @inlinable // protocol-only
  public static func _step(
    after current: (index: Int?, value: Self),
    from start: Self, by distance: Self.Stride
  ) -> (index: Int?, value: Self) {
    return (nil, current.value.advanced(by: distance))
  }
}

extension Strideable where Stride: FloatingPoint {
  @inlinable // protocol-only
  public static func _step(
    after current: (index: Int?, value: Self),
    from start: Self, by distance: Self.Stride
  ) -> (index: Int?, value: Self) {
    if let i = current.index {
      // When Stride is a floating-point type, we should avoid accumulating
      // rounding error from repeated addition.
      return (i + 1, start.advanced(by: Stride(i + 1) * distance))
    }
    return (nil, current.value.advanced(by: distance))
  }
}

extension Strideable where Self: FloatingPoint, Self == Stride {
  @inlinable // protocol-only
  public static func _step(
    after current: (index: Int?, value: Self),
    from start: Self, by distance: Self.Stride
  ) -> (index: Int?, value: Self) {
    if let i = current.index {
      // When both Self and Stride are the same floating-point type, we should
      // take advantage of fused multiply-add (where supported) to eliminate
      // intermediate rounding error.
      return (i + 1, start.addingProduct(Stride(i + 1), distance))
    }
    return (nil, current.value.advanced(by: distance))
  }
}

/// An iterator for a `StrideTo` instance.
@frozen
public struct StrideToIterator<Element: Strideable> {
  @usableFromInline
  internal let _start: Element

  @usableFromInline
  internal let _end: Element

  @usableFromInline
  internal let _stride: Element.Stride

  @usableFromInline
  internal var _current: (index: Int?, value: Element)

  @inlinable
  internal init(_start: Element, end: Element, stride: Element.Stride) {
    self._start = _start
    _end = end
    _stride = stride
    _current = (0, _start)
  }
}

extension StrideToIterator: IteratorProtocol {
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable
  public mutating func next() -> Element? {
    let result = _current.value
    if _stride > 0 ? result >= _end : result <= _end {
      return nil
    }
    _current = Element._step(after: _current, from: _start, by: _stride)
    return result
  }
}

// FIXME: should really be a Collection, as it is multipass
/// A sequence of values formed by striding over a half-open interval.
///
/// Use the `stride(from:to:by:)` function to create `StrideTo` instances.
@frozen
public struct StrideTo<Element: Strideable> {
  @usableFromInline
  internal let _start: Element

  @usableFromInline
  internal let _end: Element

  @usableFromInline
  internal let _stride: Element.Stride

  @inlinable
  internal init(_start: Element, end: Element, stride: Element.Stride) {
    _precondition(stride != 0, "Stride size must not be zero")
    // At start, striding away from end is allowed; it just makes for an
    // already-empty Sequence.
    self._start = _start
    self._end = end
    self._stride = stride
  }
}

extension StrideTo: Sequence {
  /// Returns an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inlinable
  public __consuming func makeIterator() -> StrideToIterator<Element> {
    return StrideToIterator(_start: _start, end: _end, stride: _stride)
  }

  // FIXME(conditional-conformances): this is O(N) instead of O(1), leaving it
  // here until a proper Collection conformance is possible
  @inlinable
  public var underestimatedCount: Int {
    var it = self.makeIterator()
    var count = 0
    while it.next() != nil {
      count += 1
    }
    return count
  }

  @inlinable
  public func _customContainsEquatableElement(
    _ element: Element
  ) -> Bool? {
    if element < _start || _end <= element {
      return false
    }
    return nil
  }
}

#if SWIFT_ENABLE_REFLECTION
extension StrideTo: CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(self, children: ["from": _start, "to": _end, "by": _stride])
  }
}
#endif

// FIXME(conditional-conformances): This does not yet compile (SR-6474).
#if false
extension StrideTo: RandomAccessCollection
where Element.Stride: BinaryInteger {
  public typealias Index = Int
  public typealias SubSequence = Slice<StrideTo<Element>>
  public typealias Indices = Range<Int>

  @inlinable
  public var startIndex: Index { return 0 }

  @inlinable
  public var endIndex: Index { return count }

  @inlinable
  public var count: Int {
    let distance = _start.distance(to: _end)
    guard distance != 0 && (distance < 0) == (_stride < 0) else { return 0 }
    return Int((distance - 1) / _stride) + 1
  }

  public subscript(position: Index) -> Element {
    _failEarlyRangeCheck(position, bounds: startIndex..<endIndex)
    return _start.advanced(by: Element.Stride(position) * _stride)
  }

  public subscript(bounds: Range<Index>) -> Slice<StrideTo<Element>> {
    _failEarlyRangeCheck(bounds, bounds: startIndex ..< endIndex)
    return Slice(base: self, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    _failEarlyRangeCheck(i, bounds: startIndex + 1...endIndex)
    return i - 1
  }

  @inlinable
  public func index(after i: Index) -> Index {
    _failEarlyRangeCheck(i, bounds: startIndex - 1..<endIndex)
    return i + 1
  }
}
#endif

/// Returns a sequence from a starting value to, but not including, an end
/// value, stepping by the specified amount.
///
/// You can use this function to stride over values of any type that conforms
/// to the `Strideable` protocol, such as integers or floating-point types.
/// Starting with `start`, each successive value of the sequence adds `stride`
/// until the next value would be equal to or beyond `end`.
///
///     for radians in stride(from: 0.0, to: .pi * 2, by: .pi / 2) {
///         let degrees = Int(radians * 180 / .pi)
///         print("Degrees: \(degrees), radians: \(radians)")
///     }
///     // Degrees: 0, radians: 0.0
///     // Degrees: 90, radians: 1.5707963267949
///     // Degrees: 180, radians: 3.14159265358979
///     // Degrees: 270, radians: 4.71238898038469
///
/// You can use `stride(from:to:by:)` to create a sequence that strides upward
/// or downward. Pass a negative value as `stride` to create a sequence from a
/// higher start to a lower end:
///
///     for countdown in stride(from: 3, to: 0, by: -1) {
///         print("\(countdown)...")
///     }
///     // 3...
///     // 2...
///     // 1...
///
/// If you pass a value as `stride` that moves away from `end`, the sequence
/// contains no values.
///
///     for x in stride(from: 0, to: 10, by: -1) {
///         print(x)
///     }
///     // Nothing is printed.
///
/// - Parameters:
///   - start: The starting value to use for the sequence. If the sequence
///     contains any values, the first one is `start`.
///   - end: An end value to limit the sequence. `end` is never an element of
///     the resulting sequence.
///   - stride: The amount to step by with each iteration. A positive `stride`
///     iterates upward; a negative `stride` iterates downward.
/// - Returns: A sequence from `start` toward, but not including, `end`. Each
///   value in the sequence steps by `stride`.
@inlinable
public func stride<T>(
  from start: T, to end: T, by stride: T.Stride
) -> StrideTo<T> {
  return StrideTo(_start: start, end: end, stride: stride)
}

/// An iterator for a `StrideThrough` instance.
@frozen
public struct StrideThroughIterator<Element: Strideable> {
  @usableFromInline
  internal let _start: Element

  @usableFromInline
  internal let _end: Element

  @usableFromInline
  internal let _stride: Element.Stride

  @usableFromInline
  internal var _current: (index: Int?, value: Element)

  @usableFromInline
  internal var _didReturnEnd: Bool = false

  @inlinable
  internal init(_start: Element, end: Element, stride: Element.Stride) {
    self._start = _start
    _end = end
    _stride = stride
    _current = (0, _start)
  }
}

extension StrideThroughIterator: IteratorProtocol {
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable
  public mutating func next() -> Element? {
    let result = _current.value
    if _stride > 0 ? result >= _end : result <= _end {
      // This check is needed because if we just changed the above operators
      // to > and <, respectively, we might advance current past the end
      // and throw it out of bounds (e.g. above Int.max) unnecessarily.
      if result == _end && !_didReturnEnd {
        _didReturnEnd = true
        return result
      }
      return nil
    }
    _current = Element._step(after: _current, from: _start, by: _stride)
    return result
  }
}

// FIXME: should really be a Collection, as it is multipass
/// A sequence of values formed by striding over a closed interval.
///
/// Use the `stride(from:through:by:)` function to create `StrideThrough` 
/// instances.
@frozen
public struct StrideThrough<Element: Strideable> {
  @usableFromInline
  internal let _start: Element
  @usableFromInline
  internal let _end: Element
  @usableFromInline
  internal let _stride: Element.Stride
  
  @inlinable
  internal init(_start: Element, end: Element, stride: Element.Stride) {
    _precondition(stride != 0, "Stride size must not be zero")
    self._start = _start
    self._end = end
    self._stride = stride
  }
}

extension StrideThrough: Sequence {
  /// Returns an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inlinable
  public __consuming func makeIterator() -> StrideThroughIterator<Element> {
    return StrideThroughIterator(_start: _start, end: _end, stride: _stride)
  }

  // FIXME(conditional-conformances): this is O(N) instead of O(1), leaving it
  // here until a proper Collection conformance is possible
  @inlinable
  public var underestimatedCount: Int {
    var it = self.makeIterator()
    var count = 0
    while it.next() != nil {
      count += 1
    }
    return count
  }

  @inlinable
  public func _customContainsEquatableElement(
    _ element: Element
  ) -> Bool? {
    if element < _start || _end < element {
      return false
    }
    return nil
  }
}

#if SWIFT_ENABLE_REFLECTION
extension StrideThrough: CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(self,
      children: ["from": _start, "through": _end, "by": _stride])
  }
}
#endif

// FIXME(conditional-conformances): This does not yet compile (SR-6474).
#if false
extension StrideThrough: RandomAccessCollection
where Element.Stride: BinaryInteger {
  public typealias Index = ClosedRangeIndex<Int>
  public typealias SubSequence = Slice<StrideThrough<Element>>

  @inlinable
  public var startIndex: Index {
    let distance = _start.distance(to: _end)
    return distance == 0 || (distance < 0) == (_stride < 0)
      ? ClosedRangeIndex(0)
      : ClosedRangeIndex()
  }

  @inlinable
  public var endIndex: Index { return ClosedRangeIndex() }

  @inlinable
  public var count: Int {
    let distance = _start.distance(to: _end)
    guard distance != 0 else { return 1 }
    guard (distance < 0) == (_stride < 0) else { return 0 }
    return Int(distance / _stride) + 1
  }

  public subscript(position: Index) -> Element {
    let offset = Element.Stride(position._dereferenced) * _stride
    return _start.advanced(by: offset)
  }

  public subscript(bounds: Range<Index>) -> Slice<StrideThrough<Element>> {
    return Slice(base: self, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index {
    switch i._value {
    case .inRange(let n):
      _precondition(n > 0, "Incrementing past start index")
      return ClosedRangeIndex(n - 1)
    case .pastEnd:
      _precondition(_end >= _start, "Incrementing past start index")
      return ClosedRangeIndex(count - 1)
    }
  }

  @inlinable
  public func index(after i: Index) -> Index {
    switch i._value {
    case .inRange(let n):
      return n == (count - 1)
        ? ClosedRangeIndex()
        : ClosedRangeIndex(n + 1)
    case .pastEnd:
      _preconditionFailure("Incrementing past end index")
    }
  }
}
#endif

/// Returns a sequence from a starting value toward, and possibly including, an end
/// value, stepping by the specified amount.
///
/// You can use this function to stride over values of any type that conforms
/// to the `Strideable` protocol, such as integers or floating-point types.
/// Starting with `start`, each successive value of the sequence adds `stride`
/// until the next value would be beyond `end`.
///
///     for radians in stride(from: 0.0, through: .pi * 2, by: .pi / 2) {
///         let degrees = Int(radians * 180 / .pi)
///         print("Degrees: \(degrees), radians: \(radians)")
///     }
///     // Degrees: 0, radians: 0.0
///     // Degrees: 90, radians: 1.5707963267949
///     // Degrees: 180, radians: 3.14159265358979
///     // Degrees: 270, radians: 4.71238898038469
///     // Degrees: 360, radians: 6.28318530717959
///
/// You can use `stride(from:through:by:)` to create a sequence that strides 
/// upward or downward. Pass a negative value as `stride` to create a sequence 
/// from a higher start to a lower end:
///
///     for countdown in stride(from: 3, through: 1, by: -1) {
///         print("\(countdown)...")
///     }
///     // 3...
///     // 2...
///     // 1...
///
/// The value you pass as `end` is not guaranteed to be included in the 
/// sequence. If stepping from `start` by `stride` does not produce `end`, 
/// the last value in the sequence will be one step before going beyond `end`.
///
///     for multipleOfThree in stride(from: 3, through: 10, by: 3) {
///         print(multipleOfThree)
///     }
///     // 3
///     // 6
///     // 9
///
/// If you pass a value as `stride` that moves away from `end`, the sequence 
/// contains no values.
///
///     for x in stride(from: 0, through: 10, by: -1) {
///         print(x)
///     }
///     // Nothing is printed.
///
/// - Parameters:
///   - start: The starting value to use for the sequence. If the sequence
///     contains any values, the first one is `start`.
///   - end: An end value to limit the sequence. `end` is an element of
///     the resulting sequence if and only if it can be produced from `start` 
///     using steps of `stride`.
///   - stride: The amount to step by with each iteration. A positive `stride`
///     iterates upward; a negative `stride` iterates downward.
/// - Returns: A sequence from `start` toward, and possibly including, `end`. 
///   Each value in the sequence is separated by `stride`.
@inlinable
public func stride<T>(
  from start: T, through end: T, by stride: T.Stride
) -> StrideThrough<T> {
  return StrideThrough(_start: start, end: end, stride: stride)
}

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

/// A type that supplies the values of a sequence one at a time.
///
/// The `IteratorProtocol` protocol is tightly linked with the `Sequence`
/// protocol. Sequences provide access to their elements by creating an
/// iterator, which keeps track of its iteration process and returns one
/// element at a time as it advances through the sequence.
///
/// Whenever you use a `for`-`in` loop with an array, set, or any other
/// collection or sequence, you're using that type's iterator. Swift uses a
/// sequence's or collection's iterator internally to enable the `for`-`in`
/// loop language construct.
///
/// Using a sequence's iterator directly gives you access to the same elements
/// in the same order as iterating over that sequence using a `for`-`in` loop.
/// For example, you might typically use a `for`-`in` loop to print each of
/// the elements in an array.
///
///     let animals = ["Antelope", "Butterfly", "Camel", "Dolphin"]
///     for animal in animals {
///         print(animal)
///     }
///     // Prints "Antelope"
///     // Prints "Butterfly"
///     // Prints "Camel"
///     // Prints "Dolphin"
///
/// Behind the scenes, Swift uses the `animals` array's iterator to loop over
/// the contents of the array.
///
///     var animalIterator = animals.makeIterator()
///     while let animal = animalIterator.next() {
///         print(animal)
///     }
///     // Prints "Antelope"
///     // Prints "Butterfly"
///     // Prints "Camel"
///     // Prints "Dolphin"
///
/// The call to `animals.makeIterator()` returns an instance of the array's
/// iterator. Next, the `while` loop calls the iterator's `next()` method
/// repeatedly, binding each element that is returned to `animal` and exiting
/// when the `next()` method returns `nil`.
///
/// Using Iterators Directly
/// ========================
///
/// You rarely need to use iterators directly, because a `for`-`in` loop is the
/// more idiomatic approach to traversing a sequence in Swift. Some
/// algorithms, however, may call for direct iterator use.
///
/// One example is the `reduce1(_:)` method. Similar to the `reduce(_:_:)`
/// method defined in the standard library, which takes an initial value and a
/// combining closure, `reduce1(_:)` uses the first element of the sequence as
/// the initial value.
///
/// Here's an implementation of the `reduce1(_:)` method. The sequence's
/// iterator is used directly to retrieve the initial value before looping
/// over the rest of the sequence.
///
///     extension Sequence {
///         func reduce1(
///             _ nextPartialResult: (Element, Element) -> Element
///         ) -> Element?
///         {
///             var i = makeIterator()
///             guard var accumulated = i.next() else {
///                 return nil
///             }
///
///             while let element = i.next() {
///                 accumulated = nextPartialResult(accumulated, element)
///             }
///             return accumulated
///         }
///     }
///
/// The `reduce1(_:)` method makes certain kinds of sequence operations
/// simpler. Here's how to find the longest string in a sequence, using the
/// `animals` array introduced earlier as an example:
///
///     let longestAnimal = animals.reduce1 { current, element in
///         if current.count > element.count {
///             return current
///         } else {
///             return element
///         }
///     }
///     print(longestAnimal)
///     // Prints Optional("Butterfly")
///
/// Using Multiple Iterators
/// ========================
///
/// Whenever you use multiple iterators (or `for`-`in` loops) over a single
/// sequence, be sure you know that the specific sequence supports repeated
/// iteration, either because you know its concrete type or because the
/// sequence is also constrained to the `Collection` protocol.
///
/// Obtain each separate iterator from separate calls to the sequence's
/// `makeIterator()` method rather than by copying. Copying an iterator is
/// safe, but advancing one copy of an iterator by calling its `next()` method
/// may invalidate other copies of that iterator. `for`-`in` loops are safe in
/// this regard.
///
/// Adding IteratorProtocol Conformance to Your Type
/// ================================================
///
/// Implementing an iterator that conforms to `IteratorProtocol` is simple.
/// Declare a `next()` method that advances one step in the related sequence
/// and returns the current element. When the sequence has been exhausted, the
/// `next()` method returns `nil`.
///
/// For example, consider a custom `Countdown` sequence. You can initialize the
/// `Countdown` sequence with a starting integer and then iterate over the
/// count down to zero. The `Countdown` structure's definition is short: It
/// contains only the starting count and the `makeIterator()` method required
/// by the `Sequence` protocol.
///
///     struct Countdown: Sequence {
///         let start: Int
///
///         func makeIterator() -> CountdownIterator {
///             return CountdownIterator(self)
///         }
///     }
///
/// The `makeIterator()` method returns another custom type, an iterator named
/// `CountdownIterator`. The `CountdownIterator` type keeps track of both the
/// `Countdown` sequence that it's iterating and the number of times it has
/// returned a value.
///
///     struct CountdownIterator: IteratorProtocol {
///         let countdown: Countdown
///         var times = 0
///
///         init(_ countdown: Countdown) {
///             self.countdown = countdown
///         }
///
///         mutating func next() -> Int? {
///             let nextNumber = countdown.start - times
///             guard nextNumber > 0
///                 else { return nil }
///
///             times += 1
///             return nextNumber
///         }
///     }
///
/// Each time the `next()` method is called on a `CountdownIterator` instance,
/// it calculates the new next value, checks to see whether it has reached
/// zero, and then returns either the number, or `nil` if the iterator is
/// finished returning elements of the sequence.
///
/// Creating and iterating over a `Countdown` sequence uses a
/// `CountdownIterator` to handle the iteration.
///
///     let threeTwoOne = Countdown(start: 3)
///     for count in threeTwoOne {
///         print("\(count)...")
///     }
///     // Prints "3..."
///     // Prints "2..."
///     // Prints "1..."
public protocol IteratorProtocol<Element> {
  /// The type of element traversed by the iterator.
  associatedtype Element

  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Repeatedly calling this method returns, in order, all the elements of the
  /// underlying sequence. As soon as the sequence has run out of elements, all
  /// subsequent calls return `nil`.
  ///
  /// You must not call this method if any other copy of this iterator has been
  /// advanced with a call to its `next()` method.
  ///
  /// The following example shows how an iterator can be used explicitly to
  /// emulate a `for`-`in` loop. First, retrieve a sequence's iterator, and
  /// then call the iterator's `next()` method until it returns `nil`.
  ///
  ///     let numbers = [2, 3, 5, 7]
  ///     var numbersIterator = numbers.makeIterator()
  ///
  ///     while let num = numbersIterator.next() {
  ///         print(num)
  ///     }
  ///     // Prints "2"
  ///     // Prints "3"
  ///     // Prints "5"
  ///     // Prints "7"
  ///
  /// - Returns: The next element in the underlying sequence, if a next element
  ///   exists; otherwise, `nil`.
  mutating func next() -> Element?
}

/// A type that provides sequential, iterated access to its elements.
///
/// A sequence is a list of values that you can step through one at a time. The
/// most common way to iterate over the elements of a sequence is to use a
/// `for`-`in` loop:
///
///     let oneTwoThree = 1...3
///     for number in oneTwoThree {
///         print(number)
///     }
///     // Prints "1"
///     // Prints "2"
///     // Prints "3"
///
/// While seemingly simple, this capability gives you access to a large number
/// of operations that you can perform on any sequence. As an example, to
/// check whether a sequence includes a particular value, you can test each
/// value sequentially until you've found a match or reached the end of the
/// sequence. This example checks to see whether a particular insect is in an
/// array.
///
///     let bugs = ["Aphid", "Bumblebee", "Cicada", "Damselfly", "Earwig"]
///     var hasMosquito = false
///     for bug in bugs {
///         if bug == "Mosquito" {
///             hasMosquito = true
///             break
///         }
///     }
///     print("'bugs' has a mosquito: \(hasMosquito)")
///     // Prints "'bugs' has a mosquito: false"
///
/// The `Sequence` protocol provides default implementations for many common
/// operations that depend on sequential access to a sequence's values. For
/// clearer, more concise code, the example above could use the array's
/// `contains(_:)` method, which every sequence inherits from `Sequence`,
/// instead of iterating manually:
///
///     if bugs.contains("Mosquito") {
///         print("Break out the bug spray.")
///     } else {
///         print("Whew, no mosquitos!")
///     }
///     // Prints "Whew, no mosquitos!"
///
/// Repeated Access
/// ===============
///
/// The `Sequence` protocol makes no requirement on conforming types regarding
/// whether they will be destructively consumed by iteration. As a
/// consequence, don't assume that multiple `for`-`in` loops on a sequence
/// will either resume iteration or restart from the beginning:
///
///     for element in sequence {
///         if ... some condition { break }
///     }
///
///     for element in sequence {
///         // No defined behavior
///     }
///
/// In this case, you cannot assume either that a sequence will be consumable
/// and will resume iteration, or that a sequence is a collection and will
/// restart iteration from the first element. A conforming sequence that is
/// not a collection is allowed to produce an arbitrary sequence of elements
/// in the second `for`-`in` loop.
///
/// To establish that a type you've created supports nondestructive iteration,
/// add conformance to the `Collection` protocol.
///
/// Conforming to the Sequence Protocol
/// ===================================
///
/// Making your own custom types conform to `Sequence` enables many useful
/// operations, like `for`-`in` looping and the `contains` method, without
/// much effort. To add `Sequence` conformance to your own custom type, add a
/// `makeIterator()` method that returns an iterator.
///
/// Alternatively, if your type can act as its own iterator, implementing the
/// requirements of the `IteratorProtocol` protocol and declaring conformance
/// to both `Sequence` and `IteratorProtocol` are sufficient.
///
/// Here's a definition of a `Countdown` sequence that serves as its own
/// iterator. The `makeIterator()` method is provided as a default
/// implementation.
///
///     struct Countdown: Sequence, IteratorProtocol {
///         var count: Int
///
///         mutating func next() -> Int? {
///             if count == 0 {
///                 return nil
///             } else {
///                 defer { count -= 1 }
///                 return count
///             }
///         }
///     }
///
///     let threeToGo = Countdown(count: 3)
///     for i in threeToGo {
///         print(i)
///     }
///     // Prints "3"
///     // Prints "2"
///     // Prints "1"
///
/// Expected Performance
/// ====================
///
/// A sequence should provide its iterator in O(1). The `Sequence` protocol
/// makes no other requirements about element access, so routines that
/// traverse a sequence should be considered O(*n*) unless documented
/// otherwise.
public protocol Sequence<Element> {
  /// A type representing the sequence's elements.
  associatedtype Element

  /// A type that provides the sequence's iteration interface and
  /// encapsulates its iteration state.
  associatedtype Iterator: IteratorProtocol where Iterator.Element == Element

  // FIXME: <rdar://problem/34142121>
  // This typealias should be removed as it predates the source compatibility
  // guarantees of Swift 3, but it cannot due to a bug.
  @available(*, unavailable, renamed: "Iterator")
  typealias Generator = Iterator

  /// A type that represents a subsequence of some of the sequence's elements.
  // associatedtype SubSequence: Sequence = AnySequence<Element>
  //   where Element == SubSequence.Element,
  //         SubSequence.SubSequence == SubSequence
  // typealias SubSequence = AnySequence<Element>

  /// Returns an iterator over the elements of this sequence.
  __consuming func makeIterator() -> Iterator

  /// A value less than or equal to the number of elements in the sequence,
  /// calculated nondestructively.
  ///
  /// The default implementation returns 0. If you provide your own
  /// implementation, make sure to compute the value nondestructively.
  ///
  /// - Complexity: O(1), except if the sequence also conforms to `Collection`.
  ///   In this case, see the documentation of `Collection.underestimatedCount`.
  var underestimatedCount: Int { get }

  /// Sequences whose `Element` is `Equatable` and that are able to quickly
  /// check if they contain a particular value can implement this requirement
  /// to speed up the standard `contains` method.
  ///
  /// The default implementation returns nil, indicating that `contains` should
  /// fall back to the standard linear search algorithm.
  ///
  /// `Sequence` and `Collection` algorithms other than `contains` itself may
  /// adapt their behavior based on whether or not this function returns nil.
  /// For example, a generic algorithm that needs to do containment checks for
  /// many different values may decide not to copy items into a temporary `Set`
  /// if it sees that the sequence implements this method. Therefore, sequences
  /// should only implement this method if they can do it in better than linear
  /// time.
  ///
  /// For sequences that are destructively consumed by iteration, calling this
  /// method must not consume any elements. (Such sequences usually leave this
  /// method with its default, `nil`-returning implementation, which trivially
  /// satisfies this requirement.)
  ///
  /// - Returns: `nil` if containment cannot be verified in better than linear
  ///    time; otherwise, the method returns a boolean value indicating whether
  ///    or not the item is an element of this sequence.
  ///
  /// - Complexity: If this function returns `nil`, it must do so in constant
  ///    (O(1)) time. If this returns non-`nil`, then it must have better than linear
  ///    (O(*n*)) complexity.
  func _customContainsEquatableElement(
    _ element: Element
  ) -> Bool?

  /// Create a native array buffer containing the elements of `self`,
  /// in the same order.
  __consuming func _copyToContiguousArray() -> ContiguousArray<Element>

  /// Copy `self` into an unsafe buffer, initializing its memory.
  ///
  /// The default implementation simply iterates over the elements of the
  /// sequence, initializing the buffer one item at a time.
  ///
  /// For sequences whose elements are stored in contiguous chunks of memory,
  /// it may be more efficient to copy them in bulk, using the
  /// `UnsafeMutablePointer.initialize(from:count:)` method.
  ///
  /// - Parameter ptr: An unsafe buffer addressing uninitialized memory. The
  ///    buffer must be of sufficient size to accommodate
  ///    `source.underestimatedCount` elements. (Some implementations trap
  ///    if given a buffer that's smaller than this.)
  ///
  /// - Returns: `(it, c)`, where `c` is the number of elements copied into the
  ///    buffer, and `it` is a partially consumed iterator that can be used to
  ///    retrieve elements that did not fit into the buffer (if any). (This can
  ///    only happen if `underestimatedCount` turned out to be an actual
  ///    underestimate, and the buffer did not contain enough space to hold the
  ///    entire sequence.)
  ///
  ///    On return, the memory region in `buffer[0 ..< c]` is initialized to
  ///    the first `c` elements in the sequence.
  __consuming func _copyContents(
    initializing ptr: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator,UnsafeMutableBufferPointer<Element>.Index)

  /// Executes a closure on the sequence’s contiguous storage.
  ///
  /// This method calls `body(buffer)`, where `buffer` is a pointer to the
  /// collection’s contiguous storage. If the contiguous storage doesn't exist,
  /// the collection creates it. If the collection doesn’t support an internal
  /// representation in a form of contiguous storage, the method doesn’t call
  /// `body` --- it immediately returns `nil`.
  ///
  /// The optimizer can often eliminate bounds- and uniqueness-checking
  /// within an algorithm. When that fails, however, invoking the same
  /// algorithm on the `buffer` argument may let you trade safety for speed.
  ///
  /// Successive calls to this method may provide a different pointer on each
  /// call. Don't store `buffer` outside of this method.
  ///
  /// A `Collection` that provides its own implementation of this method
  /// must provide contiguous storage to its elements in the same order
  /// as they appear in the collection. This guarantees that it's possible to
  /// generate contiguous mutable storage to any of its subsequences by slicing
  /// `buffer` with a range formed from the distances to the subsequence's
  /// `startIndex` and `endIndex`, respectively.
  ///
  /// - Parameters:
  ///   - body: A closure that receives an `UnsafeBufferPointer` to the
  ///     sequence's contiguous storage.
  /// - Returns: The value returned from `body`, unless the sequence doesn't
  ///   support contiguous storage, in which case the method ignores `body` and
  ///   returns `nil`.
  func withContiguousStorageIfAvailable<R>(
    _ body: (_ buffer: UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R?
}

// Provides a default associated type witness for Iterator when the
// Self type is both a Sequence and an Iterator.
extension Sequence where Self: IteratorProtocol {
  // @_implements(Sequence, Iterator)
  public typealias _Default_Iterator = Self
}

/// A default makeIterator() function for `IteratorProtocol` instances that
/// are declared to conform to `Sequence`
extension Sequence where Self.Iterator == Self {
  /// Returns an iterator over the elements of this sequence.
  @inlinable
  public __consuming func makeIterator() -> Self {
    return self
  }
}

/// A sequence that lazily consumes and drops `n` elements from an underlying
/// `Base` iterator before possibly returning the first available element.
///
/// The underlying iterator's sequence may be infinite.
@frozen
public struct DropFirstSequence<Base: Sequence> {
  @usableFromInline
  internal let _base: Base
  @usableFromInline
  internal let _limit: Int
  
  @inlinable 
  public init(_ base: Base, dropping limit: Int) {
    _precondition(limit >= 0, 
      "Can't drop a negative number of elements from a sequence")
    _base = base
    _limit = limit
  }
}

extension DropFirstSequence: Sequence {
  public typealias Element = Base.Element
  public typealias Iterator = Base.Iterator
  public typealias SubSequence = AnySequence<Element>
  
  @inlinable
  public __consuming func makeIterator() -> Iterator {
    var it = _base.makeIterator()
    var dropped = 0
    while dropped < _limit, it.next() != nil { dropped &+= 1 }
    return it
  }

  @inlinable
  public __consuming func dropFirst(_ k: Int) -> DropFirstSequence<Base> {
    // If this is already a _DropFirstSequence, we need to fold in
    // the current drop count and drop limit so no data is lost.
    //
    // i.e. [1,2,3,4].dropFirst(1).dropFirst(1) should be equivalent to
    // [1,2,3,4].dropFirst(2).
    return DropFirstSequence(_base, dropping: _limit + k)
  }
}

/// A sequence that only consumes up to `n` elements from an underlying
/// `Base` iterator.
///
/// The underlying iterator's sequence may be infinite.
@frozen
public struct PrefixSequence<Base: Sequence> {
  @usableFromInline
  internal var _base: Base
  @usableFromInline
  internal let _maxLength: Int

  @inlinable
  public init(_ base: Base, maxLength: Int) {
    _precondition(maxLength >= 0, "Can't take a prefix of negative length")
    _base = base
    _maxLength = maxLength
  }
}

extension PrefixSequence {
  @frozen
  public struct Iterator {
    @usableFromInline
    internal var _base: Base.Iterator
    @usableFromInline
    internal var _remaining: Int
    
    @inlinable
    internal init(_ base: Base.Iterator, maxLength: Int) {
      _base = base
      _remaining = maxLength
    }
  }  
}

extension PrefixSequence.Iterator: IteratorProtocol {
  public typealias Element = Base.Element
  
  @inlinable
  public mutating func next() -> Element? {
    if _remaining != 0 {
      _remaining &-= 1
      return _base.next()
    } else {
      return nil
    }
  }  
}

extension PrefixSequence: Sequence {
  @inlinable
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_base.makeIterator(), maxLength: _maxLength)
  }

  @inlinable
  public __consuming func prefix(_ maxLength: Int) -> PrefixSequence<Base> {
    let length = Swift.min(maxLength, self._maxLength)
    return PrefixSequence(_base, maxLength: length)
  }
}


/// A sequence that lazily consumes and drops `n` elements from an underlying
/// `Base` iterator before possibly returning the first available element.
///
/// The underlying iterator's sequence may be infinite.
@frozen
public struct DropWhileSequence<Base: Sequence> {
  public typealias Element = Base.Element
  
  @usableFromInline
  internal var _iterator: Base.Iterator
  @usableFromInline
  internal var _nextElement: Element?
  
  @inlinable
  internal init(iterator: Base.Iterator, predicate: (Element) throws -> Bool) rethrows {
    _iterator = iterator
    _nextElement = _iterator.next()
    
    while let x = _nextElement, try predicate(x) {
      _nextElement = _iterator.next()
    }
  }
  
  @inlinable
  internal init(_ base: Base, predicate: (Element) throws -> Bool) rethrows {
    self = try DropWhileSequence(iterator: base.makeIterator(), predicate: predicate)
  }
}

extension DropWhileSequence {
  @frozen
  public struct Iterator {
    @usableFromInline
    internal var _iterator: Base.Iterator
    @usableFromInline
    internal var _nextElement: Element?
    
    @inlinable
    internal init(_ iterator: Base.Iterator, nextElement: Element?) {
      _iterator = iterator
      _nextElement = nextElement
    }
  }
}

extension DropWhileSequence.Iterator: IteratorProtocol {
  public typealias Element = Base.Element
  
  @inlinable
  public mutating func next() -> Element? {
    guard let next = _nextElement else { return nil }
    _nextElement = _iterator.next()
    return next
  }
}

extension DropWhileSequence: Sequence {
  @inlinable
  public func makeIterator() -> Iterator {
    return Iterator(_iterator, nextElement: _nextElement)
  }
  
  @inlinable
  public __consuming func drop(
    while predicate: (Element) throws -> Bool
  ) rethrows -> DropWhileSequence<Base> {
    guard let x = _nextElement, try predicate(x) else { return self }
    return try DropWhileSequence(iterator: _iterator, predicate: predicate)
  }
}

//===----------------------------------------------------------------------===//
// Default implementations for Sequence
//===----------------------------------------------------------------------===//

extension Sequence {
  /// Returns an array containing the results of mapping the given closure
  /// over the sequence's elements.
  ///
  /// In this example, `map` is used first to convert the names in the array
  /// to lowercase strings and then to count their characters.
  ///
  ///     let cast = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     let lowercaseNames = cast.map { $0.lowercased() }
  ///     // 'lowercaseNames' == ["vivien", "marlon", "kim", "karl"]
  ///     let letterCounts = cast.map { $0.count }
  ///     // 'letterCounts' == [6, 6, 3, 4]
  ///
  /// - Parameter transform: A mapping closure. `transform` accepts an
  ///   element of this sequence as its parameter and returns a transformed
  ///   value of the same or of a different type.
  /// - Returns: An array containing the transformed elements of this
  ///   sequence.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func map<T>(
    _ transform: (Element) throws -> T
  ) rethrows -> [T] {
    let initialCapacity = underestimatedCount
    var result = ContiguousArray<T>()
    result.reserveCapacity(initialCapacity)

    var iterator = self.makeIterator()

    // Add elements up to the initial capacity without checking for regrowth.
    for _ in 0..<initialCapacity {
      result.append(try transform(iterator.next()!))
    }
    // Add remaining elements, if any.
    while let element = iterator.next() {
      result.append(try transform(element))
    }
    return Array(result)
  }

  /// Returns an array containing, in order, the elements of the sequence
  /// that satisfy the given predicate.
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
  ///   whether the element should be included in the returned array.
  /// - Returns: An array of the elements that `isIncluded` allowed.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> [Element] {
    return try _filter(isIncluded)
  }

  @_transparent
  public func _filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> [Element] {

    var result = ContiguousArray<Element>()

    var iterator = self.makeIterator()

    while let element = iterator.next() {
      if try isIncluded(element) {
        result.append(element)
      }
    }

    return Array(result)
  }

  /// A value less than or equal to the number of elements in the sequence,
  /// calculated nondestructively.
  ///
  /// The default implementation returns 0. If you provide your own
  /// implementation, make sure to compute the value nondestructively.
  ///
  /// - Complexity: O(1), except if the sequence also conforms to `Collection`.
  ///   In this case, see the documentation of `Collection.underestimatedCount`.
  @inlinable
  public var underestimatedCount: Int {
    return 0
  }

  @inlinable
  @inline(__always)
  public func _customContainsEquatableElement(
    _ element: Iterator.Element
  ) -> Bool? {
    return nil
  }

  /// Calls the given closure on each element in the sequence in the same order
  /// as a `for`-`in` loop.
  ///
  /// The two loops in the following example produce the same output:
  ///
  ///     let numberWords = ["one", "two", "three"]
  ///     for word in numberWords {
  ///         print(word)
  ///     }
  ///     // Prints "one"
  ///     // Prints "two"
  ///     // Prints "three"
  ///
  ///     numberWords.forEach { word in
  ///         print(word)
  ///     }
  ///     // Same as above
  ///
  /// Using the `forEach` method is distinct from a `for`-`in` loop in two
  /// important ways:
  ///
  /// 1. You cannot use a `break` or `continue` statement to exit the current
  ///    call of the `body` closure or skip subsequent calls.
  /// 2. Using the `return` statement in the `body` closure will exit only from
  ///    the current call to `body`, not from any outer scope, and won't skip
  ///    subsequent calls.
  ///
  /// - Parameter body: A closure that takes an element of the sequence as a
  ///   parameter.
  @_semantics("sequence.forEach")
  @inlinable
  public func forEach(
    _ body: (Element) throws -> Void
  ) rethrows {
    for element in self {
      try body(element)
    }
  }
}

extension Sequence {
  /// Returns the first element of the sequence that satisfies the given
  /// predicate.
  ///
  /// The following example uses the `first(where:)` method to find the first
  /// negative number in an array of integers:
  ///
  ///     let numbers = [3, 7, 4, -2, 9, -6, 10, 1]
  ///     if let firstNegative = numbers.first(where: { $0 < 0 }) {
  ///         print("The first negative number is \(firstNegative).")
  ///     }
  ///     // Prints "The first negative number is -2."
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence as
  ///   its argument and returns a Boolean value indicating whether the
  ///   element is a match.
  /// - Returns: The first element of the sequence that satisfies `predicate`,
  ///   or `nil` if there is no element that satisfies `predicate`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  public func first(
    where predicate: (Element) throws -> Bool
  ) rethrows -> Element? {
    for element in self {
      if try predicate(element) {
        return element
      }
    }
    return nil
  }
}

extension Sequence where Element: Equatable {
  /// Returns the longest possible subsequences of the sequence, in order,
  /// around elements equal to the given element.
  ///
  /// The resulting array consists of at most `maxSplits + 1` subsequences.
  /// Elements that are used to split the sequence are not returned as part of
  /// any subsequence.
  ///
  /// The following examples show the effects of the `maxSplits` and
  /// `omittingEmptySubsequences` parameters when splitting a string at each
  /// space character (" "). The first use of `split` returns each word that
  /// was originally separated by one or more spaces.
  ///
  ///     let line = "BLANCHE:   I don't want realism. I want magic!"
  ///     print(line.split(separator: " ")
  ///               .map(String.init))
  ///     // Prints "["BLANCHE:", "I", "don\'t", "want", "realism.", "I", "want", "magic!"]"
  ///
  /// The second example passes `1` for the `maxSplits` parameter, so the
  /// original string is split just once, into two new strings.
  ///
  ///     print(line.split(separator: " ", maxSplits: 1)
  ///               .map(String.init))
  ///     // Prints "["BLANCHE:", "  I don\'t want realism. I want magic!"]"
  ///
  /// The final example passes `false` for the `omittingEmptySubsequences`
  /// parameter, so the returned array contains empty strings where spaces
  /// were repeated.
  ///
  ///     print(line.split(separator: " ", omittingEmptySubsequences: false)
  ///               .map(String.init))
  ///     // Prints "["BLANCHE:", "", "", "I", "don\'t", "want", "realism.", "I", "want", "magic!"]"
  ///
  /// - Parameters:
  ///   - separator: The element that should be split upon.
  ///   - maxSplits: The maximum number of times to split the sequence, or one
  ///     less than the number of subsequences to return. If `maxSplits + 1`
  ///     subsequences are returned, the last one is a suffix of the original
  ///     sequence containing the remaining elements. `maxSplits` must be
  ///     greater than or equal to zero. The default value is `Int.max`.
  ///   - omittingEmptySubsequences: If `false`, an empty subsequence is
  ///     returned in the result for each consecutive pair of `separator`
  ///     elements in the sequence and for each instance of `separator` at the
  ///     start or end of the sequence. If `true`, only nonempty subsequences
  ///     are returned. The default value is `true`.
  /// - Returns: An array of subsequences, split from this sequence's elements.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func split(
    separator: Element,
    maxSplits: Int = Int.max,
    omittingEmptySubsequences: Bool = true
  ) -> [ArraySlice<Element>] {
    return split(
      maxSplits: maxSplits,
      omittingEmptySubsequences: omittingEmptySubsequences,
      whereSeparator: { $0 == separator })
  }
}

extension Sequence {

  /// Returns the longest possible subsequences of the sequence, in order, that
  /// don't contain elements satisfying the given predicate. Elements that are
  /// used to split the sequence are not returned as part of any subsequence.
  ///
  /// The following examples show the effects of the `maxSplits` and
  /// `omittingEmptySubsequences` parameters when splitting a string using a
  /// closure that matches spaces. The first use of `split` returns each word
  /// that was originally separated by one or more spaces.
  ///
  ///     let line = "BLANCHE:   I don't want realism. I want magic!"
  ///     print(line.split(whereSeparator: { $0 == " " })
  ///               .map(String.init))
  ///     // Prints "["BLANCHE:", "I", "don\'t", "want", "realism.", "I", "want", "magic!"]"
  ///
  /// The second example passes `1` for the `maxSplits` parameter, so the
  /// original string is split just once, into two new strings.
  ///
  ///     print(
  ///        line.split(maxSplits: 1, whereSeparator: { $0 == " " })
  ///                       .map(String.init))
  ///     // Prints "["BLANCHE:", "  I don\'t want realism. I want magic!"]"
  ///
  /// The final example passes `true` for the `allowEmptySlices` parameter, so
  /// the returned array contains empty strings where spaces were repeated.
  ///
  ///     print(
  ///         line.split(
  ///             omittingEmptySubsequences: false,
  ///             whereSeparator: { $0 == " " }
  ///         ).map(String.init))
  ///     // Prints "["BLANCHE:", "", "", "I", "don\'t", "want", "realism.", "I", "want", "magic!"]"
  ///
  /// - Parameters:
  ///   - maxSplits: The maximum number of times to split the sequence, or one
  ///     less than the number of subsequences to return. If `maxSplits + 1`
  ///     subsequences are returned, the last one is a suffix of the original
  ///     sequence containing the remaining elements. `maxSplits` must be
  ///     greater than or equal to zero. The default value is `Int.max`.
  ///   - omittingEmptySubsequences: If `false`, an empty subsequence is
  ///     returned in the result for each pair of consecutive elements
  ///     satisfying the `isSeparator` predicate and for each element at the
  ///     start or end of the sequence satisfying the `isSeparator` predicate.
  ///     If `true`, only nonempty subsequences are returned. The default
  ///     value is `true`.
  ///   - isSeparator: A closure that returns `true` if its argument should be
  ///     used to split the sequence; otherwise, `false`.
  /// - Returns: An array of subsequences, split from this sequence's elements.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func split(
    maxSplits: Int = Int.max,
    omittingEmptySubsequences: Bool = true,
    whereSeparator isSeparator: (Element) throws -> Bool
  ) rethrows -> [ArraySlice<Element>] {
    _precondition(maxSplits >= 0, "Must take zero or more splits")
    let whole = Array(self)
    return try whole.split(
                  maxSplits: maxSplits, 
                  omittingEmptySubsequences: omittingEmptySubsequences, 
                  whereSeparator: isSeparator)
  }

  /// Returns a subsequence, up to the given maximum length, containing the
  /// final elements of the sequence.
  ///
  /// The sequence must be finite. If the maximum length exceeds the number of
  /// elements in the sequence, the result contains all the elements in the
  /// sequence.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.suffix(2))
  ///     // Prints "[4, 5]"
  ///     print(numbers.suffix(10))
  ///     // Prints "[1, 2, 3, 4, 5]"
  ///
  /// - Parameter maxLength: The maximum number of elements to return. The
  ///   value of `maxLength` must be greater than or equal to zero.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func suffix(_ maxLength: Int) -> [Element] {
    _precondition(maxLength >= 0, "Can't take a suffix of negative length from a sequence")
    guard maxLength != 0 else { return [] }

    // FIXME: <rdar://problem/21885650> Create reusable RingBuffer<T>
    // Put incoming elements into a ring buffer to save space. Once all
    // elements are consumed, reorder the ring buffer into a copy and return it.
    // This saves memory for sequences particularly longer than `maxLength`.
    var ringBuffer = ContiguousArray<Element>()
    ringBuffer.reserveCapacity(Swift.min(maxLength, underestimatedCount))

    var i = 0

    for element in self {
      if ringBuffer.count < maxLength {
        ringBuffer.append(element)
      } else {
        ringBuffer[i] = element
        i += 1
        if i >= maxLength {
          i = 0
        }
      }
    }

    if i != ringBuffer.startIndex {
      var rotated = ContiguousArray<Element>()
      rotated.reserveCapacity(ringBuffer.count)
      rotated += ringBuffer[i..<ringBuffer.endIndex]
      rotated += ringBuffer[0..<i]
      return Array(rotated)
    } else {
      return Array(ringBuffer)
    }
  }

  /// Returns a sequence containing all but the given number of initial
  /// elements.
  ///
  /// If the number of elements to drop exceeds the number of elements in
  /// the sequence, the result is an empty sequence.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.dropFirst(2))
  ///     // Prints "[3, 4, 5]"
  ///     print(numbers.dropFirst(10))
  ///     // Prints "[]"
  ///
  /// - Parameter k: The number of elements to drop from the beginning of
  ///   the sequence. `k` must be greater than or equal to zero.
  /// - Returns: A sequence starting after the specified number of
  ///   elements.
  ///
  /// - Complexity: O(1), with O(*k*) deferred to each iteration of the result,
  ///   where *k* is the number of elements to drop from the beginning of
  ///   the sequence.
  @inlinable
  @_eagerMove
  public __consuming func dropFirst(_ k: Int = 1) -> DropFirstSequence<Self> {
    return DropFirstSequence(self, dropping: k)
  }

  /// Returns a sequence containing all but the given number of final
  /// elements.
  ///
  /// The sequence must be finite. If the number of elements to drop exceeds
  /// the number of elements in the sequence, the result is an empty
  /// sequence.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.dropLast(2))
  ///     // Prints "[1, 2, 3]"
  ///     print(numbers.dropLast(10))
  ///     // Prints "[]"
  ///
  /// - Parameter n: The number of elements to drop off the end of the
  ///   sequence. `n` must be greater than or equal to zero.
  /// - Returns: A sequence leaving off the specified number of elements.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func dropLast(_ k: Int = 1) -> [Element] {
    _precondition(k >= 0, "Can't drop a negative number of elements from a sequence")
    guard k != 0 else { return Array(self) }

    // FIXME: <rdar://problem/21885650> Create reusable RingBuffer<T>
    // Put incoming elements from this sequence in a holding tank, a ring buffer
    // of size <= k. If more elements keep coming in, pull them out of the
    // holding tank into the result, an `Array`. This saves
    // `k` * sizeof(Element) of memory, because slices keep the entire
    // memory of an `Array` alive.
    var result = ContiguousArray<Element>()
    var ringBuffer = ContiguousArray<Element>()
    var i = ringBuffer.startIndex

    for element in self {
      if ringBuffer.count < k {
        ringBuffer.append(element)
      } else {
        result.append(ringBuffer[i])
        ringBuffer[i] = element
        i += 1
        if i >= k {
          i = 0
        }
      }
    }
    return Array(result)
  }

  /// Returns a sequence by skipping the initial, consecutive elements that
  /// satisfy the given predicate.
  ///
  /// The following example uses the `drop(while:)` method to skip over the
  /// positive numbers at the beginning of the `numbers` array. The result
  /// begins with the first element of `numbers` that does not satisfy
  /// `predicate`.
  ///
  ///     let numbers = [3, 7, 4, -2, 9, -6, 10, 1]
  ///     let startingWithNegative = numbers.drop(while: { $0 > 0 })
  ///     // startingWithNegative == [-2, 9, -6, 10, 1]
  ///
  /// If `predicate` matches every element in the sequence, the result is an
  /// empty sequence.
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence as
  ///   its argument and returns a Boolean value indicating whether the
  ///   element should be included in the result.
  /// - Returns: A sequence starting after the initial, consecutive elements
  ///   that satisfy `predicate`.
  ///
  /// - Complexity: O(*k*), where *k* is the number of elements to drop from
  ///   the beginning of the sequence.
  @inlinable
  @_eagerMove
  public __consuming func drop(
    while predicate: (Element) throws -> Bool
  ) rethrows -> DropWhileSequence<Self> {
    return try DropWhileSequence(self, predicate: predicate)
  }

  /// Returns a sequence, up to the specified maximum length, containing the
  /// initial elements of the sequence.
  ///
  /// If the maximum length exceeds the number of elements in the sequence,
  /// the result contains all the elements in the sequence.
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     print(numbers.prefix(2))
  ///     // Prints "[1, 2]"
  ///     print(numbers.prefix(10))
  ///     // Prints "[1, 2, 3, 4, 5]"
  ///
  /// - Parameter maxLength: The maximum number of elements to return. The
  ///   value of `maxLength` must be greater than or equal to zero.
  /// - Returns: A sequence starting at the beginning of this sequence
  ///   with at most `maxLength` elements.
  ///
  /// - Complexity: O(1)
  @inlinable
  @_eagerMove
  public __consuming func prefix(_ maxLength: Int) -> PrefixSequence<Self> {
    return PrefixSequence(self, maxLength: maxLength)
  }

  /// Returns a sequence containing the initial, consecutive elements that
  /// satisfy the given predicate.
  ///
  /// The following example uses the `prefix(while:)` method to find the
  /// positive numbers at the beginning of the `numbers` array. Every element
  /// of `numbers` up to, but not including, the first negative value is
  /// included in the result.
  ///
  ///     let numbers = [3, 7, 4, -2, 9, -6, 10, 1]
  ///     let positivePrefix = numbers.prefix(while: { $0 > 0 })
  ///     // positivePrefix == [3, 7, 4]
  ///
  /// If `predicate` matches every element in the sequence, the resulting
  /// sequence contains every element of the sequence.
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence as
  ///   its argument and returns a Boolean value indicating whether the
  ///   element should be included in the result.
  /// - Returns: A sequence of the initial, consecutive elements that
  ///   satisfy `predicate`.
  ///
  /// - Complexity: O(*k*), where *k* is the length of the result.
  @inlinable
  @_eagerMove
  public __consuming func prefix(
    while predicate: (Element) throws -> Bool
  ) rethrows -> [Element] {
    var result = ContiguousArray<Element>()

    for element in self {
      guard try predicate(element) else {
        break
      }
      result.append(element)
    }
    return Array(result)
  }
}

extension Sequence {
  /// Copy `self` into an unsafe buffer, initializing its memory.
  ///
  /// The default implementation simply iterates over the elements of the
  /// sequence, initializing the buffer one item at a time.
  ///
  /// For sequences whose elements are stored in contiguous chunks of memory,
  /// it may be more efficient to copy them in bulk, using the
  /// `UnsafeMutablePointer.initialize(from:count:)` method.
  ///
  /// - Parameter ptr: An unsafe buffer addressing uninitialized memory. The
  ///    buffer must be of sufficient size to accommodate
  ///    `source.underestimatedCount` elements. (Some implementations trap
  ///    if given a buffer that's smaller than this.)
  ///
  /// - Returns: `(it, c)`, where `c` is the number of elements copied into the
  ///    buffer, and `it` is a partially consumed iterator that can be used to
  ///    retrieve elements that did not fit into the buffer (if any). (This can
  ///    only happen if `underestimatedCount` turned out to be an actual
  ///    underestimate, and the buffer did not contain enough space to hold the
  ///    entire sequence.)
  ///
  ///    On return, the memory region in `buffer[0 ..< c]` is initialized to
  ///    the first `c` elements in the sequence.
  @inlinable
  @_eagerMove
  public __consuming func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    return _copySequenceContents(initializing: buffer)
  }

  @_eagerMove
  @_alwaysEmitIntoClient
  internal __consuming func _copySequenceContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    @_eagerMove var it = self.makeIterator()
    guard var ptr = buffer.baseAddress else { return (it, buffer.startIndex) }
    for idx in buffer.indices {
      guard let x = it.next() else {
        return (it, idx)
      }
      ptr.initialize(to: x)
      ptr += 1
    }
    return (it, buffer.endIndex)
  }
    
  @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return nil
  }  
}

// FIXME(ABI)#182
// Pending <rdar://problem/14011860> and <rdar://problem/14396120>,
// pass an IteratorProtocol through IteratorSequence to give it "Sequence-ness"
/// A sequence built around an iterator of type `Base`.
///
/// Useful mostly to recover the ability to use `for`...`in`,
/// given just an iterator `i`:
///
///     for x in IteratorSequence(i) { ... }
@frozen
public struct IteratorSequence<Base: IteratorProtocol> {
  @usableFromInline
  internal var _base: Base

  /// Creates an instance whose iterator is a copy of `base`.
  @inlinable
  public init(_ base: Base) {
    _base = base
  }
}

extension IteratorSequence: IteratorProtocol, Sequence {

  #if $NoncopyableGenerics
    public typealias Element = Base.Element
  #endif

  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  ///
  /// - Precondition: `next()` has not been applied to a copy of `self`
  ///   since the copy was made.
  @inlinable
  public mutating func next() -> Base.Element? {
    return _base.next()
  }
}

extension IteratorSequence: Sendable where Base: Sendable { }

/* FIXME: ideally for compatibility we would declare
extension Sequence {
  @available(swift, deprecated: 5, message: "")
  public typealias SubSequence = AnySequence<Element>
}
*/

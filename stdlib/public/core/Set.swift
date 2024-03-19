//===----------------------------------------------------------------------===//
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

/// An unordered collection of unique elements.
///
/// You use a set instead of an array when you need to test efficiently for
/// membership and you aren't concerned with the order of the elements in the
/// collection, or when you need to ensure that each element appears only once
/// in a collection.
///
/// You can create a set with any element type that conforms to the `Hashable`
/// protocol. By default, most types in the standard library are hashable,
/// including strings, numeric and Boolean types, enumeration cases without
/// associated values, and even sets themselves.
///
/// Swift makes it as easy to create a new set as to create a new array. Simply
/// assign an array literal to a variable or constant with the `Set` type
/// specified.
///
///     let ingredients: Set = ["cocoa beans", "sugar", "cocoa butter", "salt"]
///     if ingredients.contains("sugar") {
///         print("No thanks, too sweet.")
///     }
///     // Prints "No thanks, too sweet."
///
/// Set Operations
/// ==============
///
/// Sets provide a suite of mathematical set operations. For example, you can
/// efficiently test a set for membership of an element or check its
/// intersection with another set:
///
/// - Use the `contains(_:)` method to test whether a set contains a specific
///   element.
/// - Use the "equal to" operator (`==`) to test whether two sets contain the
///   same elements.
/// - Use the `isSubset(of:)` method to test whether a set contains all the
///   elements of another set or sequence.
/// - Use the `isSuperset(of:)` method to test whether all elements of a set
///   are contained in another set or sequence.
/// - Use the `isStrictSubset(of:)` and `isStrictSuperset(of:)` methods to test
///   whether a set is a subset or superset of, but not equal to, another set.
/// - Use the `isDisjoint(with:)` method to test whether a set has any elements
///   in common with another set.
///
/// You can also combine, exclude, or subtract the elements of two sets:
///
/// - Use the `union(_:)` method to create a new set with the elements of a set
///   and another set or sequence.
/// - Use the `intersection(_:)` method to create a new set with only the
///   elements common to a set and another set or sequence.
/// - Use the `symmetricDifference(_:)` method to create a new set with the
///   elements that are in either a set or another set or sequence, but not in
///   both.
/// - Use the `subtracting(_:)` method to create a new set with the elements of
///   a set that are not also in another set or sequence.
///
/// You can modify a set in place by using these methods' mutating
/// counterparts: `formUnion(_:)`, `formIntersection(_:)`,
/// `formSymmetricDifference(_:)`, and `subtract(_:)`.
///
/// Set operations are not limited to use with other sets. Instead, you can
/// perform set operations with another set, an array, or any other sequence
/// type.
///
///     var primes: Set = [2, 3, 5, 7]
///
///     // Tests whether primes is a subset of a Range<Int>
///     print(primes.isSubset(of: 0..<10))
///     // Prints "true"
///
///     // Performs an intersection with an Array<Int>
///     let favoriteNumbers = [5, 7, 15, 21]
///     print(primes.intersection(favoriteNumbers))
///     // Prints "[5, 7]"
///
/// Sequence and Collection Operations
/// ==================================
///
/// In addition to the `Set` type's set operations, you can use any nonmutating
/// sequence or collection methods with a set.
///
///     if primes.isEmpty {
///         print("No primes!")
///     } else {
///         print("We have \(primes.count) primes.")
///     }
///     // Prints "We have 4 primes."
///
///     let primesSum = primes.reduce(0, +)
///     // 'primesSum' == 17
///
///     let primeStrings = primes.sorted().map(String.init)
///     // 'primeStrings' == ["2", "3", "5", "7"]
///
/// You can iterate through a set's unordered elements with a `for`-`in` loop.
///
///     for number in primes {
///         print(number)
///     }
///     // Prints "5"
///     // Prints "7"
///     // Prints "2"
///     // Prints "3"
///
/// Many sequence and collection operations return an array or a type-erasing
/// collection wrapper instead of a set. To restore efficient set operations,
/// create a new set from the result.
///
///     let primesStrings = primes.map(String.init)
///     // 'primesStrings' is of type Array<String>
///     let primesStringsSet = Set(primes.map(String.init))
///     // 'primesStringsSet' is of type Set<String>
///
/// Bridging Between Set and NSSet
/// ==============================
///
/// You can bridge between `Set` and `NSSet` using the `as` operator. For
/// bridging to be possible, the `Element` type of a set must be a class, an
/// `@objc` protocol (a protocol imported from Objective-C or marked with the
/// `@objc` attribute), or a type that bridges to a Foundation type.
///
/// Bridging from `Set` to `NSSet` always takes O(1) time and space. When the
/// set's `Element` type is neither a class nor an `@objc` protocol, any
/// required bridging of elements occurs at the first access of each element,
/// so the first operation that uses the contents of the set (for example, a
/// membership test) can take O(*n*).
///
/// Bridging from `NSSet` to `Set` first calls the `copy(with:)` method
/// (`- copyWithZone:` in Objective-C) on the set to get an immutable copy and
/// then performs additional Swift bookkeeping work that takes O(1) time. For
/// instances of `NSSet` that are already immutable, `copy(with:)` returns the
/// same set in constant time; otherwise, the copying performance is
/// unspecified. The instances of `NSSet` and `Set` share buffer using the
/// same copy-on-write optimization that is used when two instances of `Set`
/// share buffer.
@frozen
@_eagerMove
public struct Set<Element: Hashable> {
  @usableFromInline
  internal var _variant: _Variant

  /// Creates an empty set with preallocated space for at least the specified
  /// number of elements.
  ///
  /// Use this initializer to avoid intermediate reallocations of a set's
  /// storage buffer when you know how many elements you'll insert into the set
  /// after creation.
  ///
  /// - Parameter minimumCapacity: The minimum number of elements that the
  ///   newly created set should be able to store without reallocating its
  ///   storage buffer.
  public // FIXME(reserveCapacity): Should be inlinable
  init(minimumCapacity: Int) {
    _variant = _Variant(native: _NativeSet(capacity: minimumCapacity))
  }

  /// Private initializer.
  @inlinable
  internal init(_native: __owned _NativeSet<Element>) {
    _variant = _Variant(native: _native)
  }

#if _runtime(_ObjC)
  @inlinable
  internal init(_cocoa: __owned __CocoaSet) {
    _variant = _Variant(cocoa: _cocoa)
  }

  /// Private initializer used for bridging.
  ///
  /// Only use this initializer when both conditions are true:
  ///
  /// * it is statically known that the given `NSSet` is immutable;
  /// * `Element` is bridged verbatim to Objective-C (i.e.,
  ///   is a reference type).
  @inlinable
  public // SPI(Foundation)
  init(_immutableCocoaSet: __owned AnyObject) {
    _internalInvariant(_isBridgedVerbatimToObjectiveC(Element.self),
      "Set can be backed by NSSet _variant only when the member type can be bridged verbatim to Objective-C")
    self.init(_cocoa: __CocoaSet(_immutableCocoaSet))
  }
#endif
}

extension Set: ExpressibleByArrayLiteral {
  /// Creates a set containing the elements of the given array literal.
  ///
  /// Do not call this initializer directly. It is used by the compiler when
  /// you use an array literal. Instead, create a new set using an array
  /// literal as its value by enclosing a comma-separated list of values in
  /// square brackets. You can use an array literal anywhere a set is expected
  /// by the type context.
  ///
  /// Here, a set of strings is created from an array literal holding only
  /// strings.
  ///
  ///     let ingredients: Set = ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///     if ingredients.isSuperset(of: ["sugar", "salt"]) {
  ///         print("Whatever it is, it's bound to be delicious!")
  ///     }
  ///     // Prints "Whatever it is, it's bound to be delicious!"
  ///
  /// - Parameter elements: A variadic list of elements of the new set.
  @inlinable
  @inline(__always)
  public init(arrayLiteral elements: Element...) {
    if elements.isEmpty {
      self.init()
      return
    }
    self.init(_nonEmptyArrayLiteral: elements)
  }

  @_alwaysEmitIntoClient
  internal init(_nonEmptyArrayLiteral elements: [Element]) {
    let native = _NativeSet<Element>(capacity: elements.count)
    for element in elements {
      let (bucket, found) = native.find(element)
      if found {
        // FIXME: Shouldn't this trap?
        continue
      }
      native._unsafeInsertNew(element, at: bucket)
    }
    self.init(_native: native)
  }
}

extension Set: Sequence {
  /// Returns an iterator over the members of the set.
  @inlinable
  @inline(__always)
  public __consuming func makeIterator() -> Iterator {
    return _variant.makeIterator()
  }

  /// Returns a Boolean value that indicates whether the given element exists
  /// in the set.
  ///
  /// This example uses the `contains(_:)` method to test whether an integer is
  /// a member of a set of prime numbers.
  ///
  ///     let primes: Set = [2, 3, 5, 7]
  ///     let x = 5
  ///     if primes.contains(x) {
  ///         print("\(x) is prime!")
  ///     } else {
  ///         print("\(x). Not prime.")
  ///     }
  ///     // Prints "5 is prime!"
  ///
  /// - Parameter member: An element to look for in the set.
  /// - Returns: `true` if `member` exists in the set; otherwise, `false`.
  ///
  /// - Complexity: O(1)
  @inlinable
  public func contains(_ member: Element) -> Bool {
    return _variant.contains(member)
  }

  @inlinable
  @inline(__always)
  public func _customContainsEquatableElement(_ member: Element) -> Bool? {
    return contains(member)
  }
}

// This is not quite Sequence.filter, because that returns [Element], not Self
// (RangeReplaceableCollection.filter returns Self, but Set isn't an RRC)
extension Set {
  /// Returns a new set containing the elements of the set that satisfy the
  /// given predicate.
  ///
  /// In this example, `filter(_:)` is used to include only names shorter than
  /// five characters.
  ///
  ///     let cast: Set = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     let shortNames = cast.filter { $0.count < 5 }
  ///
  ///     shortNames.isSubset(of: cast)
  ///     // true
  ///     shortNames.contains("Vivien")
  ///     // false
  ///
  /// - Parameter isIncluded: A closure that takes an element as its argument
  ///   and returns a Boolean value indicating whether the element should be
  ///   included in the returned set.
  /// - Returns: A set of the elements that `isIncluded` allows.
  @inlinable
  @available(swift, introduced: 4.0)
  public __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> Set {
    return try Set(_native: _variant.filter(isIncluded))
  }
}

extension Set: Collection {
  /// The starting position for iterating members of the set.
  ///
  /// If the set is empty, `startIndex` is equal to `endIndex`.
  @inlinable
  public var startIndex: Index {
    return _variant.startIndex
  }

  /// The "past the end" position for the set---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// If the set is empty, `endIndex` is equal to `startIndex`.
  @inlinable
  public var endIndex: Index {
    return _variant.endIndex
  }

  /// Accesses the member at the given position.
  @inlinable
  public subscript(position: Index) -> Element {
    // FIXME(accessors): Provide a _read
    get {
      return _variant.element(at: position)
    }
  }

  @inlinable
  public func index(after i: Index) -> Index {
    return _variant.index(after: i)
  }

  @inlinable
  public func formIndex(after i: inout Index) {
    _variant.formIndex(after: &i)
  }

  // APINAMING: complexity docs are broadly missing in this file.

  /// Returns the index of the given element in the set, or `nil` if the
  /// element is not a member of the set.
  ///
  /// - Parameter member: An element to search for in the set.
  /// - Returns: The index of `member` if it exists in the set; otherwise,
  ///   `nil`.
  ///
  /// - Complexity: O(1)
  @inlinable
  public func firstIndex(of member: Element) -> Index? {
    return _variant.index(for: member)
  }

  @inlinable
  @inline(__always)
  public func _customIndexOfEquatableElement(
     _ member: Element
    ) -> Index?? {
    return Optional(firstIndex(of: member))
  }

  @inlinable
  @inline(__always)
  public func _customLastIndexOfEquatableElement(
     _ member: Element
    ) -> Index?? {
    // The first and last elements are the same because each element is unique.
    return _customIndexOfEquatableElement(member)
  }

  /// The number of elements in the set.
  ///
  /// - Complexity: O(1).
  @inlinable
  public var count: Int {
    return _variant.count
  }

  /// A Boolean value that indicates whether the set is empty.
  @inlinable
  public var isEmpty: Bool {
    return count == 0
  }
}

// FIXME: rdar://problem/23549059 (Optimize == for Set)
// Look into initially trying to compare the two sets by directly comparing the
// contents of both buffers in order. If they happen to have the exact same
// ordering we can get the `true` response without ever hashing. If the two
// buffers' contents differ at all then we have to fall back to hashing the
// rest of the elements (but we don't need to hash any prefix that did match).
extension Set: Equatable {
  /// Returns a Boolean value indicating whether two sets have equal elements.
  ///
  /// - Parameters:
  ///   - lhs: A set.
  ///   - rhs: Another set.
  /// - Returns: `true` if the `lhs` and `rhs` have the same elements; otherwise,
  ///   `false`.
  @inlinable
  public static func == (lhs: Set<Element>, rhs: Set<Element>) -> Bool {
#if _runtime(_ObjC)
    switch (lhs._variant.isNative, rhs._variant.isNative) {
    case (true, true):
      return lhs._variant.asNative.isEqual(to: rhs._variant.asNative)
    case (false, false):
      return lhs._variant.asCocoa.isEqual(to: rhs._variant.asCocoa)
    case (true, false):
      return lhs._variant.asNative.isEqual(to: rhs._variant.asCocoa)
    case (false, true):
      return rhs._variant.asNative.isEqual(to: lhs._variant.asCocoa)
    }
#else
    return lhs._variant.asNative.isEqual(to: rhs._variant.asNative)
#endif
  }
}

extension Set: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    // FIXME(ABI)#177: <rdar://problem/18915294> Cache Set<T> hashValue

    // Generate a seed from a snapshot of the hasher.  This makes members' hash
    // values depend on the state of the hasher, which improves hashing
    // quality. (E.g., it makes it possible to resolve collisions by passing in
    // a different hasher.)
    var copy = hasher
    let seed = copy._finalize()

    var hash = 0
    for member in self {
      hash ^= member._rawHashValue(seed: seed)
    }
    hasher.combine(hash)
  }
}

@_unavailableInEmbedded
extension Set: _HasCustomAnyHashableRepresentation {
  public __consuming func _toCustomAnyHashable() -> AnyHashable? {
    return AnyHashable(_box: _SetAnyHashableBox(self))
  }
}

@_unavailableInEmbedded
internal struct _SetAnyHashableBox<Element: Hashable>: _AnyHashableBox {
  internal let _value: Set<Element>
  internal let _canonical: Set<AnyHashable>

  internal init(_ value: __owned Set<Element>) {
    self._value = value
    self._canonical = value as Set<AnyHashable>
  }

  internal var _base: Any {
    return _value
  }

  internal var _canonicalBox: _AnyHashableBox {
    return _SetAnyHashableBox<AnyHashable>(_canonical)
  }

  internal func _isEqual(to other: _AnyHashableBox) -> Bool? {
    guard let other = other as? _SetAnyHashableBox<AnyHashable> else {
      return nil
    }
    return _canonical == other._value
  }

  internal var _hashValue: Int {
    return _canonical.hashValue
  }

  internal func _hash(into hasher: inout Hasher) {
    _canonical.hash(into: &hasher)
  }

  internal func _rawHashValue(_seed: Int) -> Int {
    return _canonical._rawHashValue(seed: _seed)
  }

  internal func _unbox<T: Hashable>() -> T? {
    return _value as? T
  }

  internal func _downCastConditional<T>(
    into result: UnsafeMutablePointer<T>
  ) -> Bool {
    guard let value = _value as? T else { return false }
    result.initialize(to: value)
    return true
  }
}

extension Set: SetAlgebra {

  /// Inserts the given element in the set if it is not already present.
  ///
  /// If an element equal to `newMember` is already contained in the set, this
  /// method has no effect. In the following example, a new element is
  /// inserted into `classDays`, a set of days of the week. When an existing
  /// element is inserted, the `classDays` set does not change.
  ///
  ///     enum DayOfTheWeek: Int {
  ///         case sunday, monday, tuesday, wednesday, thursday,
  ///             friday, saturday
  ///     }
  ///
  ///     var classDays: Set<DayOfTheWeek> = [.wednesday, .friday]
  ///     print(classDays.insert(.monday))
  ///     // Prints "(inserted: true, memberAfterInsert: DayOfTheWeek.monday)"
  ///     print(classDays)
  ///     // Prints "[DayOfTheWeek.friday, DayOfTheWeek.wednesday, DayOfTheWeek.monday]"
  ///
  ///     print(classDays.insert(.friday))
  ///     // Prints "(inserted: false, memberAfterInsert: DayOfTheWeek.friday)"
  ///     print(classDays)
  ///     // Prints "[DayOfTheWeek.friday, DayOfTheWeek.wednesday, DayOfTheWeek.monday]"
  ///
  /// - Parameter newMember: An element to insert into the set.
  /// - Returns: `(true, newMember)` if `newMember` was not contained in the
  ///   set. If an element equal to `newMember` was already contained in the
  ///   set, the method returns `(false, oldMember)`, where `oldMember` is the
  ///   element that was equal to `newMember`. In some cases, `oldMember` may
  ///   be distinguishable from `newMember` by identity comparison or some
  ///   other means.
  @inlinable
  @discardableResult
  public mutating func insert(
    _ newMember: __owned Element
  ) -> (inserted: Bool, memberAfterInsert: Element) {
    return _variant.insert(newMember)
  }

  /// Inserts the given element into the set unconditionally.
  ///
  /// If an element equal to `newMember` is already contained in the set,
  /// `newMember` replaces the existing element. In this example, an existing
  /// element is inserted into `classDays`, a set of days of the week.
  ///
  ///     enum DayOfTheWeek: Int {
  ///         case sunday, monday, tuesday, wednesday, thursday,
  ///             friday, saturday
  ///     }
  ///
  ///     var classDays: Set<DayOfTheWeek> = [.monday, .wednesday, .friday]
  ///     print(classDays.update(with: .monday))
  ///     // Prints "Optional(DayOfTheWeek.monday)"
  ///
  /// - Parameter newMember: An element to insert into the set.
  /// - Returns: An element equal to `newMember` if the set already contained
  ///   such a member; otherwise, `nil`. In some cases, the returned element
  ///   may be distinguishable from `newMember` by identity comparison or some
  ///   other means.
  @inlinable
  @discardableResult
  public mutating func update(with newMember: __owned Element) -> Element? {
    return _variant.update(with: newMember)
  }

  /// Removes the specified element from the set.
  ///
  /// This example removes the element `"sugar"` from a set of ingredients.
  ///
  ///     var ingredients: Set = ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///     let toRemove = "sugar"
  ///     if let removed = ingredients.remove(toRemove) {
  ///         print("The recipe is now \(removed)-free.")
  ///     }
  ///     // Prints "The recipe is now sugar-free."
  ///
  /// - Parameter member: The element to remove from the set.
  /// - Returns: The value of the `member` parameter if it was a member of the
  ///   set; otherwise, `nil`.
  @inlinable
  @discardableResult
  public mutating func remove(_ member: Element) -> Element? {
    return _variant.remove(member)
  }

  /// Removes the element at the given index of the set.
  ///
  /// - Parameter position: The index of the member to remove. `position` must
  ///   be a valid index of the set, and must not be equal to the set's end
  ///   index.
  /// - Returns: The element that was removed from the set.
  @inlinable
  @discardableResult
  public mutating func remove(at position: Index) -> Element {
    return _variant.remove(at: position)
  }

  /// Removes all members from the set.
  ///
  /// - Parameter keepingCapacity: If `true`, the set's buffer capacity is
  ///   preserved; if `false`, the underlying buffer is released. The
  ///   default is `false`.
  @inlinable
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    _variant.removeAll(keepingCapacity: keepCapacity)
  }

  /// Removes the first element of the set.
  ///
  /// Because a set is not an ordered collection, the "first" element may not
  /// be the first element that was added to the set. The set must not be
  /// empty.
  ///
  /// - Complexity: Amortized O(1) if the set does not wrap a bridged `NSSet`.
  ///   If the set wraps a bridged `NSSet`, the performance is unspecified.
  ///
  /// - Returns: A member of the set.
  @inlinable
  @discardableResult
  public mutating func removeFirst() -> Element {
    _precondition(!isEmpty, "Can't removeFirst from an empty Set")
    return remove(at: startIndex)
  }

  //
  // APIs below this comment should be implemented strictly in terms of
  // *public* APIs above.  `_variant` should not be accessed directly.
  //
  // This separates concerns for testing.  Tests for the following APIs need
  // not to concern themselves with testing correctness of behavior of
  // underlying buffer (and different variants of it), only correctness of the
  // API itself.
  //

  /// Creates an empty set.
  ///
  /// This is equivalent to initializing with an empty array literal. For
  /// example:
  ///
  ///     var emptySet = Set<Int>()
  ///     print(emptySet.isEmpty)
  ///     // Prints "true"
  ///
  ///     emptySet = []
  ///     print(emptySet.isEmpty)
  ///     // Prints "true"
  @inlinable
  public init() {
    self = Set<Element>(_native: _NativeSet())
  }

  /// Creates a new set from a finite sequence of items.
  ///
  /// Use this initializer to create a new set from an existing sequence, for
  /// example, an array or a range.
  ///
  ///     let validIndices = Set(0..<7).subtracting([2, 4, 5])
  ///     print(validIndices)
  ///     // Prints "[6, 0, 1, 3]"
  ///
  /// This initializer can also be used to restore set methods after performing
  /// sequence operations such as `filter(_:)` or `map(_:)` on a set. For
  /// example, after filtering a set of prime numbers to remove any below 10,
  /// you can create a new set by using this initializer.
  ///
  ///     let primes: Set = [2, 3, 5, 7, 11, 13, 17, 19, 23]
  ///     let laterPrimes = Set(primes.lazy.filter { $0 > 10 })
  ///     print(laterPrimes)
  ///     // Prints "[17, 19, 23, 11, 13]"
  ///
  /// - Parameter sequence: The elements to use as members of the new set.
  @inlinable
  public init<Source: Sequence>(_ sequence: __owned Source)
  where Source.Element == Element {
    if let s = _specialize(sequence, for: Set<Element>.self) {
      // If this sequence is actually a `Set`, then we can quickly
      // adopt its storage and let COW handle uniquing only if necessary.
      self = s
    } else {
      self.init(minimumCapacity: sequence.underestimatedCount)
      for item in sequence {
        insert(item)
      }
    }
  }

  /// Returns a Boolean value that indicates whether the set is a subset of the
  /// given sequence.
  ///
  /// Set *A* is a subset of another set *B* if every member of *A* is also a
  /// member of *B*.
  ///
  ///     let employees = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isSubset(of: employees))
  ///     // Prints "true"
  ///
  /// - Parameter possibleSuperset: A sequence of elements. `possibleSuperset`
  ///   must be finite.
  /// - Returns: `true` if the set is a subset of `possibleSuperset`;
  ///   otherwise, `false`.
  @inlinable
  public func isSubset<S: Sequence>(of possibleSuperset: S) -> Bool
  where S.Element == Element {
    guard !isEmpty else { return true }
    if self.count == 1 { return possibleSuperset.contains(self.first!) }
    if let s = _specialize(possibleSuperset, for: Set<Element>.self) {
      return isSubset(of: s)
    }
    return _variant.convertedToNative.isSubset(of: possibleSuperset)
  }

  /// Returns a Boolean value that indicates whether the set is a strict subset
  /// of the given sequence.
  ///
  /// Set *A* is a strict subset of another set *B* if every member of *A* is
  /// also a member of *B* and *B* contains at least one element that is not a
  /// member of *A*.
  ///
  ///     let employees = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isStrictSubset(of: employees))
  ///     // Prints "true"
  ///
  ///     // A set is never a strict subset of itself:
  ///     print(attendees.isStrictSubset(of: attendees))
  ///     // Prints "false"
  ///
  /// - Parameter possibleStrictSuperset: A sequence of elements.
  ///   `possibleStrictSuperset` must be finite.
  /// - Returns: `true` is the set is strict subset of
  ///   `possibleStrictSuperset`; otherwise, `false`.
  @inlinable
  public func isStrictSubset<S: Sequence>(of possibleStrictSuperset: S) -> Bool
  where S.Element == Element {
    if let s = _specialize(possibleStrictSuperset, for: Set<Element>.self) {
      return isStrictSubset(of: s)
    }
    return _variant.convertedToNative.isStrictSubset(of: possibleStrictSuperset)
  }

  /// Returns a Boolean value that indicates whether the set is a superset of
  /// the given sequence.
  ///
  /// Set *A* is a superset of another set *B* if every member of *B* is also a
  /// member of *A*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees = ["Alicia", "Bethany", "Diana"]
  ///     print(employees.isSuperset(of: attendees))
  ///     // Prints "true"
  ///
  /// - Parameter possibleSubset: A sequence of elements. `possibleSubset` must
  ///   be finite.
  /// - Returns: `true` if the set is a superset of `possibleSubset`;
  ///   otherwise, `false`.
  @inlinable
  public func isSuperset<S: Sequence>(of possibleSubset: __owned S) -> Bool
  where S.Element == Element {
    if let s = _specialize(possibleSubset, for: Set<Element>.self) {
      return isSuperset(of: s)
    }
    for member in possibleSubset {
      if !contains(member) {
        return false
      }
    }
    return true
  }

  /// Returns a Boolean value that indicates whether the set is a strict
  /// superset of the given sequence.
  ///
  /// Set *A* is a strict superset of another set *B* if every member of *B* is
  /// also a member of *A* and *A* contains at least one element that is *not*
  /// a member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees = ["Alicia", "Bethany", "Diana"]
  ///     print(employees.isStrictSuperset(of: attendees))
  ///     // Prints "true"
  ///     print(employees.isStrictSuperset(of: employees))
  ///     // Prints "false"
  ///
  /// - Parameter possibleStrictSubset: A sequence of elements.
  ///   `possibleStrictSubset` must be finite.
  /// - Returns: `true` if the set is a strict superset of
  ///   `possibleStrictSubset`; otherwise, `false`.
  @inlinable
  public func isStrictSuperset<S: Sequence>(of possibleStrictSubset: S) -> Bool
  where S.Element == Element {
    if isEmpty { return false }
    if let s = _specialize(possibleStrictSubset, for: Set<Element>.self) {
      return isStrictSuperset(of: s)
    }
    return _variant.convertedToNative.isStrictSuperset(of: possibleStrictSubset)
  }

  /// Returns a Boolean value that indicates whether the set has no members in
  /// common with the given sequence.
  ///
  /// In the following example, the `employees` set is disjoint with the
  /// elements of the `visitors` array because no name appears in both.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let visitors = ["Marcia", "Nathaniel", "Olivia"]
  ///     print(employees.isDisjoint(with: visitors))
  ///     // Prints "true"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  /// - Returns: `true` if the set has no elements in common with `other`;
  ///   otherwise, `false`.
  @inlinable
  public func isDisjoint<S: Sequence>(with other: S) -> Bool
  where S.Element == Element {
    if let s = _specialize(other, for: Set<Element>.self) {
      return isDisjoint(with: s)
    }
    return _isDisjoint(with: other)
  }

  /// Returns a new set with the elements of both this set and the given
  /// sequence.
  ///
  /// In the following example, the `attendeesAndVisitors` set is made up
  /// of the elements of the `attendees` set and the `visitors` array:
  ///
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     let visitors = ["Marcia", "Nathaniel"]
  ///     let attendeesAndVisitors = attendees.union(visitors)
  ///     print(attendeesAndVisitors)
  ///     // Prints "["Diana", "Nathaniel", "Bethany", "Alicia", "Marcia"]"
  ///
  /// If the set already contains one or more elements that are also in
  /// `other`, the existing members are kept. If `other` contains multiple
  /// instances of equivalent elements, only the first instance is kept.
  ///
  ///     let initialIndices = Set(0..<5)
  ///     let expandedIndices = initialIndices.union([2, 3, 6, 6, 7, 7])
  ///     print(expandedIndices)
  ///     // Prints "[2, 4, 6, 7, 0, 1, 3]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  /// - Returns: A new set with the unique elements of this set and `other`.
  @inlinable
  public __consuming func union<S: Sequence>(_ other: __owned S) -> Set<Element>
  where S.Element == Element {
    var newSet = self
    newSet.formUnion(other)
    return newSet
  }

  /// Inserts the elements of the given sequence into the set.
  ///
  /// If the set already contains one or more elements that are also in
  /// `other`, the existing members are kept. If `other` contains multiple
  /// instances of equivalent elements, only the first instance is kept.
  ///
  ///     var attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     let visitors = ["Diana", "Marcia", "Nathaniel"]
  ///     attendees.formUnion(visitors)
  ///     print(attendees)
  ///     // Prints "["Diana", "Nathaniel", "Bethany", "Alicia", "Marcia"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  @inlinable
  public mutating func formUnion<S: Sequence>(_ other: __owned S)
  where S.Element == Element {
    for item in other {
      insert(item)
    }
  }

  /// Returns a new set containing the elements of this set that do not occur
  /// in the given sequence.
  ///
  /// In the following example, the `nonNeighbors` set is made up of the
  /// elements of the `employees` set that are not elements of `neighbors`:
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     let nonNeighbors = employees.subtracting(neighbors)
  ///     print(nonNeighbors)
  ///     // Prints "["Chris", "Diana", "Alicia"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  /// - Returns: A new set.
  @inlinable
  public __consuming func subtracting<S: Sequence>(_ other: S) -> Set<Element>
  where S.Element == Element {
    return self._subtracting(other)
  }

  @inlinable
  internal __consuming func _subtracting<S: Sequence>(
    _ other: S
  ) -> Set<Element>
  where S.Element == Element {
    return Set(_native: _variant.convertedToNative.subtracting(other))
  }

  /// Removes the elements of the given sequence from the set.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// also elements of the `neighbors` array are removed. In particular, the
  /// names `"Bethany"` and `"Eric"` are removed from `employees`.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     employees.subtract(neighbors)
  ///     print(employees)
  ///     // Prints "["Chris", "Diana", "Alicia"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  @inlinable
  public mutating func subtract<S: Sequence>(_ other: S)
  where S.Element == Element {
    _subtract(other)
  }

  @inlinable
  internal mutating func _subtract<S: Sequence>(_ other: S)
  where S.Element == Element {
    // If self is empty we don't need to iterate over `other` because there's
    // nothing to remove on self.
    guard !isEmpty else { return }

    for item in other {
      remove(item)
    }
  }

  /// Returns a new set with the elements that are common to both this set and
  /// the given sequence.
  ///
  /// In the following example, the `bothNeighborsAndEmployees` set is made up
  /// of the elements that are in *both* the `employees` and `neighbors` sets.
  /// Elements that are in only one or the other are left out of the result of
  /// the intersection.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     let bothNeighborsAndEmployees = employees.intersection(neighbors)
  ///     print(bothNeighborsAndEmployees)
  ///     // Prints "["Bethany", "Eric"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  /// - Returns: A new set.
  @inlinable
  public __consuming func intersection<S: Sequence>(_ other: S) -> Set<Element>
  where S.Element == Element {
    if let s = _specialize(other, for: Set<Element>.self) {
      return self.intersection(s)
    }
    return Set(_native: _variant.convertedToNative.genericIntersection(other))
  }

  /// Removes the elements of the set that aren't also in the given sequence.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// not also members of the `neighbors` set are removed. In particular, the
  /// names `"Alicia"`, `"Chris"`, and `"Diana"` are removed.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     employees.formIntersection(neighbors)
  ///     print(employees)
  ///     // Prints "["Bethany", "Eric"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  @inlinable
  public mutating func formIntersection<S: Sequence>(_ other: S)
  where S.Element == Element {
    // FIXME: This discards storage reserved with reserveCapacity.
    // FIXME: Depending on the ratio of elements kept in the result, it may be
    // faster to do the removals in place, in bulk.
    self = self.intersection(other)
  }

  /// Returns a new set with the elements that are either in this set or in the
  /// given sequence, but not in both.
  ///
  /// In the following example, the `eitherNeighborsOrEmployees` set is made up
  /// of the elements of the `employees` and `neighbors` sets that are not in
  /// both `employees` *and* `neighbors`. In particular, the names `"Bethany"`
  /// and `"Eric"` do not appear in `eitherNeighborsOrEmployees`.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani"]
  ///     let eitherNeighborsOrEmployees = employees.symmetricDifference(neighbors)
  ///     print(eitherNeighborsOrEmployees)
  ///     // Prints "["Diana", "Forlani", "Alicia"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  /// - Returns: A new set.
  @inlinable
  public __consuming func symmetricDifference<S: Sequence>(
    _ other: __owned S
  ) -> Set<Element>
  where S.Element == Element {
    var newSet = self
    newSet.formSymmetricDifference(other)
    return newSet
  }

  /// Replace this set with the elements contained in this set or the given
  /// set, but not both.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// also members of `neighbors` are removed from `employees`, while the
  /// elements of `neighbors` that are not members of `employees` are added to
  /// `employees`. In particular, the names `"Bethany"` and `"Eric"` are
  /// removed from `employees` while the name `"Forlani"` is added.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Diana", "Eric"]
  ///     let neighbors = ["Bethany", "Eric", "Forlani"]
  ///     employees.formSymmetricDifference(neighbors)
  ///     print(employees)
  ///     // Prints "["Diana", "Forlani", "Alicia"]"
  ///
  /// - Parameter other: A sequence of elements. `other` must be finite.
  @inlinable
  public mutating func formSymmetricDifference<S: Sequence>(
    _ other: __owned S)
  where S.Element == Element {
    let otherSet = Set(other)
    formSymmetricDifference(otherSet)
  }
}

@_unavailableInEmbedded
extension Set: CustomStringConvertible, CustomDebugStringConvertible {
  /// A string that represents the contents of the set.
  public var description: String {
    return _makeCollectionDescription()
  }

  /// A string that represents the contents of the set, suitable for debugging.
  public var debugDescription: String {
    return _makeCollectionDescription(withTypeName: "Set")
  }
}

extension Set {
  /// Removes the elements of the given set from this set.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// also members of the `neighbors` set are removed. In particular, the
  /// names `"Bethany"` and `"Eric"` are removed from `employees`.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     employees.subtract(neighbors)
  ///     print(employees)
  ///     // Prints "["Diana", "Chris", "Alicia"]"
  ///
  /// - Parameter other: Another set.
  @inlinable
  public mutating func subtract(_ other: Set<Element>) {
    _subtract(other)
  }

  /// Returns a Boolean value that indicates whether this set is a subset of
  /// the given set.
  ///
  /// Set *A* is a subset of another set *B* if every member of *A* is also a
  /// member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isSubset(of: employees))
  ///     // Prints "true"
  ///
  /// - Parameter other: Another set.
  /// - Returns: `true` if the set is a subset of `other`; otherwise, `false`.
  @inlinable
  public func isSubset(of other: Set<Element>) -> Bool {
    guard self.count <= other.count else { return false }
    for member in self {
      guard other.contains(member) else {
        return false
      }
    }
    return true
  }

  /// Returns a Boolean value that indicates whether this set is a superset of
  /// the given set.
  ///
  /// Set *A* is a superset of another set *B* if every member of *B* is also a
  /// member of *A*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(employees.isSuperset(of: attendees))
  ///     // Prints "true"
  ///
  /// - Parameter other: Another set.
  /// - Returns: `true` if the set is a superset of `other`; otherwise,
  ///   `false`.
  @inlinable
  public func isSuperset(of other: Set<Element>) -> Bool {
    return other.isSubset(of: self)
  }

  /// Returns a Boolean value that indicates whether this set has no members in
  /// common with the given set.
  ///
  /// In the following example, the `employees` set is disjoint with the
  /// `visitors` set because no name appears in both sets.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let visitors: Set = ["Marcia", "Nathaniel", "Olivia"]
  ///     print(employees.isDisjoint(with: visitors))
  ///     // Prints "true"
  ///
  /// - Parameter other: Another set.
  /// - Returns: `true` if the set has no elements in common with `other`;
  ///   otherwise, `false`.
  @inlinable
  public func isDisjoint(with other: Set<Element>) -> Bool {
    guard !isEmpty && !other.isEmpty else { return true }
    let (smaller, larger) =
      count < other.count ? (self, other) : (other, self)
    for member in smaller {
      if larger.contains(member) {
        return false
      }
    }
    return true
  }

  @inlinable
  internal func _isDisjoint<S: Sequence>(with other: S) -> Bool
  where S.Element == Element {
    guard !isEmpty else { return true }

    for member in other {
      if contains(member) {
        return false
      }
    }
    return true
  }

  /// Returns a new set containing the elements of this set that do not occur
  /// in the given set.
  ///
  /// In the following example, the `nonNeighbors` set is made up of the
  /// elements of the `employees` set that are not elements of `neighbors`:
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     let nonNeighbors = employees.subtracting(neighbors)
  ///     print(nonNeighbors)
  ///     // Prints "["Diana", "Chris", "Alicia"]"
  ///
  /// - Parameter other: Another set.
  /// - Returns: A new set.
  @inlinable
  public __consuming func subtracting(_ other: Set<Element>) -> Set<Element> {
    // Heuristic: if `other` is small enough, it's better to make a copy of the
    // set and remove each item one by one. (The best cutoff point depends on
    // the `Element` type; the one below is an educated guess.) FIXME: Derive a
    // better cutoff by benchmarking.
    if other.count <= self.count / 8 {
      var copy = self
      copy._subtract(other)
      return copy
    }
    // Otherwise do a regular subtraction using a temporary bitmap.
    return self._subtracting(other)
  }

  /// Returns a Boolean value that indicates whether the set is a strict
  /// superset of the given sequence.
  ///
  /// Set *A* is a strict superset of another set *B* if every member of *B* is
  /// also a member of *A* and *A* contains at least one element that is *not*
  /// a member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(employees.isStrictSuperset(of: attendees))
  ///     // Prints "true"
  ///     print(employees.isStrictSuperset(of: employees))
  ///     // Prints "false"
  ///
  /// - Parameter other: Another set.
  /// - Returns: `true` if the set is a strict superset of
  ///   `other`; otherwise, `false`.
  @inlinable
  public func isStrictSuperset(of other: Set<Element>) -> Bool {
    return self.count > other.count && other.isSubset(of: self)
  }

  /// Returns a Boolean value that indicates whether the set is a strict subset
  /// of the given sequence.
  ///
  /// Set *A* is a strict subset of another set *B* if every member of *A* is
  /// also a member of *B* and *B* contains at least one element that is not a
  /// member of *A*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isStrictSubset(of: employees))
  ///     // Prints "true"
  ///
  ///     // A set is never a strict subset of itself:
  ///     print(attendees.isStrictSubset(of: attendees))
  ///     // Prints "false"
  ///
  /// - Parameter other: Another set.
  /// - Returns: `true` if the set is a strict subset of
  ///   `other`; otherwise, `false`.
  @inlinable
  public func isStrictSubset(of other: Set<Element>) -> Bool {
    return self.count < other.count && self.isSubset(of: other)
  }

  /// Returns a new set with the elements that are common to both this set and
  /// the given sequence.
  ///
  /// In the following example, the `bothNeighborsAndEmployees` set is made up
  /// of the elements that are in *both* the `employees` and `neighbors` sets.
  /// Elements that are in only one or the other are left out of the result of
  /// the intersection.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     let bothNeighborsAndEmployees = employees.intersection(neighbors)
  ///     print(bothNeighborsAndEmployees)
  ///     // Prints "["Bethany", "Eric"]"
  ///
  /// - Parameter other: Another set.
  /// - Returns: A new set.
  @inlinable
  public __consuming func intersection(_ other: Set<Element>) -> Set<Element> {
    Set(_native: _variant.intersection(other))
  }

  /// Removes the elements of the set that are also in the given sequence and
  /// adds the members of the sequence that are not already in the set.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// also members of `neighbors` are removed from `employees`, while the
  /// elements of `neighbors` that are not members of `employees` are added to
  /// `employees`. In particular, the names `"Alicia"`, `"Chris"`, and
  /// `"Diana"` are removed from `employees` while the names `"Forlani"` and
  /// `"Greta"` are added.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     employees.formSymmetricDifference(neighbors)
  ///     print(employees)
  ///     // Prints "["Diana", "Chris", "Forlani", "Alicia", "Greta"]"
  ///
  /// - Parameter other: Another set.
  @inlinable
  public mutating func formSymmetricDifference(_ other: __owned Set<Element>) {
    for member in other {
      if contains(member) {
        remove(member)
      } else {
        insert(member)
      }
    }
  }
}

extension Set {
  /// The position of an element in a set.
  @frozen
  public struct Index {
    // Index for native buffer is efficient.  Index for bridged NSSet is
    // not, because neither NSEnumerator nor fast enumeration support moving
    // backwards.  Even if they did, there is another issue: NSEnumerator does
    // not support NSCopying, and fast enumeration does not document that it is
    // safe to copy the state.  So, we cannot implement Index that is a value
    // type for bridged NSSet in terms of Cocoa enumeration facilities.

    @frozen
    @usableFromInline
    internal enum _Variant {
      case native(_HashTable.Index)
#if _runtime(_ObjC)
      case cocoa(__CocoaSet.Index)
#endif
    }

    @usableFromInline
    internal var _variant: _Variant

    @inlinable
    @inline(__always)
    internal init(_variant: __owned _Variant) {
      self._variant = _variant
    }

    @inlinable
    @inline(__always)
    internal init(_native index: _HashTable.Index) {
      self.init(_variant: .native(index))
    }

#if _runtime(_ObjC)
    @inlinable
    @inline(__always)
    internal init(_cocoa index: __owned __CocoaSet.Index) {
      self.init(_variant: .cocoa(index))
    }
#endif
  }
}

extension Set.Index {
#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _guaranteedNative: Bool {
    return _canBeClass(Element.self) == 0
  }

  /// Allow the optimizer to consider the surrounding code unreachable if
  /// Set<Element> is guaranteed to be native.
  @usableFromInline
  @_transparent
  internal func _cocoaPath() {
    if _guaranteedNative {
      _conditionallyUnreachable()
    }
  }

  @inlinable
  @inline(__always)
  internal mutating func _isUniquelyReferenced() -> Bool {
    defer { _fixLifetime(self) }
    var handle = _asCocoa.handleBitPattern
    return handle == 0 || _isUnique_native(&handle)
  }
#endif

#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _isNative: Bool {
    switch _variant {
    case .native:
      return true
    case .cocoa:
      _cocoaPath()
      return false
    }
  }
#endif

  @usableFromInline @_transparent
  internal var _asNative: _HashTable.Index {
    switch _variant {
    case .native(let nativeIndex):
      return nativeIndex
#if _runtime(_ObjC)
    case .cocoa:
      _preconditionFailure(
        "Attempting to access Set elements using an invalid index")
#endif
    }
  }

#if _runtime(_ObjC)
  @usableFromInline
  internal var _asCocoa: __CocoaSet.Index {
    @_transparent
    get {
      switch _variant {
      case .native:
        _preconditionFailure(
          "Attempting to access Set elements using an invalid index")
      case .cocoa(let cocoaIndex):
        return cocoaIndex
      }
    }
    _modify {
      guard case .cocoa(var cocoa) = _variant else {
        _preconditionFailure(
          "Attempting to access Set elements using an invalid index")
      }
      let dummy = _HashTable.Index(bucket: _HashTable.Bucket(offset: 0), age: 0)
      _variant = .native(dummy)
      defer { _variant = .cocoa(cocoa) }
      yield &cocoa
    }
  }
#endif
}

extension Set.Index: Equatable {
  @inlinable
  public static func == (
    lhs: Set<Element>.Index,
    rhs: Set<Element>.Index
  ) -> Bool {
    switch (lhs._variant, rhs._variant) {
    case (.native(let lhsNative), .native(let rhsNative)):
      return lhsNative == rhsNative
  #if _runtime(_ObjC)
    case (.cocoa(let lhsCocoa), .cocoa(let rhsCocoa)):
      lhs._cocoaPath()
      return lhsCocoa == rhsCocoa
    default:
      _preconditionFailure("Comparing indexes from different sets")
  #endif
    }
  }
}

extension Set.Index: Comparable {
  @inlinable
  public static func < (
    lhs: Set<Element>.Index,
    rhs: Set<Element>.Index
  ) -> Bool {
    switch (lhs._variant, rhs._variant) {
    case (.native(let lhsNative), .native(let rhsNative)):
      return lhsNative < rhsNative
  #if _runtime(_ObjC)
    case (.cocoa(let lhsCocoa), .cocoa(let rhsCocoa)):
      lhs._cocoaPath()
      return lhsCocoa < rhsCocoa
    default:
      _preconditionFailure("Comparing indexes from different sets")
  #endif
    }
  }
}

extension Set.Index: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  public // FIXME(cocoa-index): Make inlinable
  func hash(into hasher: inout Hasher) {
#if _runtime(_ObjC)
    guard _isNative else {
      hasher.combine(1 as UInt8)
      hasher.combine(_asCocoa._offset)
      return
    }
    hasher.combine(0 as UInt8)
    hasher.combine(_asNative.bucket.offset)
#else
    hasher.combine(_asNative.bucket.offset)
#endif
  }
}

extension Set {
  /// An iterator over the members of a `Set<Element>`.
  @frozen
  public struct Iterator {
    // Set has a separate IteratorProtocol and Index because of efficiency
    // and implementability reasons.
    //
    // Native sets have efficient indices.  Bridged NSSet instances don't.
    //
    // Even though fast enumeration is not suitable for implementing
    // Index, which is multi-pass, it is suitable for implementing a
    // IteratorProtocol, which is being consumed as iteration proceeds.

    @usableFromInline
    @frozen
    internal enum _Variant {
      case native(_NativeSet<Element>.Iterator)
#if _runtime(_ObjC)
      case cocoa(__CocoaSet.Iterator)
#endif
    }

    @usableFromInline
    internal var _variant: _Variant

    @inlinable
    internal init(_variant: __owned _Variant) {
      self._variant = _variant
    }

    @inlinable
    internal init(_native: __owned _NativeSet<Element>.Iterator) {
      self.init(_variant: .native(_native))
    }

#if _runtime(_ObjC)
    @usableFromInline
    internal init(_cocoa: __owned __CocoaSet.Iterator) {
      self.init(_variant: .cocoa(_cocoa))
    }
#endif
  }
}

@available(*, unavailable)
extension Set.Iterator._Variant: Sendable {}

extension Set.Iterator {
#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _guaranteedNative: Bool {
    return _canBeClass(Element.self) == 0
  }

  /// Allow the optimizer to consider the surrounding code unreachable if
  /// Set<Element> is guaranteed to be native.
  @usableFromInline @_transparent
  internal func _cocoaPath() {
    if _guaranteedNative {
      _conditionallyUnreachable()
    }
  }
#endif

#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _isNative: Bool {
    switch _variant {
    case .native:
      return true
    case .cocoa:
      _cocoaPath()
      return false
    }
  }
#endif

  @usableFromInline @_transparent
  internal var _asNative: _NativeSet<Element>.Iterator {
    get {
      switch _variant {
      case .native(let nativeIterator):
        return nativeIterator
#if _runtime(_ObjC)
      case .cocoa:
        _internalInvariantFailure("internal error: does not contain a native index")
#endif
      }
    }
    set {
      self._variant = .native(newValue)
    }
  }

#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _asCocoa: __CocoaSet.Iterator {
    get {
      switch _variant {
      case .native:
        _internalInvariantFailure("internal error: does not contain a Cocoa index")
      case .cocoa(let cocoa):
        return cocoa
      }
    }
  }
#endif
}

extension Set.Iterator: IteratorProtocol {
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable
  @inline(__always)
  public mutating func next() -> Element? {
#if _runtime(_ObjC)
    guard _isNative else {
      guard let cocoaElement = _asCocoa.next() else { return nil }
      return _forceBridgeFromObjectiveC(cocoaElement, Element.self)
    }
#endif
    return _asNative.next()
  }
}

#if SWIFT_ENABLE_REFLECTION
extension Set.Iterator: CustomReflectable {
  /// A mirror that reflects the iterator.
  public var customMirror: Mirror {
    return Mirror(
      self,
      children: EmptyCollection<(label: String?, value: Any)>())
  }
}

extension Set: CustomReflectable {
  /// A mirror that reflects the set.
  public var customMirror: Mirror {
    let style = Mirror.DisplayStyle.`set`
    return Mirror(self, unlabeledChildren: self, displayStyle: style)
  }
}
#endif

extension Set {
  /// Removes and returns the first element of the set.
  ///
  /// Because a set is not an ordered collection, the "first" element may not
  /// be the first element that was added to the set.
  ///
  /// - Returns: A member of the set. If the set is empty, returns `nil`.
  @inlinable
  public mutating func popFirst() -> Element? {
    guard !isEmpty else { return nil }
    return remove(at: startIndex)
  }

  /// The total number of elements that the set can contain without
  /// allocating new storage.
  @inlinable
  public var capacity: Int {
    return _variant.capacity
  }

  /// Reserves enough space to store the specified number of elements.
  ///
  /// If you are adding a known number of elements to a set, use this
  /// method to avoid multiple reallocations. This method ensures that the
  /// set has unique, mutable, contiguous storage, with space allocated
  /// for at least the requested number of elements.
  ///
  /// Calling the `reserveCapacity(_:)` method on a set with bridged
  /// storage triggers a copy to contiguous storage even if the existing
  /// storage has room to store `minimumCapacity` elements.
  ///
  /// - Parameter minimumCapacity: The requested number of elements to
  ///   store.
  public // FIXME(reserveCapacity): Should be inlinable
  mutating func reserveCapacity(_ minimumCapacity: Int) {
    _variant.reserveCapacity(minimumCapacity)
    _internalInvariant(self.capacity >= minimumCapacity)
  }
}

public typealias SetIndex<Element: Hashable> = Set<Element>.Index
public typealias SetIterator<Element: Hashable> = Set<Element>.Iterator

extension Set: @unchecked Sendable
  where Element: Sendable { }
extension Set.Index: @unchecked Sendable
  where Element: Sendable { }
extension Set.Iterator: @unchecked Sendable
  where Element: Sendable { }

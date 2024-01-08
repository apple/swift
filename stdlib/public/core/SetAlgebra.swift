//===--- SetAlgebra.swift - Protocols for set operations ------------------===//
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
//
//  
//
//===----------------------------------------------------------------------===//

/// A type that provides mathematical set operations.
///
/// You use types that conform to the `SetAlgebra` protocol when you need
/// efficient membership tests or mathematical set operations such as
/// intersection, union, and subtraction. In the standard library, you can
/// use the `Set` type with elements of any hashable type, or you can easily
/// create bit masks with `SetAlgebra` conformance using the `OptionSet`
/// protocol. See those types for more information.
///
/// - Note: Unlike ordinary set types, the `Element` type of an `OptionSet` is
///   identical to the `OptionSet` type itself. The `SetAlgebra` protocol is
///   specifically designed to accommodate both kinds of set.
///
/// Conforming to the SetAlgebra Protocol
/// =====================================
///
/// When implementing a custom type that conforms to the `SetAlgebra` protocol,
/// you must implement the required initializers and methods. For the
/// inherited methods to work properly, conforming types must meet the
/// following axioms. Assume that `S` is a custom type that conforms to the
/// `SetAlgebra` protocol, `x` and `y` are instances of `S`, and `e` is of
/// type `S.Element`---the type that the set holds.
///
/// - `S() == []`
/// - `x.intersection(x) == x`
/// - `x.intersection([]) == []`
/// - `x.union(x) == x`
/// - `x.union([]) == x`
/// - `x.contains(e)` implies `x.union(y).contains(e)`
/// - `x.union(y).contains(e)` implies `x.contains(e) || y.contains(e)`
/// - `x.contains(e) && y.contains(e)` if and only if
///   `x.intersection(y).contains(e)`
/// - `x.isSubset(of: y)` implies `x.union(y) == y`
/// - `x.isSuperset(of: y)` implies `x.union(y) == x`
/// - `x.isSubset(of: y)` if and only if `y.isSuperset(of: x)`
/// - `x.isStrictSuperset(of: y)` if and only if
///   `x.isSuperset(of: y) && x != y`
/// - `x.isStrictSubset(of: y)` if and only if `x.isSubset(of: y) && x != y`
public protocol SetAlgebra<Element>: Equatable, ExpressibleByArrayLiteral {
  /// A type for which the conforming type provides a containment test.
  associatedtype Element
  
  /// Creates an empty set.
  ///
  /// This initializer is equivalent to initializing with an empty array
  /// literal. For example, you create an empty `Set` instance with either
  /// this initializer or with an empty array literal.
  ///
  ///     var emptySet = Set<Int>()
  ///     print(emptySet.isEmpty)
  ///     // Prints "true"
  ///
  ///     emptySet = []
  ///     print(emptySet.isEmpty)
  ///     // Prints "true"
  init()
  
  /// Returns a Boolean value that indicates whether the given element exists
  /// in the set.
  ///
  /// This example uses the `contains(_:)` method to test whether an integer is
  /// a member of a set of prime numbers.
  ///
  ///     let primes: Set = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]
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
  func contains(_ member: Element) -> Bool

  /// Returns a new set with the elements of both this and the given set.
  ///
  /// In the following example, the `attendeesAndVisitors` set is made up
  /// of the elements of the `attendees` and `visitors` sets:
  ///
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     let visitors = ["Marcia", "Nathaniel"]
  ///     let attendeesAndVisitors = attendees.union(visitors)
  ///     print(attendeesAndVisitors)
  ///     // Prints "["Diana", "Nathaniel", "Bethany", "Alicia", "Marcia"]"
  ///
  /// If the set already contains one or more elements that are also in
  /// `other`, the existing members are kept.
  ///
  ///     let initialIndices = Set(0..<5)
  ///     let expandedIndices = initialIndices.union([2, 3, 6, 7])
  ///     print(expandedIndices)
  ///     // Prints "[2, 4, 6, 7, 0, 1, 3]"
  ///
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: A new set with the unique elements of this set and `other`.
  ///
  /// - Note: if this set and `other` contain elements that are equal but
  ///   distinguishable (e.g. via `===`), which of these elements is present
  ///   in the result is unspecified.
  __consuming func union(_ other: __owned Self) -> Self
  
  /// Returns a new set with the elements that are common to both this set and
  /// the given set.
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: A new set.
  ///
  /// - Note: if this set and `other` contain elements that are equal but
  ///   distinguishable (e.g. via `===`), which of these elements is present
  ///   in the result is unspecified.
  __consuming func intersection(_ other: Self) -> Self

  /// Returns a new set with the elements that are either in this set or in the
  /// given set, but not in both.
  ///
  /// In the following example, the `eitherNeighborsOrEmployees` set is made up
  /// of the elements of the `employees` and `neighbors` sets that are not in
  /// both `employees` *and* `neighbors`. In particular, the names `"Bethany"`
  /// and `"Eric"` do not appear in `eitherNeighborsOrEmployees`.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani"]
  ///     let eitherNeighborsOrEmployees = employees.symmetricDifference(neighbors)
  ///     print(eitherNeighborsOrEmployees)
  ///     // Prints "["Diana", "Forlani", "Alicia"]"
  ///
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: A new set.
  __consuming func symmetricDifference(_ other: __owned Self) -> Self

  // FIXME(move-only types): SetAlgebra.insert is not implementable by a
  // set with move-only Element type, since it would be necessary to copy
  // the argument in order to both store it inside the set and return it as
  // the `memberAfterInsert`.

  /// Inserts the given element in the set if it is not already present.
  ///
  /// If an element equal to `newMember` is already contained in the set, this
  /// method has no effect. In this example, a new element is inserted into
  /// `classDays`, a set of days of the week. When an existing element is
  /// inserted, the `classDays` set does not change.
  ///
  ///     enum DayOfTheWeek: Int {
  ///         case sunday, monday, tuesday, wednesday, thursday,
  ///             friday, saturday
  ///     }
  ///
  ///     var classDays: Set<DayOfTheWeek> = [.wednesday, .friday]
  ///     print(classDays.insert(.monday))
  ///     // Prints "(true, .monday)"
  ///     print(classDays)
  ///     // Prints "[.friday, .wednesday, .monday]"
  ///
  ///     print(classDays.insert(.friday))
  ///     // Prints "(false, .friday)"
  ///     print(classDays)
  ///     // Prints "[.friday, .wednesday, .monday]"
  ///
  /// - Parameter newMember: An element to insert into the set.
  /// - Returns: `(true, newMember)` if `newMember` was not contained in the
  ///   set. If an element equal to `newMember` was already contained in the
  ///   set, the method returns `(false, oldMember)`, where `oldMember` is the
  ///   element that was equal to `newMember`. In some cases, `oldMember` may
  ///   be distinguishable from `newMember` by identity comparison or some
  ///   other means.
  @discardableResult
  mutating func insert(
    _ newMember: __owned Element
  ) -> (inserted: Bool, memberAfterInsert: Element)
  
  /// Removes the given element and any elements subsumed by the given element.
  ///
  /// - Parameter member: The element of the set to remove.
  /// - Returns: For ordinary sets, an element equal to `member` if `member` is
  ///   contained in the set; otherwise, `nil`. In some cases, a returned
  ///   element may be distinguishable from `member` by identity comparison
  ///   or some other means.
  ///
  ///   For sets where the set type and element type are the same, like
  ///   `OptionSet` types, this method returns any intersection between the set
  ///   and `[member]`, or `nil` if the intersection is empty.
  @discardableResult
  mutating func remove(_ member: Element) -> Element?

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
  ///     // Prints "Optional(.monday)"
  ///
  /// - Parameter newMember: An element to insert into the set.
  /// - Returns: For ordinary sets, an element equal to `newMember` if the set
  ///   already contained such a member; otherwise, `nil`. In some cases, the
  ///   returned element may be distinguishable from `newMember` by identity
  ///   comparison or some other means.
  ///
  ///   For sets where the set type and element type are the same, like
  ///   `OptionSet` types, this method returns any intersection between the 
  ///   set and `[newMember]`, or `nil` if the intersection is empty.
  @discardableResult
  mutating func update(with newMember: __owned Element) -> Element?
  
  /// Adds the elements of the given set to the set.
  ///
  /// In the following example, the elements of the `visitors` set are added to
  /// the `attendees` set:
  ///
  ///     var attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     let visitors: Set = ["Diana", "Marcia", "Nathaniel"]
  ///     attendees.formUnion(visitors)
  ///     print(attendees)
  ///     // Prints "["Diana", "Nathaniel", "Bethany", "Alicia", "Marcia"]"
  ///
  /// If the set already contains one or more elements that are also in
  /// `other`, the existing members are kept.
  ///
  ///     var initialIndices = Set(0..<5)
  ///     initialIndices.formUnion([2, 3, 6, 7])
  ///     print(initialIndices)
  ///     // Prints "[2, 4, 6, 7, 0, 1, 3]"
  ///
  /// - Parameter other: A set of the same type as the current set.
  mutating func formUnion(_ other: __owned Self)

  /// Removes the elements of this set that aren't also in the given set.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// not also members of the `neighbors` set are removed. In particular, the
  /// names `"Alicia"`, `"Chris"`, and `"Diana"` are removed.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani", "Greta"]
  ///     employees.formIntersection(neighbors)
  ///     print(employees)
  ///     // Prints "["Bethany", "Eric"]"
  ///
  /// - Parameter other: A set of the same type as the current set.
  mutating func formIntersection(_ other: Self)

  /// Removes the elements of the set that are also in the given set and adds
  /// the members of the given set that are not already in the set.
  ///
  /// In the following example, the elements of the `employees` set that are
  /// also members of `neighbors` are removed from `employees`, while the
  /// elements of `neighbors` that are not members of `employees` are added to
  /// `employees`. In particular, the names `"Bethany"` and `"Eric"` are
  /// removed from `employees` while the name `"Forlani"` is added.
  ///
  ///     var employees: Set = ["Alicia", "Bethany", "Diana", "Eric"]
  ///     let neighbors: Set = ["Bethany", "Eric", "Forlani"]
  ///     employees.formSymmetricDifference(neighbors)
  ///     print(employees)
  ///     // Prints "["Diana", "Forlani", "Alicia"]"
  ///
  /// - Parameter other: A set of the same type.
  mutating func formSymmetricDifference(_ other: __owned Self)

  //===--- Requirements with default implementations ----------------------===//
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: A new set.
  __consuming func subtracting(_ other: Self) -> Self

  /// Returns a Boolean value that indicates whether the set is a subset of
  /// another set.
  ///
  /// Set *A* is a subset of another set *B* if every member of *A* is also a
  /// member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isSubset(of: employees))
  ///     // Prints "true"
  ///
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a subset of `other`; otherwise, `false`.
  func isSubset(of other: Self) -> Bool

  /// Returns a Boolean value that indicates whether the set has no members in
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set has no elements in common with `other`;
  ///   otherwise, `false`.
  func isDisjoint(with other: Self) -> Bool

  /// Returns a Boolean value that indicates whether the set is a superset of
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a superset of `possibleSubset`;
  ///   otherwise, `false`.
  func isSuperset(of other: Self) -> Bool

  /// A Boolean value that indicates whether the set has no elements.
  var isEmpty: Bool { get }
  
  /// Creates a new set from a finite sequence of items.
  ///
  /// Use this initializer to create a new set from an existing sequence, like
  /// an array or a range:
  ///
  ///     let validIndices = Set(0..<7).subtracting([2, 4, 5])
  ///     print(validIndices)
  ///     // Prints "[6, 0, 1, 3]"
  ///
  /// - Parameter sequence: The elements to use as members of the new set.
  init<S: Sequence>(_ sequence: __owned S) where S.Element == Element

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
  /// - Parameter other: A set of the same type as the current set.
  mutating func subtract(_ other: Self)
}

/// `SetAlgebra` requirements for which default implementations
/// are supplied.
///
/// - Note: A type conforming to `SetAlgebra` can implement any of
///   these initializers or methods, and those implementations will be
///   used in lieu of these defaults.
extension SetAlgebra {
  /// Creates a new set from a finite sequence of items.
  ///
  /// Use this initializer to create a new set from an existing sequence, like
  /// an array or a range:
  ///
  ///     let validIndices = Set(0..<7).subtracting([2, 4, 5])
  ///     print(validIndices)
  ///     // Prints "[6, 0, 1, 3]"
  ///
  /// - Parameter sequence: The elements to use as members of the new set.
  @inlinable // protocol-only
  public init<S: Sequence>(@_eagerMove _ sequence: __owned S)
    where S.Element == Element {
    self.init()
    // Needed to fully optimize OptionSet literals.
    _onFastPath()
    for e in sequence { insert(e) }
  }

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
  /// - Parameter other: A set of the same type as the current set.
  @inlinable // protocol-only
  public mutating func subtract(_ other: Self) {
    self.formIntersection(self.symmetricDifference(other))
  }

  /// Returns a Boolean value that indicates whether the set is a subset of
  /// another set.
  ///
  /// Set *A* is a subset of another set *B* if every member of *A* is also a
  /// member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(attendees.isSubset(of: employees))
  ///     // Prints "true"
  ///
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a subset of `other`; otherwise, `false`.
  @inlinable // protocol-only
  public func isSubset(of other: Self) -> Bool {
    return self.intersection(other) == self
  }

  /// Returns a Boolean value that indicates whether the set is a superset of
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a superset of `other`; otherwise,
  ///   `false`.
  @inlinable // protocol-only
  public func isSuperset(of other: Self) -> Bool {
    return other.isSubset(of: self)
  }

  /// Returns a Boolean value that indicates whether the set has no members in
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set has no elements in common with `other`;
  ///   otherwise, `false`.
  @inlinable // protocol-only
  public func isDisjoint(with other: Self) -> Bool {
    return self.intersection(other).isEmpty
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: A new set.
  @inlinable // protocol-only
  public func subtracting(_ other: Self) -> Self {
    return self.intersection(self.symmetricDifference(other))
  }

  /// A Boolean value that indicates whether the set has no elements.
  @inlinable // protocol-only
  public var isEmpty: Bool {
    return self == Self()
  }

  /// Returns a Boolean value that indicates whether this set is a strict
  /// superset of the given set.
  ///
  /// Set *A* is a strict superset of another set *B* if every member of *B* is
  /// also a member of *A* and *A* contains at least one element that is *not*
  /// a member of *B*.
  ///
  ///     let employees: Set = ["Alicia", "Bethany", "Chris", "Diana", "Eric"]
  ///     let attendees: Set = ["Alicia", "Bethany", "Diana"]
  ///     print(employees.isStrictSuperset(of: attendees))
  ///     // Prints "true"
  ///
  ///     // A set is never a strict superset of itself:
  ///     print(employees.isStrictSuperset(of: employees))
  ///     // Prints "false"
  ///
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a strict superset of `other`; otherwise,
  ///   `false`.
  @inlinable // protocol-only
  public func isStrictSuperset(of other: Self) -> Bool {
    return self.isSuperset(of: other) && self != other
  }

  /// Returns a Boolean value that indicates whether this set is a strict
  /// subset of the given set.
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
  /// - Parameter other: A set of the same type as the current set.
  /// - Returns: `true` if the set is a strict subset of `other`; otherwise,
  ///   `false`.
  @inlinable // protocol-only
  public func isStrictSubset(of other: Self) -> Bool {
    return other.isStrictSuperset(of: self)
  }
}

extension SetAlgebra where Element == ArrayLiteralElement {
  /// Creates a set containing the elements of the given array literal.
  ///
  /// Do not call this initializer directly. It is used by the compiler when
  /// you use an array literal. Instead, create a new set using an array
  /// literal as its value by enclosing a comma-separated list of values in
  /// square brackets. You can use an array literal anywhere a set is expected
  /// by the type context.
  ///
  /// Here, a set of strings is created from an array literal holding only
  /// strings:
  ///
  ///     let ingredients: Set = ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///     if ingredients.isSuperset(of: ["sugar", "salt"]) {
  ///         print("Whatever it is, it's bound to be delicious!")
  ///     }
  ///     // Prints "Whatever it is, it's bound to be delicious!"
  ///
  /// - Parameter arrayLiteral: A list of elements of the new set.
  @inlinable // protocol-only
  public init(arrayLiteral: Element...) {
    self.init(arrayLiteral)
  }  
}

//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2015 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A collection of insertions and removals that describe the difference 
/// between two ordered collection states.
@available(SwiftStdlib 5.1, *)
public struct CollectionDifference<ChangeElement> {
  /// A single change to a collection.
  @frozen
  public enum Change {
    /// An insertion.
    ///
    /// The `offset` value is the offset of the inserted element in the final
    /// state of the collection after the difference is fully applied.
    /// A non-`nil` `associatedWith` value is the offset of the complementary 
    /// change.
    case insert(offset: Int, element: ChangeElement, associatedWith: Int?)
    
    /// A removal.
    ///
    /// The `offset` value is the offset of the element to be removed in the
    /// original state of the collection. A non-`nil` `associatedWith` value is 
    /// the offset of the complementary change.
    case remove(offset: Int, element: ChangeElement, associatedWith: Int?)

    // Internal common field accessors
    internal var _offset: Int {
      get {
        switch self {
        case .insert(offset: let o, element: _, associatedWith: _):
          return o
        case .remove(offset: let o, element: _, associatedWith: _):
          return o
        }
      }
    }
    internal var _element: ChangeElement {
      get {
        switch self {
        case .insert(offset: _, element: let e, associatedWith: _):
          return e
        case .remove(offset: _, element: let e, associatedWith: _):
          return e
        }
      }
    }
    internal var _associatedOffset: Int? {
      get {
        switch self {
        case .insert(offset: _, element: _, associatedWith: let o):
          return o
        case .remove(offset: _, element: _, associatedWith: let o):
          return o
        }
      }
    }
  }

  /// The insertions contained by this difference, from lowest offset to 
  /// highest.
  public let insertions: [Change]
  
  /// The removals contained by this difference, from lowest offset to highest.
  public let removals: [Change]

  /// Creates a new collection difference from a collection of changes.
  ///
  /// To find the difference between two collections, use the 
  /// `difference(from:)` method declared on the `BidirectionalCollection`
  /// protocol.
  ///
  /// The collection of changes passed as `changes` must meet these 
  /// requirements:
  ///
  /// - All insertion offsets are unique
  /// - All removal offsets are unique
  /// - All associations between insertions and removals are symmetric
  ///
  /// - Parameter changes: A collection of changes that represent a transition
  ///   between two states.
  ///
  /// - Complexity: O(*n* * log(*n*)), where *n* is the length of the
  ///   parameter.
  public init?<Changes: Collection>(
    _ changes: Changes
  ) where Changes.Element == Change {
    guard CollectionDifference<ChangeElement>._validateChanges(changes) else {
      return nil
    }

    self.init(_validatedChanges: changes)
  }

  /// Internal initializer for use by algorithms that cannot produce invalid
  /// collections of changes. These include the Myers' diff algorithm,
  /// self.inverse(), and the move inferencer.
  ///
  /// If parameter validity cannot be guaranteed by the caller then
  /// `CollectionDifference.init?(_:)` should be used instead.
  ///
  /// - Parameter c: A valid collection of changes that represent a transition
  ///   between two states.
  ///
  /// - Complexity: O(*n* * log(*n*)), where *n* is the length of the
  ///   parameter.
  internal init<Changes: Collection>(
    _validatedChanges changes: Changes
  ) where Changes.Element == Change {
    let sortedChanges = changes.sorted { (a, b) -> Bool in
      switch (a, b) {
      case (.remove(_, _, _), .insert(_, _, _)):
        return true
      case (.insert(_, _, _), .remove(_, _, _)):
        return false
      default:
        return a._offset < b._offset
      }
    }

    // Find first insertion via binary search
    let firstInsertIndex: Int
    if sortedChanges.isEmpty {
      firstInsertIndex = 0
    } else {
      var range = 0...sortedChanges.count
      while range.lowerBound != range.upperBound {
        let i = range.lowerBound + (range.upperBound &- range.lowerBound) / 2
        switch sortedChanges[i] {
        case .insert(_, _, _):
          range = range.lowerBound...i
        case .remove(_, _, _):
          range = (i + 1)...range.upperBound
        }
      }
      firstInsertIndex = range.lowerBound
    }

    removals = Array(sortedChanges[0..<firstInsertIndex])
    insertions = Array(sortedChanges[firstInsertIndex..<sortedChanges.count])
  }

  /// The public initializer calls this function to ensure that its parameter
  /// meets the conditions set in its documentation.
  ///
  /// - Parameter changes: a collection of `CollectionDifference.Change`
  ///   instances intended to represent a valid state transition for
  ///   `CollectionDifference`.
  ///
  /// - Returns: whether the parameter meets the following criteria:
  ///
  ///   1. All insertion offsets are unique
  ///   2. All removal offsets are unique
  ///   3. All associations between insertions and removals are symmetric
  ///
  /// Complexity: O(`changes.count`)
  private static func _validateChanges<Changes: Collection>(
    _ changes : Changes
  ) -> Bool where Changes.Element == Change {
    if changes.isEmpty { return true }

    var insertAssocToOffset = Dictionary<Int,Int>()
    var removeOffsetToAssoc = Dictionary<Int,Int>()
    var insertOffset = Set<Int>()
    var removeOffset = Set<Int>()

    for change in changes {
      let offset = change._offset
      if offset < 0 { return false }

      switch change {
      case .remove(_, _, _):
        if removeOffset.contains(offset) { return false }
        removeOffset.insert(offset)
      case .insert(_, _, _):
        if insertOffset.contains(offset) { return false }
        insertOffset.insert(offset)
      } 

      if let assoc = change._associatedOffset {
        if assoc < 0 { return false }
        switch change {
        case .remove(_, _, _):
          if removeOffsetToAssoc[offset] != nil { return false }
          removeOffsetToAssoc[offset] = assoc
        case .insert(_, _, _):
          if insertAssocToOffset[assoc] != nil { return false }
          insertAssocToOffset[assoc] = offset
        }
      }
    }

    return removeOffsetToAssoc == insertAssocToOffset
  }

  public func inverse() -> Self {
    return CollectionDifference(_validatedChanges: self.map { c in
      switch c {
        case .remove(let o, let e, let a):
          return .insert(offset: o, element: e, associatedWith: a)
        case .insert(let o, let e, let a):
          return .remove(offset: o, element: e, associatedWith: a)
      }
    })
  }
}

/// A CollectionDifference is itself a Collection.
///
/// The enumeration order of `Change` elements is:
///
/// 1. `.remove`s, from highest `offset` to lowest
/// 2. `.insert`s, from lowest `offset` to highest
///
/// This guarantees that applicators on compatible base states are safe when
/// written in the form:
///
/// ```
/// for c in diff {
///   switch c {
///   case .remove(offset: let o, element: _, associatedWith: _):
///     arr.remove(at: o)
///   case .insert(offset: let o, element: let e, associatedWith: _):
///     arr.insert(e, at: o)
///   }
/// }
/// ```
@available(SwiftStdlib 5.1, *)
extension CollectionDifference: Collection {
  public typealias Element = Change

  /// The position of a collection difference.
  @frozen
  public struct Index {
    // Opaque index type is isomorphic to Int
    @usableFromInline
    internal let _offset: Int

    internal init(_offset offset: Int) {
      _offset = offset
    }
  }

  public var startIndex: Index {
    return Index(_offset: 0)
  }

  public var endIndex: Index {
    return Index(_offset: removals.count + insertions.count)
  }

  public func index(after index: Index) -> Index {
    return Index(_offset: index._offset + 1)
  }

  public subscript(position: Index) -> Element {
    if position._offset < removals.count {
      return removals[removals.count - (position._offset + 1)]
    }
    return insertions[position._offset - removals.count]
  }

  public func index(before index: Index) -> Index {
    return Index(_offset: index._offset - 1)
  }

  public func formIndex(_ index: inout Index, offsetBy distance: Int) {
    index = Index(_offset: index._offset + distance)
  }

  public func distance(from start: Index, to end: Index) -> Int {
    return end._offset - start._offset
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Index: Equatable {
  @inlinable
  public static func == (
    lhs: CollectionDifference.Index,
    rhs: CollectionDifference.Index
  ) -> Bool {
    return lhs._offset == rhs._offset
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Index: Comparable {
  @inlinable
  public static func < (
    lhs: CollectionDifference.Index,
    rhs: CollectionDifference.Index
  ) -> Bool {
    return lhs._offset < rhs._offset
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Index: Hashable {
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(_offset)
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Change: Equatable where ChangeElement: Equatable {}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference: Equatable where ChangeElement: Equatable {}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Change: Hashable where ChangeElement: Hashable {}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference: Hashable where ChangeElement: Hashable {}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference where ChangeElement: Hashable {
  /// Returns a new collection difference with associations between individual
  /// elements that have been removed and inserted only once.
  ///
  /// - Returns: A collection difference with all possible moves inferred.
  ///
  /// - Complexity: O(*n*) where *n* is the number of collection differences.
  public func inferringMoves() -> CollectionDifference<ChangeElement> {
    let uniqueRemovals: [ChangeElement:Int?] = {
      var result = [ChangeElement:Int?](minimumCapacity: Swift.min(removals.count, insertions.count))
      for removal in removals {
        let element = removal._element
        if result[element] != .none {
          result[element] = .some(.none)
        } else {
          result[element] = .some(removal._offset)
        }
      }
      return result.filter { (_, v) -> Bool in v != .none }
    }()
    
    let uniqueInsertions: [ChangeElement:Int?] = {
      var result = [ChangeElement:Int?](minimumCapacity: Swift.min(removals.count, insertions.count))
      for insertion in insertions {
        let element = insertion._element
        if result[element] != .none {
          result[element] = .some(.none)
        } else {
          result[element] = .some(insertion._offset)
        }
      }
      return result.filter { (_, v) -> Bool in v != .none }
    }()

    return CollectionDifference(_validatedChanges: map({ (change: Change) -> Change in
      switch change {
      case .remove(offset: let offset, element: let element, associatedWith: _):
        if uniqueRemovals[element] == nil {
          return change
        }
        if let assoc = uniqueInsertions[element] {
          return .remove(offset: offset, element: element, associatedWith: assoc)
        }
      case .insert(offset: let offset, element: let element, associatedWith: _):
        if uniqueInsertions[element] == nil {
          return change
        }
        if let assoc = uniqueRemovals[element] {
          return .insert(offset: offset, element: element, associatedWith: assoc)
        }
      }
      return change
    }))
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Change: Codable where ChangeElement: Codable {
  private enum _CodingKeys: String, CodingKey {
    case offset
    case element
    case associatedOffset
    case isRemove
  }

  public init(from decoder: Decoder) throws {
    let values = try decoder.container(keyedBy: _CodingKeys.self)
    let offset = try values.decode(Int.self, forKey: .offset)
    let element = try values.decode(ChangeElement.self, forKey: .element)
    let associatedOffset = try values.decode(Int?.self, forKey: .associatedOffset)
    let isRemove = try values.decode(Bool.self, forKey: .isRemove)
    if isRemove {
      self = .remove(offset: offset, element: element, associatedWith: associatedOffset)
    } else {
      self = .insert(offset: offset, element: element, associatedWith: associatedOffset)
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: _CodingKeys.self)
    switch self {
    case .remove(_, _, _):
      try container.encode(true, forKey: .isRemove)
    case .insert(_, _, _):
      try container.encode(false, forKey: .isRemove)
    }
    
    try container.encode(_offset, forKey: .offset)
    try container.encode(_element, forKey: .element)
    try container.encode(_associatedOffset, forKey: .associatedOffset)
  }
}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference: Codable where ChangeElement: Codable {}

@available(SwiftStdlib 5.1, *)
extension CollectionDifference: Sendable where ChangeElement: Sendable { }
@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Change: Sendable where ChangeElement: Sendable { }
@available(SwiftStdlib 5.1, *)
extension CollectionDifference.Index: Sendable where ChangeElement: Sendable { }

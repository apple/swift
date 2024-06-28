//===--- KeyValuePairs.swift ----------------------------------------------===//
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

/// A lightweight collection of key-value pairs.
///
/// Use a `KeyValuePairs` instance when you need an ordered collection of
/// key-value pairs and don't require the fast key lookup that the
/// `Dictionary` type provides. Unlike key-value pairs in a true dictionary,
/// neither the key nor the value of a `KeyValuePairs` instance must
/// conform to the `Hashable` protocol.
///
/// You initialize a `KeyValuePairs` instance using a Swift dictionary
/// literal. Besides maintaining the order of the original dictionary literal,
/// `KeyValuePairs` also allows duplicates keys. For example:
///
///     let recordTimes: KeyValuePairs = ["Florence Griffith-Joyner": 10.49,
///                                           "Evelyn Ashford": 10.76,
///                                           "Evelyn Ashford": 10.79,
///                                           "Marlies Gohr": 10.81]
///     print(recordTimes.first!)
///     // Prints "(key: "Florence Griffith-Joyner", value: 10.49)"
///
/// Some operations that are efficient on a dictionary are slower when using
/// `KeyValuePairs`. In particular, to find the value matching a key, you
/// must search through every element of the collection. The call to
/// `firstIndex(where:)` in the following example must traverse the whole
/// collection to find the element that matches the predicate:
///
///     let runner = "Marlies Gohr"
///     if let index = recordTimes.firstIndex(where: { $0.0 == runner }) {
///         let time = recordTimes[index].1
///         print("\(runner) set a 100m record of \(time) seconds.")
///     } else {
///         print("\(runner) couldn't be found in the records.")
///     }
///     // Prints "Marlies Gohr set a 100m record of 10.81 seconds."
///
/// Key-Value Pairs as a Function Parameter
/// ---------------------------------------
///
/// When calling a function with a `KeyValuePairs` parameter, you can pass
/// a Swift dictionary literal without causing a `Dictionary` to be created.
/// This capability can be especially important when the order of elements in
/// the literal is significant.
///
/// For example, you could create an `IntPairs` structure that holds a list of
/// two-integer tuples and use an initializer that accepts a
/// `KeyValuePairs` instance.
///
///     struct IntPairs {
///         var elements: [(Int, Int)]
///
///         init(_ elements: KeyValuePairs<Int, Int>) {
///             self.elements = Array(elements)
///         }
///     }
///
/// When you're ready to create a new `IntPairs` instance, use a dictionary
/// literal as the parameter to the `IntPairs` initializer. The
/// `KeyValuePairs` instance preserves the order of the elements as
/// passed.
///
///     let pairs = IntPairs([1: 2, 1: 1, 3: 4, 2: 1])
///     print(pairs.elements)
///     // Prints "[(1, 2), (1, 1), (3, 4), (2, 1)]"
@frozen // trivial-implementation
public struct KeyValuePairs<Key, Value>: ExpressibleByDictionaryLiteral {
  @usableFromInline // trivial-implementation
  internal let _elements: [(Key, Value)]

  /// Creates a new `KeyValuePairs` instance from the given dictionary
  /// literal.
  ///
  /// The order of the key-value pairs is kept intact in the resulting
  /// `KeyValuePairs` instance.
  @inlinable // trivial-implementation
  public init(dictionaryLiteral elements: (Key, Value)...) {
    self._elements = elements
  }
}

/// `Collection` conformance that allows `KeyValuePairs` to
/// interoperate with the rest of the standard library.
extension KeyValuePairs: RandomAccessCollection {
  /// The element type of a `KeyValuePairs`: a tuple containing an
  /// individual key-value pair.
  public typealias Element = (key: Key, value: Value)
  public typealias Index = Int
  public typealias Indices = Range<Int>
  public typealias SubSequence = Slice<KeyValuePairs>
  
  /// The position of the first element in a nonempty collection.
  ///
  /// If the `KeyValuePairs` instance is empty, `startIndex` is equal to
  /// `endIndex`.
  @inlinable // trivial-implementation
  public var startIndex: Index { return 0 }

  /// The collection's "past the end" position---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// If the `KeyValuePairs` instance is empty, `endIndex` is equal to
  /// `startIndex`.
  @inlinable // trivial-implementation
  public var endIndex: Index { return _elements.endIndex }

  /// Accesses the element at the specified position.
  ///
  /// - Parameter position: The position of the element to access. `position`
  ///   must be a valid index of the collection that is not equal to the
  ///   `endIndex` property.
  /// - Returns: The key-value pair at position `position`.
  @inlinable // trivial-implementation
  public subscript(position: Index) -> Element {
    return _elements[position]
  }
}

@_unavailableInEmbedded
extension KeyValuePairs: CustomStringConvertible {
  /// A string that represents the contents of the dictionary.
  public var description: String {
    return _makeKeyValuePairDescription()
  }
}

@_unavailableInEmbedded
extension KeyValuePairs: CustomDebugStringConvertible {
  /// A string that represents the contents of the dictionary, suitable for
  /// debugging.
  public var debugDescription: String {
    return _makeKeyValuePairDescription()
  }
}

extension KeyValuePairs: Sendable
    where Key: Sendable, Value: Sendable { }

//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift Atomics open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Builtin

/// A pair of two word sized `UInt`s.
///
/// This type's primary purpose is to be used in double wide atomic operations.
/// On platforms that support it, atomic operations on `WordPair` are done in a
/// single operation for two words. Users can use this type as itself when used
/// on `Atomic`, or it could be used as an intermediate step for custom
/// `AtomicRepresentable` types that are also double wide.
///
///     let atomicPair = Atomic<WordPair>(WordPair(first: 0, second: 0))
///     atomicPair.store(WordPair(first: someVersion, second: .max), ordering: .relaxed)
///
/// When used as an intermediate step for custom `AtomicRepresentable` types, it
/// is critical that their `AtomicRepresentation` be equal to
/// `WordPair.AtomicRepresentation`.
///
///     struct GridPoint {
///       var x: Int
///       var y: Int
///     }
///
///     extension GridPoint: AtomicRepresentable {
///       typealias AtomicRepresentation = WordPair.AtomicRepresentation
///
///       ...
///     }
///
/// - Note: This type only conforms to `AtomicRepresentable` on platforms that
///   support double wide atomics.
@available(SwiftStdlib 6.0, *)
@frozen
public struct WordPair {
  /// The first element in this word pair.
  public var first: UInt

  /// The second element in this word pair.
  public var second: UInt

  /// Initialize a new `WordPair` value given both individual words.
  ///
  /// - Parameter first: The first word to use in the pair.
  /// - Parameter second: The second word to use in the pair.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(first: UInt, second: UInt) {
    self.first = first
    self.second = second
  }
}

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || (_pointerBitWidth(_64) && _hasAtomicBitWidth(_128))

@available(SwiftStdlib 6.0, *)
extension WordPair: AtomicRepresentable {
#if _pointerBitWidth(_64)
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  public typealias AtomicRepresentation = _Atomic128BitStorage
#elseif _pointerBitWidth(_32)
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  public typealias AtomicRepresentation = _Atomic64BitStorage
#else
#error("Unsupported platform")
#endif

  /// Destroys a value of `Self` and prepares an `AtomicRepresentation` storage
  /// type to be used for atomic operations.
  ///
  /// - Note: This is not an atomic operation. This simply encodes the logical
  ///   type `Self` into its storage representation suitable for atomic
  ///   operations, `AtomicRepresentation`.
  ///
  /// - Parameter value: A valid instance of `Self` that's about to be destroyed
  ///   to encode an instance of its `AtomicRepresentation`.
  /// - Returns: The newly encoded `AtomicRepresentation` storage.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public static func encodeAtomicRepresentation(
    _ value: consuming WordPair
  ) -> AtomicRepresentation {
#if _pointerBitWidth(_64)
    var i128 = Builtin.zext_Int64_Int128(value.first._value)
    var high128 = Builtin.zext_Int64_Int128(value.second._value)
    let highShift = Builtin.zext_Int64_Int128(UInt(64)._value)
    high128 = Builtin.shl_Int128(high128, highShift)
    i128 = Builtin.or_Int128(i128, high128)

    return AtomicRepresentation(i128)
#elseif _pointerBitWidth(_32)
    var i64 = Builtin.zext_Int32_Int64(value.first._value)
    var high64 = Builtin.zext_Int32_Int64(value.second._value)
    let highShift = Builtin.zext_Int32_Int64(UInt(32)._value)
    high64 = Builtin.shl_Int64(high64, highShift)
    i64 = Builtin.or_Int64(i64, high64)

    return AtomicRepresentation(i64)
#else
#error("Unsupported platform")
#endif
  }

  /// Recovers the logical atomic type `Self` by destroying some
  /// `AtomicRepresentation` storage instance returned from an atomic operation.
  ///
  /// - Note: This is not an atomic operation. This simply decodes the storage
  ///   representation used in atomic operations back into the logical type for
  ///   normal use, `Self`.
  ///
  /// - Parameter storage: The storage representation for `Self` that's used
  ///   within atomic operations.
  /// - Returns: The newly decoded logical type `Self`.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public static func decodeAtomicRepresentation(
    _ representation: consuming AtomicRepresentation
  ) -> WordPair {
#if _pointerBitWidth(_64)
    let highShift = Builtin.zext_Int64_Int128(UInt(64)._value)
    let high128 = Builtin.lshr_Int128(representation._storage, highShift)
    let high = Builtin.trunc_Int128_Int64(high128)
    let low = Builtin.trunc_Int128_Int64(representation._storage)
#elseif _pointerBitWidth(_32)
    let highShift = Builtin.zext_Int32_Int64(UInt(32)._value)
    let high64 = Builtin.lshr_Int64(representation._storage, highShift)
    let high = Builtin.trunc_Int64_Int32(high64)
    let low = Builtin.trunc_Int64_Int32(representation._storage)
#else
#error("Unsupported platform")
#endif

    return WordPair(first: UInt(low), second: UInt(high))
  }
}

#endif

@available(SwiftStdlib 6.0, *)
extension WordPair: Equatable {
  /// Compares two values of this type to determine if they are equivalent to
  /// each other.
  ///
  /// - Parameter lhs: The first value to compare.
  /// - Parameter rhs: The second value to compare.
  /// - Returns: True if both values were equal, or false if they were unequal.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public static func ==(lhs: WordPair, rhs: WordPair) -> Bool {
    lhs.first == rhs.first && lhs.second == rhs.second
  }
}

@available(SwiftStdlib 6.0, *)
extension WordPair: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public func hash(into hasher: inout Hasher) {
    hasher.combine(first)
    hasher.combine(second)
  }
}

@available(SwiftStdlib 6.0, *)
@_unavailableInEmbedded
extension WordPair: CustomStringConvertible {
  /// A string that represents the contents of the word pair.
  @available(SwiftStdlib 6.0, *)
  public var description: String {
    "WordPair(first: \(first), second: \(second))"
  }
}

@available(SwiftStdlib 6.0, *)
@_unavailableInEmbedded
extension WordPair: CustomDebugStringConvertible {
  /// A string that represents the contents of the word pair, suitable for
  /// debugging.
  @available(SwiftStdlib 6.0, *)
  public var debugDescription: String {
    "WordPair(first: \(first), second: \(second))"
  }
}

@available(SwiftStdlib 6.0, *)
extension WordPair: Sendable {}

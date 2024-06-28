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

/// An atomic value that also supports atomic operations when wrapped
/// in an `Optional`. Atomic optional representable types come with a standalone
/// atomic representation for their optional-wrapped variants.
@available(SwiftStdlib 6.0, *)
public protocol AtomicOptionalRepresentable: AtomicRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  associatedtype AtomicOptionalRepresentation: BitwiseCopyable

  /// Destroys a value of `Self` and prepares an `AtomicOptionalRepresentation`
  /// storage type to be used for atomic operations on `Optional`.
  ///
  /// - Note: This is not an atomic operation. This simply encodes the logical
  ///   type `Self` into its storage representation suitable for atomic
  ///   operations, `AtomicOptionalRepresentation`.
  ///
  /// - Parameter value: An optional instance of `Self` that's about to be
  ///   destroyed to encode an instance of its `AtomicOptionalRepresentation`.
  /// - Returns: The newly encoded `AtomicOptionalRepresentation` storage.
  static func encodeAtomicOptionalRepresentation(
    _ value: consuming Self?
  ) -> AtomicOptionalRepresentation

  /// Recovers the logical atomic type `Self?` by destroying some
  /// `AtomicOptionalRepresentation` storage instance returned from an atomic
  /// operation on `Optional`.
  ///
  /// - Note: This is not an atomic operation. This simply decodes the storage
  ///   representation used in atomic operations on `Optional` back into the
  ///   logical type for normal use, `Self?`.
  ///
  /// - Parameter storage: The optional storage representation for `Self?`
  ///   that's used within atomic operations on `Optional`.
  /// - Returns: The newly decoded logical type `Self?`.
  static func decodeAtomicOptionalRepresentation(
    _ representation: consuming AtomicOptionalRepresentation
  ) -> Self?
}

//===----------------------------------------------------------------------===//
// RawRepresentable AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension RawRepresentable
where
  Self: AtomicOptionalRepresentable,
  RawValue: AtomicOptionalRepresentable
{
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = RawValue.AtomicOptionalRepresentation

  /// Destroys a value of `Self` and prepares an `AtomicOptionalRepresentation`
  /// storage type to be used for atomic operations on `Optional`.
  ///
  /// - Note: This is not an atomic operation. This simply encodes the logical
  ///   type `Self` into its storage representation suitable for atomic
  ///   operations, `AtomicOptionalRepresentation`.
  ///
  /// - Parameter value: An optional instance of `Self` that's about to be
  ///   destroyed to encode an instance of its `AtomicOptionalRepresentation`.
  /// - Returns: The newly encoded `AtomicOptionalRepresentation` storage.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public static func encodeAtomicOptionalRepresentation(
    _ value: consuming Self?
  ) -> RawValue.AtomicOptionalRepresentation {
    // FIXME: There is currently a compiler crash with the following:
    //
    // RawValue.encodeAtomicOptionalRepresentation(value?.rawValue)

    if let value = value {
      return RawValue.encodeAtomicOptionalRepresentation(value.rawValue)
    }

    return RawValue.encodeAtomicOptionalRepresentation(nil)
  }

  /// Recovers the logical atomic type `Self?` by destroying some
  /// `AtomicOptionalRepresentation` storage instance returned from an atomic
  /// operation on `Optional`.
  ///
  /// - Note: This is not an atomic operation. This simply decodes the storage
  ///   representation used in atomic operations on `Optional` back into the
  ///   logical type for normal use, `Self?`.
  ///
  /// - Parameter storage: The optional storage representation for `Self?`
  ///   that's used within atomic operations on `Optional`.
  /// - Returns: The newly decoded logical type `Self?`.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public static func decodeAtomicOptionalRepresentation(
    _ representation: consuming RawValue.AtomicOptionalRepresentation
  ) -> Self? {
    RawValue.decodeAtomicOptionalRepresentation(representation).flatMap {
      Self(rawValue: $0)
    }
  }
}

//===----------------------------------------------------------------------===//
// Optional AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension Optional: AtomicRepresentable where Wrapped: AtomicOptionalRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Wrapped.AtomicOptionalRepresentation

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
    _ value: consuming Wrapped?
  ) -> AtomicRepresentation {
    Wrapped.encodeAtomicOptionalRepresentation(value)
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
  ) -> Wrapped? {
    Wrapped.decodeAtomicOptionalRepresentation(representation)
  }
}

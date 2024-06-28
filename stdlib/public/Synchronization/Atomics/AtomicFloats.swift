//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Float16 AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))

@available(SwiftStdlib 6.0, *)
extension Float16: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = UInt16.AtomicRepresentation

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
    _ value: consuming Float16
  ) -> AtomicRepresentation {
    UInt16.encodeAtomicRepresentation(value.bitPattern)
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
  ) -> Float16 {
    Float16(bitPattern: UInt16.decodeAtomicRepresentation(representation))
  }
}

#endif

//===----------------------------------------------------------------------===//
// Float AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension Float: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = UInt32.AtomicRepresentation

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
    _ value: consuming Float
  ) -> AtomicRepresentation {
    UInt32.encodeAtomicRepresentation(value.bitPattern)
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
  ) -> Float {
    Float(bitPattern: UInt32.decodeAtomicRepresentation(representation))
  }
}

//===----------------------------------------------------------------------===//
// Double AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || _pointerBitWidth(_64)

@available(SwiftStdlib 6.0, *)
extension Double: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = UInt64.AtomicRepresentation

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
    _ value: consuming Double
  ) -> AtomicRepresentation {
    UInt64.encodeAtomicRepresentation(value.bitPattern)
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
  ) -> Double {
    Double(bitPattern: UInt64.decodeAtomicRepresentation(representation))
  }
}

#endif

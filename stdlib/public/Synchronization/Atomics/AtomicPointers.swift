//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// UnsafePointer AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension UnsafePointer: AtomicRepresentable where Pointee: ~Copyable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafePointer<Pointee>
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> UnsafePointer<Pointee> {
    UnsafePointer<Pointee>(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )!
  }
}

@available(SwiftStdlib 6.0, *)
extension UnsafePointer: AtomicOptionalRepresentable where Pointee: ~Copyable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafePointer<Pointee>?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> UnsafePointer<Pointee>? {
    UnsafePointer<Pointee>(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )
  }
}

//===----------------------------------------------------------------------===//
// UnsafeMutablePointer AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension UnsafeMutablePointer: AtomicRepresentable where Pointee: ~Copyable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeMutablePointer<Pointee>
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> UnsafeMutablePointer<Pointee> {
    UnsafeMutablePointer<Pointee>(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )!
  }
}

@available(SwiftStdlib 6.0, *)
extension UnsafeMutablePointer: AtomicOptionalRepresentable
where Pointee: ~Copyable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeMutablePointer<Pointee>?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> UnsafeMutablePointer<Pointee>? {
    UnsafeMutablePointer<Pointee>(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )
  }
}

//===----------------------------------------------------------------------===//
// UnsafeRawPointer AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension UnsafeRawPointer: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeRawPointer
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> UnsafeRawPointer {
    UnsafeRawPointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )!
  }
}

@available(SwiftStdlib 6.0, *)
extension UnsafeRawPointer: AtomicOptionalRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeRawPointer?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> UnsafeRawPointer? {
    UnsafeRawPointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )
  }
}

//===----------------------------------------------------------------------===//
// UnsafeMutableRawPointer AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension UnsafeMutableRawPointer: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeMutableRawPointer
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> UnsafeMutableRawPointer {
    UnsafeMutableRawPointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )!
  }
}

@available(SwiftStdlib 6.0, *)
extension UnsafeMutableRawPointer: AtomicOptionalRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming UnsafeMutableRawPointer?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> UnsafeMutableRawPointer? {
    UnsafeMutableRawPointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )
  }
}

//===----------------------------------------------------------------------===//
// Unmanaged AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension Unmanaged: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming Unmanaged<Instance>
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value.toOpaque())
    )
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
  ) -> Unmanaged<Instance> {
    Unmanaged<Instance>.fromOpaque(
      UnsafeRawPointer.decodeAtomicRepresentation(representation)
    )
  }
}

@available(SwiftStdlib 6.0, *)
extension Unmanaged: AtomicOptionalRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming Unmanaged<Instance>?
  ) -> AtomicOptionalRepresentation {
    // FIXME: The following leads to a compiler crash at the moment.
    //
    // Int.AtomicRepresentation(Int(bitPattern: value?.toOpaque())._value)

    if let unmanaged = value {
      return Int.encodeAtomicRepresentation(
        Int(bitPattern: unmanaged.toOpaque())
      )
    }

    return Int.AtomicRepresentation(0._value)
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> Unmanaged<Instance>? {
    UnsafeRawPointer.decodeAtomicOptionalRepresentation(representation).map {
      Unmanaged.fromOpaque($0)
    }
  }
}

//===----------------------------------------------------------------------===//
// OpaquePointer AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension OpaquePointer: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming OpaquePointer
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> OpaquePointer {
    OpaquePointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )!
  }
}

@available(SwiftStdlib 6.0, *)
extension OpaquePointer: AtomicOptionalRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming OpaquePointer?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> OpaquePointer? {
    OpaquePointer(
      bitPattern: Int.decodeAtomicRepresentation(representation)
    )
  }
}

//===----------------------------------------------------------------------===//
// ObjectIdentifier AtomicRepresentable and AtomicOptionalRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension ObjectIdentifier: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = Int.AtomicRepresentation

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
    _ value: consuming ObjectIdentifier
  ) -> AtomicRepresentation {
    Int.encodeAtomicRepresentation(
      Int(bitPattern: value)
    )
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
  ) -> ObjectIdentifier {
    // ObjectIdentifier doesn't have a bitPattern init..?
    unsafeBitCast(
      Int.decodeAtomicRepresentation(representation),
      to: ObjectIdentifier.self
    )
  }
}

@available(SwiftStdlib 6.0, *)
extension ObjectIdentifier: AtomicOptionalRepresentable {
  /// The storage representation type that encodes to and decodes from
  /// `Optional<Self>` which is a suitable type when used in atomic operations
  /// on `Optional`.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicOptionalRepresentation = Int.AtomicRepresentation

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
    _ value: consuming ObjectIdentifier?
  ) -> AtomicOptionalRepresentation {
    Int.encodeAtomicRepresentation(
      // {U}Int have bitPattern inits for ObjectIdentifier, but not optional
      // ObjectIdentifier :sad:
      unsafeBitCast(value, to: Int.self)
    )
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
    _ representation: consuming AtomicOptionalRepresentation
  ) -> ObjectIdentifier? {
    // ObjectIdentifier doesn't have a bitPattern init..?
    unsafeBitCast(
      Int.decodeAtomicRepresentation(representation),
      to: ObjectIdentifier?.self
    )
  }
}

//===----------------------------------------------------------------------===//
// UnsafeBufferPointer AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || (_pointerBitWidth(_64) && _hasAtomicBitWidth(_128))

@available(SwiftStdlib 6.0, *)
extension UnsafeBufferPointer: AtomicRepresentable where Element: ~Copyable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = WordPair.AtomicRepresentation

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
    _ value: consuming UnsafeBufferPointer<Element>
  ) -> AtomicRepresentation {
    let valueCopy = value

    return WordPair.encodeAtomicRepresentation(
      WordPair(
        first: UInt(bitPattern: valueCopy.baseAddress),
        second: UInt(truncatingIfNeeded: valueCopy.count)
      )
    )
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
  ) -> UnsafeBufferPointer<Element> {
    let wp = WordPair.decodeAtomicRepresentation(representation)

    return UnsafeBufferPointer<Element>(
      start: UnsafePointer<Element>(bitPattern: wp.first),
      count: Int(truncatingIfNeeded: wp.second)
    )
  }
}

#endif

//===----------------------------------------------------------------------===//
// UnsafeMutableBufferPointer AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || (_pointerBitWidth(_64) && _hasAtomicBitWidth(_128))

@available(SwiftStdlib 6.0, *)
extension UnsafeMutableBufferPointer: AtomicRepresentable
where Element: ~Copyable
{
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = WordPair.AtomicRepresentation

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
    _ value: consuming UnsafeMutableBufferPointer<Element>
  ) -> AtomicRepresentation {
    let valueCopy = value

    return WordPair.encodeAtomicRepresentation(
      WordPair(
        first: UInt(bitPattern: valueCopy.baseAddress),
        second: UInt(truncatingIfNeeded: valueCopy.count)
      )
    )
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
  ) -> UnsafeMutableBufferPointer<Element> {
    let wp = WordPair.decodeAtomicRepresentation(representation)

    return UnsafeMutableBufferPointer<Element>(
      start: UnsafeMutablePointer<Element>(bitPattern: wp.first),
      count: Int(truncatingIfNeeded: wp.second)
    )
  }
}

#endif

//===----------------------------------------------------------------------===//
// UnsafeRawBufferPointer AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || (_pointerBitWidth(_64) && _hasAtomicBitWidth(_128))

@available(SwiftStdlib 6.0, *)
extension UnsafeRawBufferPointer: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = WordPair.AtomicRepresentation

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
    _ value: consuming UnsafeRawBufferPointer
  ) -> AtomicRepresentation {
    let valueCopy = value

    return WordPair.encodeAtomicRepresentation(
      WordPair(
        first: UInt(bitPattern: valueCopy.baseAddress),
        second: UInt(truncatingIfNeeded: valueCopy.count)
      )
    )
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
  ) -> UnsafeRawBufferPointer {
    let wp = WordPair.decodeAtomicRepresentation(representation)

    return UnsafeRawBufferPointer(
      start: UnsafeRawPointer(bitPattern: wp.first),
      count: Int(truncatingIfNeeded: wp.second)
    )
  }
}

#endif

//===----------------------------------------------------------------------===//
// UnsafeMutableRawBufferPointer AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

#if (_pointerBitWidth(_32) && _hasAtomicBitWidth(_64)) || (_pointerBitWidth(_64) && _hasAtomicBitWidth(_128))

@available(SwiftStdlib 6.0, *)
extension UnsafeMutableRawBufferPointer: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = WordPair.AtomicRepresentation

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
    _ value: consuming UnsafeMutableRawBufferPointer
  ) -> AtomicRepresentation {
    let valueCopy = value

    return WordPair.encodeAtomicRepresentation(
      WordPair(
        first: UInt(bitPattern: valueCopy.baseAddress),
        second: UInt(truncatingIfNeeded: valueCopy.count)
      )
    )
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
  ) -> UnsafeMutableRawBufferPointer {
    let wp = WordPair.decodeAtomicRepresentation(representation)

    return UnsafeMutableRawBufferPointer(
      start: UnsafeMutableRawPointer(bitPattern: wp.first),
      count: Int(truncatingIfNeeded: wp.second)
    )
  }
}

#endif

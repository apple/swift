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

import Builtin

//===----------------------------------------------------------------------===//
// Bool AtomicRepresentable conformance
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension Bool: AtomicRepresentable {
  /// The storage representation type that `Self` encodes to and decodes from
  /// which is a suitable type when used in atomic operations.
  @available(SwiftStdlib 6.0, *)
  public typealias AtomicRepresentation = UInt8.AtomicRepresentation

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
    _ value: borrowing Bool
  ) -> AtomicRepresentation {
    UInt8.encodeAtomicRepresentation(
      UInt8(Builtin.zext_Int1_Int8(value._value))
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
  ) -> Bool {
    Bool(Builtin.trunc_Int8_Int1(
      UInt8.decodeAtomicRepresentation(representation)._value)
    )
  }
}

//===----------------------------------------------------------------------===//
// Bool atomic operations
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 6.0, *)
extension Atomic where Value == Bool {
  /// Perform an atomic logical AND operation and return the old and new value,
  /// applying the specified memory ordering.
  ///
  /// - Parameter operand: A boolean value.
  /// - Parameter ordering: The memory ordering to apply on this operation.
  /// - Returns: A tuple with the old value before the operation a the new value
  ///   after the operation.
  @available(SwiftStdlib 6.0, *)
  @discardableResult
  @_semantics("atomics.requires_constant_orderings")
  @_alwaysEmitIntoClient
  @_transparent
  public func logicalAnd(
    _ operand: Bool,
    ordering: AtomicUpdateOrdering
  ) -> (oldValue: Bool, newValue: Bool) {
    let builtinOperand = Bool.encodeAtomicRepresentation(operand)._storage

    let original = switch ordering {
    case .relaxed:
      Builtin.atomicrmw_and_monotonic_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiring:
      Builtin.atomicrmw_and_acquire_Int8(
        _rawAddress,
        builtinOperand
      )

    case .releasing:
      Builtin.atomicrmw_and_release_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiringAndReleasing:
      Builtin.atomicrmw_and_acqrel_Int8(
        _rawAddress,
        builtinOperand
      )

    case .sequentiallyConsistent:
      Builtin.atomicrmw_and_seqcst_Int8(
        _rawAddress,
        builtinOperand
      )

    default:
      Builtin.unreachable()
    }

    let old = Bool.decodeAtomicRepresentation(UInt8.AtomicRepresentation(original))

    return (oldValue: old, newValue: old && operand)
  }

  /// Perform an atomic logical OR operation and return the old and new value,
  /// applying the specified memory ordering.
  ///
  /// - Parameter operand: A boolean value.
  /// - Parameter ordering: The memory ordering to apply on this operation.
  /// - Returns: A tuple with the old value before the operation a the new value
  ///   after the operation.
  @available(SwiftStdlib 6.0, *)
  @discardableResult
  @_semantics("atomics.requires_constant_orderings")
  @_alwaysEmitIntoClient
  @_transparent
  public func logicalOr(
    _ operand: Bool,
    ordering: AtomicUpdateOrdering
  ) -> (oldValue: Bool, newValue: Bool) {
    let builtinOperand = Bool.encodeAtomicRepresentation(operand)._storage

    let original = switch ordering {
    case .relaxed:
      Builtin.atomicrmw_or_monotonic_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiring:
      Builtin.atomicrmw_or_acquire_Int8(
        _rawAddress,
        builtinOperand
      )

    case .releasing:
      Builtin.atomicrmw_or_release_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiringAndReleasing:
      Builtin.atomicrmw_or_acqrel_Int8(
        _rawAddress,
        builtinOperand
      )

    case .sequentiallyConsistent:
      Builtin.atomicrmw_or_seqcst_Int8(
        _rawAddress,
        builtinOperand
      )

    default:
      Builtin.unreachable()
    }

    let old = Bool.decodeAtomicRepresentation(UInt8.AtomicRepresentation(original))

    return (oldValue: old, newValue: old || operand)
  }

  /// Perform an atomic logical XOR operation and return the old and new value,
  /// applying the specified memory ordering.
  ///
  /// - Parameter operand: A boolean value.
  /// - Parameter ordering: The memory ordering to apply on this operation.
  /// - Returns: A tuple with the old value before the operation a the new value
  ///   after the operation.
  @available(SwiftStdlib 6.0, *)
  @discardableResult
  @_semantics("atomics.requires_constant_orderings")
  @_alwaysEmitIntoClient
  @_transparent
  public func logicalXor(
    _ operand: Bool,
    ordering: AtomicUpdateOrdering
  ) -> (oldValue: Bool, newValue: Bool) {
    let builtinOperand = Bool.encodeAtomicRepresentation(operand)._storage

    let original = switch ordering {
    case .relaxed:
      Builtin.atomicrmw_xor_monotonic_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiring:
      Builtin.atomicrmw_xor_acquire_Int8(
        _rawAddress,
        builtinOperand
      )

    case .releasing:
      Builtin.atomicrmw_xor_release_Int8(
        _rawAddress,
        builtinOperand
      )

    case .acquiringAndReleasing:
      Builtin.atomicrmw_xor_acqrel_Int8(
        _rawAddress,
        builtinOperand
      )

    case .sequentiallyConsistent:
      Builtin.atomicrmw_xor_seqcst_Int8(
        _rawAddress,
        builtinOperand
      )

    default:
      Builtin.unreachable()
    }

    let old = Bool.decodeAtomicRepresentation(UInt8.AtomicRepresentation(original))

    return (oldValue: old, newValue: old != operand)
  }
}

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

//===----------------------------------------------------------------------===//
// Load Orderings
//===----------------------------------------------------------------------===//

/// Specifies the memory ordering semantics of an atomic load operation.
@available(SwiftStdlib 6.0, *)
@frozen
public struct AtomicLoadOrdering {
  @usableFromInline
  internal var _rawValue: Int

  @available(SwiftStdlib 6.0, *)
  @inlinable
  @_semantics("constant_evaluable")
  @_transparent
  internal init(_rawValue: Int) {
    self._rawValue = _rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLoadOrdering {
  // FIXME: Explain these ordering levels in more detail.

  /// Guarantees the atomicity of the specific operation on which it is applied,
  /// but imposes no ordering constraints on any other variable accesses.
  ///
  /// This value corresponds to `std::memory_order_relaxed` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var relaxed: Self {
    Self(_rawValue: 0)
  }

  /// An acquiring load synchronizes with a releasing operation whose
  /// value its reads. It ensures that the releasing and acquiring
  /// threads agree that all subsequent variable accesses on the
  /// acquiring thread happen after the atomic operation itself.
  ///
  /// This value corresponds to `std::memory_order_acquire` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var acquiring: Self {
    Self(_rawValue: 2)
  }

  /// A sequentially consistent load performs an acquiring load and
  /// also guarantees that it and all other sequentially consistent
  /// atomic operations (loads, stores, updates) appear to be executed
  /// in a single, total sequential ordering.
  ///
  /// This value corresponds to `std::memory_order_seq_cst` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var sequentiallyConsistent: Self {
    Self(_rawValue: 5)
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLoadOrdering: Equatable {
  @available(SwiftStdlib 6.0, *)
  @_transparent
  public static func ==(left: Self, right: Self) -> Bool {
    left._rawValue == right._rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLoadOrdering: Hashable {
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(_rawValue)
  }
}

@available(SwiftStdlib 6.0, *)
@_unavailableInEmbedded
extension AtomicLoadOrdering: CustomStringConvertible {
  @available(SwiftStdlib 6.0, *)
  public var description: String {
    switch self {
    case .relaxed: return "relaxed"
    case .acquiring: return "acquiring"
    case .sequentiallyConsistent: return "sequentiallyConsistent"
    default: return "AtomicLoadOrdering(\(_rawValue))"
    }
  }
}

//===----------------------------------------------------------------------===//
// Store Orderings
//===----------------------------------------------------------------------===//

/// Specifies the memory ordering semantics of an atomic store operation.
@available(SwiftStdlib 6.0, *)
@frozen
public struct AtomicStoreOrdering {
  @usableFromInline
  internal var _rawValue: Int

  @available(SwiftStdlib 6.0, *)
  @inlinable
  @_semantics("constant_evaluable")
  @_transparent
  internal init(_rawValue: Int) {
    self._rawValue = _rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicStoreOrdering {
  // FIXME: Explain these ordering levels in more detail.

  /// Guarantees the atomicity of the specific operation on which it is applied,
  /// but imposes no ordering constraints on any other variable accesses.
  ///
  /// This value corresponds to `std::memory_order_relaxed` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var relaxed: Self {
    Self(_rawValue: 0)
  }

  /// A releasing store synchronizes with acquiring operations that
  /// read the value it stores. It ensures that the releasing and
  /// acquiring threads agree that all preceding variable accesses on
  /// the releasing thread happen before the atomic operation itself.
  ///
  /// This value corresponds to `std::memory_order_release` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var releasing: Self {
    Self(_rawValue: 3)
  }

  /// A sequentially consistent store performs a releasing store and
  /// also guarantees that it and all other sequentially consistent
  /// atomic operations (loads, stores, updates) appear to be executed
  /// in a single, total sequential ordering.
  ///
  /// This value corresponds to `std::memory_order_seq_cst` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var sequentiallyConsistent: Self {
    Self(_rawValue: 5)
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicStoreOrdering: Equatable {
  @_transparent
  public static func ==(left: Self, right: Self) -> Bool {
    left._rawValue == right._rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicStoreOrdering: Hashable {
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(_rawValue)
  }
}

@available(SwiftStdlib 6.0, *)
@_unavailableInEmbedded
extension AtomicStoreOrdering: CustomStringConvertible {
  @available(SwiftStdlib 6.0, *)
  public var description: String {
    switch self {
    case .relaxed: return "relaxed"
    case .releasing: return "releasing"
    case .sequentiallyConsistent: return "sequentiallyConsistent"
    default: return "AtomicStoreOrdering(\(_rawValue))"
    }
  }
}

//===----------------------------------------------------------------------===//
// Update Orderings
//===----------------------------------------------------------------------===//

/// Specifies the memory ordering semantics of an atomic read-modify-write
/// operation.
@available(SwiftStdlib 6.0, *)
@frozen
public struct AtomicUpdateOrdering {
  @usableFromInline
  internal var _rawValue: Int

  @available(SwiftStdlib 6.0, *)
  @inlinable
  @_semantics("constant_evaluable")
  @_transparent
  internal init(_rawValue: Int) {
    self._rawValue = _rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicUpdateOrdering {
  // FIXME: Explain these ordering levels in more detail.

  /// Guarantees the atomicity of the specific operation on which it is applied,
  /// but imposes no ordering constraints on any other variable accesses.
  ///
  /// This value corresponds to `std::memory_order_relaxed` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var relaxed: Self {
    Self(_rawValue: 0)
  }

  /// An acquiring update synchronizes with a releasing operation
  /// whose value its reads. It ensures that the releasing and
  /// acquiring threads agree that all subsequent variable accesses on
  /// the acquring thread happen after the atomic operation itself.
  ///
  /// This value corresponds to `std::memory_order_acquire` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var acquiring: Self {
    Self(_rawValue: 2)
  }

  /// A releasing update synchronizes with acquiring operations that
  /// read the value it stores. It ensures that the releasing and
  /// acquiring threads agree that all preceding variable accesses on
  /// the releasing thread happen before the atomic operation itself.
  ///
  /// This value corresponds to `std::memory_order_release` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var releasing: Self {
    Self(_rawValue: 3)
  }

  /// An acquiring-and-releasing operation is a combination of
  /// `.acquiring` and `.releasing` operation on the same variable.
  ///
  /// This value corresponds to `std::memory_order_acq_rel` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var acquiringAndReleasing: Self {
    Self(_rawValue: 4)
  }

  /// A sequentially consistent update performs an
  /// acquiring-and-releasing update and also guarantees that it and
  /// all other sequentially consistent atomic operations (loads, stores,
  /// updates) appear to be executed in a single, total sequential
  /// ordering.
  ///
  /// This value corresponds to `std::memory_order_seq_cst` in C++.
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_alwaysEmitIntoClient
  @_transparent
  public static var sequentiallyConsistent: Self {
    Self(_rawValue: 5)
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicUpdateOrdering: Equatable {
  @available(SwiftStdlib 6.0, *)
  @_transparent
  public static func ==(left: Self, right: Self) -> Bool {
    left._rawValue == right._rawValue
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicUpdateOrdering: Hashable {
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(_rawValue)
  }
}

@available(SwiftStdlib 6.0, *)
@_unavailableInEmbedded
extension AtomicUpdateOrdering: CustomStringConvertible {
  @available(SwiftStdlib 6.0, *)
  public var description: String {
    switch self {
    case .relaxed: return "relaxed"
    case .acquiring: return "acquiring"
    case .releasing: return "releasing"
    case .acquiringAndReleasing: return "acquiringAndReleasing"
    case .sequentiallyConsistent: return "sequentiallyConsistent"
    default: return "AtomicUpdateOrdering(\(_rawValue))"
    }
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLoadOrdering {
  @available(SwiftStdlib 6.0, *)
  @_semantics("constant_evaluable")
  @_semantics("atomics.requires_constant_orderings")
  @_alwaysEmitIntoClient
  @_transparent
  static func _failureOrdering(
    for ordering: AtomicUpdateOrdering
  ) -> AtomicLoadOrdering {
    switch ordering {
    case .relaxed: return .relaxed
    case .acquiring: return .acquiring
    case .releasing: return .relaxed
    case .acquiringAndReleasing: return .acquiring
    case .sequentiallyConsistent: return .sequentiallyConsistent
    default: fatalError("Unsupported ordering")
    }
  }
}

//===----------------------------------------------------------------------===//
// Atomic Memory Fence
//===----------------------------------------------------------------------===//

/// Establishes a memory ordering without associating it with a
/// particular atomic operation.
///
/// - A relaxed fence has no effect.
/// - An acquiring fence ties to any preceding atomic operation that
///   reads a value, and synchronizes with any releasing operation whose
///   value was read.
/// - A releasing fence ties to any subsequent atomic operation that
///   modifies a value, and synchronizes with any acquiring operation
///   that reads the result.
/// - An acquiring and releasing fence is a combination of an
///   acquiring and a releasing fence.
/// - A sequentially consistent fence behaves like an acquiring and
///   releasing fence, and ensures that the fence itself is part of
///   the single, total ordering for all sequentially consistent
///   operations.
///
/// This operation corresponds to `std::atomic_thread_fence` in C++.
///
/// Be aware that Thread Sanitizer does not support fences and may report
/// false-positive races for data protected by a fence.
@available(SwiftStdlib 6.0, *)
@_semantics("atomics.requires_constant_orderings")
@_alwaysEmitIntoClient
@_transparent
public func atomicMemoryFence(
  ordering: AtomicUpdateOrdering
) {
  switch ordering {
  case .relaxed:
    return

  case .acquiring:
    Builtin.fence_acquire()

  case .releasing:
    Builtin.fence_release()

  case .acquiringAndReleasing:
    Builtin.fence_acqrel()

  case .sequentiallyConsistent:
    Builtin.fence_seqcst()

  default:
    Builtin.unreachable()
  }
}

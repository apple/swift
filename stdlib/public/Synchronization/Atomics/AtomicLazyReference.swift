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

/// A lazily initializable atomic strong reference.
///
/// These values can be set (initialized) exactly once, but read many
/// times.
@available(SwiftStdlib 6.0, *)
@frozen
@_staticExclusiveOnly
public struct AtomicLazyReference<Instance: AnyObject>: ~Copyable {
  @usableFromInline
  let storage: Atomic<Unmanaged<Instance>?>

  @available(SwiftStdlib 6.0, *)
  @inlinable
  public init() {
    storage = Atomic<Unmanaged<Instance>?>(nil)
  }

  @inlinable
  deinit {
    if let unmanaged = storage.load(ordering: .acquiring) {
      unmanaged.release()
    }
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLazyReference {
  /// Atomically initializes this reference if its current value is nil, then
  /// returns the initialized value. If this reference is already initialized,
  /// then `storeIfNil(_:)` discards its supplied argument and returns the
  /// current value without updating it.
  ///
  /// The following example demonstrates how this can be used to implement a
  /// thread-safe lazily initialized reference:
  ///
  ///     class Image {
  ///       let _histogram = AtomicLazyReference<Histogram>()
  ///
  ///       // This is safe to call concurrently from multiple threads.
  ///       var atomicLazyHistogram: Histogram {
  ///         if let histogram = _histogram.load() { return histogram }
  ///         // Note that code here may run concurrently on
  ///         // multiple threads, but only one of them will get to
  ///         // succeed setting the reference.
  ///         let histogram = ...
  ///         return _histogram.storeIfNil(histogram)
  ///       }
  ///     }
  ///
  /// - Note: This operation uses acquiring-and-releasing memory ordering.
  ///
  /// - Parameter desired: A value of `Instance` that we will attempt to store
  ///   if the lazy reference is currently nil.
  /// - Returns: The value of `Instance` that was successfully stored within the
  ///   lazy reference. This may or may not be the same value of `Instance` that
  ///   was passed to this function.
  @available(SwiftStdlib 6.0, *)
  public func storeIfNil(_ desired: consuming Instance) -> Instance {
    let desiredUnmanaged = Unmanaged.passRetained(desired)
    let (exchanged, current) = storage.compareExchange(
      expected: nil,
      desired: desiredUnmanaged,
      ordering: .acquiringAndReleasing
    )

    if !exchanged {
      // The reference has already been initialized. Balance the retain that we
      // performed on 'desired'.
      desiredUnmanaged.release()
      return current!.takeUnretainedValue()
    }

    return desiredUnmanaged.takeUnretainedValue()
  }

  /// Atomically loads and returns the current value of this reference.
  ///
  /// - Note: The load operation is performed with the memory ordering
  ///   `AtomicLoadOrdering.acquiring`.
  ///
  /// - Returns: A value of `Instance` if the lazy reference was written to, or
  ///   `nil` if it has not been written to yet.
  @available(SwiftStdlib 6.0, *)
  public func load() -> Instance? {
    let value = storage.load(ordering: .acquiring)
    return value?.takeUnretainedValue()
  }
}

@available(SwiftStdlib 6.0, *)
extension AtomicLazyReference: @unchecked Sendable where Instance: Sendable {}

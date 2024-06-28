//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift Atomics open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A synchronization primitive that protects shared mutable state via
/// mutual exclusion.
///
/// The `Mutex` type offers non-recursive exclusive access to the state
/// it is protecting by blocking threads attempting to acquire the lock.
/// Only one execution context at a time has access to the value stored
/// within the `Mutex` allowing for exclusive access.
///
/// An example use of `Mutex` in a class used simultaneously by many
/// threads protecting a `Dictionary` value:
///
///     class Manager {
///       let cache = Mutex<[Key: Resource]>([:])
///
///       func saveResouce(_ resource: Resouce, as key: Key) {
///         cache.withLock {
///           $0[key] = resource
///         }
///       }
///     }
///
@available(SwiftStdlib 6.0, *)
@frozen
@_staticExclusiveOnly
public struct Mutex<Value: ~Copyable>: ~Copyable {
  @usableFromInline
  let handle = _MutexHandle()

  @usableFromInline
  let value: _Cell<Value>

  /// Initializes a value of this mutex with the given initial state.
  ///
  /// - Parameter initialValue: The initial value to give to the mutex.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(_ initialValue: consuming sending Value) {
    value = _Cell(initialValue)
  }
}

@available(SwiftStdlib 6.0, *)
extension Mutex: @unchecked Sendable where Value: ~Copyable {}

@available(SwiftStdlib 6.0, *)
extension Mutex where Value: ~Copyable {
  /// Calls the given closure after acquring the lock and then releases
  /// ownership.
  ///
  /// This method is equivalent to the following sequence of code:
  ///
  ///     mutex.lock()
  ///     defer {
  ///       mutex.unlock()
  ///     }
  ///     return try body(&value)
  ///
  /// - Warning: Recursive calls to `withLock` within the
  ///   closure parameter has behavior that is platform dependent.
  ///   Some platforms may choose to panic the process, deadlock,
  ///   or leave this behavior unspecified. This will never
  ///   reacquire the lock however.
  ///
  /// - Parameter body: A closure with a parameter of `Value`
  ///   that has exclusive access to the value being stored within
  ///   this mutex. This closure is considered the critical section
  ///   as it will only be executed once the calling thread has
  ///   acquired the lock.
  ///
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func withLock<Result: ~Copyable, E: Error>(
    _ body: @Sendable (inout Value) throws(E) -> sending Result
  ) throws(E) -> sending Result {
    handle._lock()

    defer {
      handle._unlock()
    }

    return try body(&value._address.pointee)
  }

  /// Attempts to acquire the lock and then calls the given closure if
  /// successful.
  ///
  /// If the calling thread was successful in acquiring the lock, the
  /// closure will be executed and then immediately after it will
  /// release ownership of the lock. If we were unable to acquire the
  /// lock, this will return `nil`.
  ///
  /// This method is equivalent to the following sequence of code:
  ///
  ///     guard mutex.tryLock() else {
  ///       return nil
  ///     }
  ///     defer {
  ///       mutex.unlock()
  ///     }
  ///     return try body(&value)
  ///
  /// - Warning: Recursive calls to `withLockIfAvailable` within the
  ///   closure parameter has behavior that is platform dependent.
  ///   Some platforms may choose to panic the process, deadlock,
  ///   or leave this behavior unspecified. This will never
  ///   reacquire the lock however.
  ///
  /// - Parameter body: A closure with a parameter of `Value`
  ///   that has exclusive access to the value being stored within
  ///   this mutex. This closure is considered the critical section
  ///   as it will only be executed if the calling thread acquires
  ///   the lock.
  ///
  /// - Returns: The return value, if any, of the `body` closure parameter
  ///   or nil if the lock couldn't be acquired.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func withLockIfAvailable<Result: ~Copyable, E: Error>(
    _ body: @Sendable (inout Value) throws(E) -> sending Result
  ) throws(E) -> sending Result? {
    guard handle._tryLock() else {
      return nil
    }

    defer {
      handle._unlock()
    }

    return try body(&value._address.pointee)
  }
}

@available(SwiftStdlib 6.0, *)
extension Mutex where Value == Void {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func _unsafeLock() {
    handle._lock()
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func _unsafeTryLock() -> Bool {
    handle._tryLock()
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func _unsafeUnlock() {
    handle._unlock()
  }
}

@available(SwiftStdlib 6.0, *)
extension _MutexHandle {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func unsafeLock() {
    _lock()
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func unsafeTryLock() -> Bool {
    _tryLock()
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public borrowing func unsafeUnlock() {
    _unlock()
  }
}

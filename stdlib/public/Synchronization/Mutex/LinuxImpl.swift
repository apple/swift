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

import SynchronizationShims

extension Atomic where Value == UInt32 {
  borrowing func wait() {
    _swift_stdlib_wait(.init(rawAddress))
  }

  borrowing func wake() {
    _swift_stdlib_wake(.init(rawAddress))
  }
}

@available(SwiftStdlib 6.0, *)
@frozen
@usableFromInline
@_staticExclusiveOnly
internal struct _MutexHandle: ~Copyable {
  // There are only 3 different values that storage can hold at a single time.
  // 0: unlocked
  // TID: locked, current thread's id (uncontended)
  // (TID | SWIFT_FUTEX_WAITERS): locked, current thread's id (contended)
  @usableFromInline
  let storage: Atomic<UInt32>

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  init() {
    storage = Atomic(0)
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  borrowing func lock() {
    // TODO: Is it worth caching this value in TLS?
    let selfId = _swift_stdlib_gettid()

    // Note: We could probably merge this cas into a do/while style loop, but we
    // really want to perform the strong variant before attempting to do weak
    // ones in the loop.

    var (exchanged, state) = storage.compareExchange(
      expected: 0,
      desired: selfId,
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    )

    if _fastPath(exchanged) {
      // Locked!
      return
    }

    while !exchanged {
      // Clear the waiter bit, if we have one, and check to see if this is a
      // recursive call to lock.
      let currentOwner = state & ~SWIFT_FUTEX_WAITERS
      if _slowPath(currentOwner == selfId) {
        // TODO: Need a colder function.
        fatalError("Recursive call to lock")
      }

      // Block until unlock has been called. This will return early if the call
      // to unlock happened between attempting to acquire and attempting to
      // wait while nobody else managed to acquire it yet.
      storage.wait()

      (exchanged, state) = storage.weakCompareExchange(
        expected: 0,
        desired: selfId,
        successOrdering: .acquiring,
        failureOrdering: .relaxed
      )
    }

    // Locked!
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  borrowing func tryLock() -> Bool {
    storage.compareExchange(
      expected: 0,
      desired: _swift_stdlib_gettid(),
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    ).exchanged
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  borrowing func unlock() {
    // TODO: Is it worth caching this value in TLS?
    let selfId = _swift_stdlib_gettid()

    // Attempt to release the lock. We can only atomically release the lock in
    // user-space when there are no other waiters. If there are waiters, the
    // waiter bit is set and we need to inform the kernel that we're unlocking.
    let (exchanged, oldValue) = storage.compareExchange(
      expected: selfId,
      desired: 0,
      successOrdering: .releasing,
      failureOrdering: .relaxed
    )

    if _fastPath(exchanged) {
      // No waiters, unlocked!
      return
    }

    // Clear the waiter bit from the old value and check to ensure we were the
    // previous owner of the lock.
    let oldOwner = oldValue & ~SWIFT_FUTEX_WAITERS
    guard _fastPath(oldOwner == selfId) else {
      // Either we called unlock while being unacquired, or another thread who
      // didn't have ownership of the lock called unlock.
      if oldOwner == 0 {
        // TODO: Need a colder function.
        fatalError("Call to unlock on an already unlocked mutex")
      } else {
        fatalError("Call to unlock on thread who wasn't holding the lock")
      }
    }

    // Wake up the next highest priority waiter.
    storage.wake()
  }
}

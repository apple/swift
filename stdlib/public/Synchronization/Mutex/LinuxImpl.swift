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

import _SynchronizationShims
#if canImport(Android)
import Android
#else
import Glibc
#endif

extension Atomic where Value == UInt32 {
  // This returns 'false' on success and 'true' on error. Check 'errno' for the
  // specifc error value.
  internal borrowing func _futexLock() -> UInt32 {
    _swift_stdlib_futex_lock(.init(_rawAddress))
  }

  // This returns 'false' on success and 'true' on error. Check 'errno' for the
  // specifc error value.
  internal borrowing func _futexTryLock() -> UInt32 {
    _swift_stdlib_futex_trylock(.init(_rawAddress))
  }

  // This returns 'false' on success and 'true' on error. Check 'errno' for the
  // specific error value.
  internal borrowing func _futexUnlock() -> UInt32 {
    _swift_stdlib_futex_unlock(.init(_rawAddress))
  }
}

@available(SwiftStdlib 6.0, *)
@frozen
@_staticExclusiveOnly
public struct _MutexHandle: ~Copyable {
  // There are only 3 different values that storage can hold at a single time.
  // 0: unlocked
  // TID: locked, current thread's id (uncontended)
  // (TID | FUTEX_WAITERS): locked, current thread's id (contended)
  @usableFromInline
  let storage: Atomic<UInt32>

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init() {
    storage = Atomic(0)
  }
}

@available(SwiftStdlib 6.0, *)
extension _MutexHandle {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal borrowing func _lock() {
    // Note: This is being TLS cached.
    let selfId = _swift_stdlib_gettid()

    let (exchanged, _) = storage.compareExchange(
      expected: 0,
      desired: selfId,
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    )

    if _fastPath(exchanged) {
      // Locked!
      return
    }

    _lockSlow(selfId)
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _lockSlow(_ selfId: UInt32) {
    // Before relinquishing control to the kernel to block this particular
    // thread, run a little spin lock to keep this thread busy in the scenario
    // where the current owner thread's critical section is somewhat quick. We
    // avoid a lot of the syscall overhead in these cases which allow both the
    // owner thread and this current thread to do the user-space atomic for
    // releasing and acquiring (assuming no existing waiters). The waiter bit is
    // typically unset when a call to 'FUTEX_UNLOCK_PI' has no other pi state,
    // meaning there is no one else waiting to acquire the lock.
    do {
      // This value is controlled on a per architecture bases defined in
      // 'SpinLoopHint.swift'.
      var tries = _tries

      repeat {
        // Do a relaxed load of the futex value to prevent introducing a memory
        // barrier on each iteration of this loop. We're already informing the
        // CPU that this is a spin loop via the '_spinLoopHint' call which
        // should hopefully slow down the loop a considerable amount to view an
        // actually change in the value potentially. An extra memory barrier
        // would make it even slower on top of the fact that we may not even be
        // able to attempt to acquire the lock.
        let state = storage.load(ordering: .relaxed)

        if state == 0, storage.compareExchange(
          expected: 0,
          desired: selfId,
          successOrdering: .acquiring,
          failureOrdering: .relaxed
        ).exchanged {
          // Locked!
          return
        }

        tries &-= 1

        // Inform the CPU that we're doing a spin loop which should have the
        // effect of slowing down this loop if only by a little to preserve
        // energy.
        _spinLoopHint()
      } while tries != 0
    }

    // We've exhausted our spins. Ask the kernel to block for us until the owner
    // releases the lock.
    //
    // Note: The kernel will attempt to acquire the lock for us as well which
    // could succeed if the owner releases in between finishing spinning the
    // futex syscall.
    while true {
      // Block until an equivalent '_futexUnlock' has been called by the owner.
      // This returns '0' on success which means the kernel has acquired the
      // lock for us.
      switch storage._futexLock() {
      case 0:
        // Locked!
        return

      // EINTR  - "A FUTEX_WAIT or FUTEX_WAIT_BITSET operation was interrupted
      //           by a signal (see signal(7)). Before Linux 2.6.22, this error
      //           could also be returned for a spurious wakeup; since Linux
      //           2.6.22, this no longer happens."
      // EAGAIN - "The futex owner thread ID of uaddr is about to exit, but has
      //           not yet handled the internal state cleanup. Try again."
      case 4, 11:
        continue

      // EDEADLK - "The futex word at uaddr is already locked by the caller."
      case 35:
        // TODO: Replace with a colder function / one that takes a StaticString
        fatalError("Recursive call to lock Mutex")

      // This handles all of the following errors which generally aren't
      // applicable to this implementation:
      //
      // EACCES - "No read access to the memory of a futex word."
      // EFAULT - "A required pointer argument did not point to a valid
      //           user-space address."
      // EINVAL - "The operation in futex_op is one of those that employs a
      //           timeout, but the supplied timeout argument was invalid
      //           (tv_sec was less than zero, or tv_nsec was not less than
      //           1,000,000,000)."
      //          OR
      //          "The operation specified in futex_op employs one or both of
      //           the pointers uaddr and uaddr2, but one of these does not
      //           point to a valid object—that is, the address is not four-
      //           byte-aligned."
      //          OR
      //          "The kernel detected an inconsistency between the user-space
      //           state at uaddr and the kernel state. This indicates either
      //           state corruption or that the kernel found a waiter on uaddr
      //           which is waiting via FUTEX_WAIT or FUTEX_WAIT_BITSET."
      //          OR
      //          "Invalid argument."
      // ENOMEM - "The kernel could not allocate memory to hold state
      //           information."
      // ENOSYS - "Invalid operation specified in futex_op."
      //          OR
      //          "A run-time check determined that the operation is not
      //           available. The PI-futex operations are not implemented on all
      //           architectures and are not supported on some CPU variants."
      // EPERM  - "The caller is not allowed to attach itself to the futex at
      //           uaddr (This may be caused by a state corruption in user
      //           space.)"
      // ESRCH  - "The thread ID in the futex word at uaddr does not exist."
      default:
        // TODO: Replace with a colder function / one that takes a StaticString
        fatalError("Unknown error occured while attempting to acquire a Mutex")
      }
    }
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal borrowing func _tryLock() -> Bool {
    // Do a user space cmpxchg to see if we can easily acquire the lock.
    if storage.compareExchange(
      expected: 0,

      // Note: This is being TLS cached.
      desired: _swift_stdlib_gettid(),
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    ).exchanged {
      // Locked!
      return true
    }

    // The quick atomic op failed, ask the kernel to see if it can acquire the
    // lock for us.
    return _tryLockSlow()
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _tryLockSlow() -> Bool {
    // Note: "Because the kernel has access to more state information than user
    //        space, acquisition of the lock might succeed if performed by the
    //        kernel in cases where the futex word (i.e., the state information
    //        accessible to use-space) contains stale state (FUTEX_WAITERS
    //        and/or FUTEX_OWNER_DIED). This can happen when the owner of the
    //        futex died. User space cannot handle this condition in a race-free
    //        manner, but the kernel can fix this up and acquire the futex."
    switch storage._futexTryLock() {
    case 0:
      // Locked!
      return true

    // EDEADLK - "The futex word at uaddr is already locked by the caller."
    case 35:
      // TODO: Replace with a colder function / one that takes a StaticString
      fatalError("Attempt to try to lock Mutex in already acquired thread")

    // This handles all of the following errors which generally aren't
    // applicable to this implementation:
    //
    // EACCES - "No read access to the memory of a futex word."
    // EAGAIN - "The futex owner thread ID of uaddr is about to exit, but has
    //           not yet handled the internal state cleanup. Try again."
    // EFAULT - "A required pointer argument did not point to a valid
    //           user-space address."
    // EINVAL - "The operation in futex_op is one of those that employs a
    //           timeout, but the supplied timeout argument was invalid
    //           (tv_sec was less than zero, or tv_nsec was not less than
    //           1,000,000,000)."
    //          OR
    //          "The operation specified in futex_op employs one or both of
    //           the pointers uaddr and uaddr2, but one of these does not
    //           point to a valid object—that is, the address is not four-
    //           byte-aligned."
    //          OR
    //          "The kernel detected an inconsistency between the user-space
    //           state at uaddr and the kernel state. This indicates either
    //           state corruption or that the kernel found a waiter on uaddr
    //           which is waiting via FUTEX_WAIT or FUTEX_WAIT_BITSET."
    //          OR
    //          "Invalid argument."
    // ENOMEM - "The kernel could not allocate memory to hold state
    //           information."
    // ENOSYS - "Invalid operation specified in futex_op."
    //          OR
    //          "A run-time check determined that the operation is not
    //           available. The PI-futex operations are not implemented on all
    //           architectures and are not supported on some CPU variants."
    // EPERM  - "The caller is not allowed to attach itself to the futex at
    //           uaddr (This may be caused by a state corruption in user
    //           space.)"
    // ESRCH  - "The thread ID in the futex word at uaddr does not exist."
    default:
      // Note: We could maybe retry this operation when given EAGAIN, but this
      //       is more or less supposed to be a quick yes/no.
      return false
    }
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal borrowing func _unlock() {
    // Note: This is being TLS cached.
    let selfId = _swift_stdlib_gettid()

    // Attempt to release the lock. We can only atomically release the lock in
    // user-space when there are no other waiters. If there are waiters, the
    // waiter bit is set and we need to inform the kernel that we're unlocking.
    let (exchanged, _) = storage.compareExchange(
      expected: selfId,
      desired: 0,
      successOrdering: .releasing,
      failureOrdering: .relaxed
    )

    if _fastPath(exchanged) {
      // No waiters, unlocked!
      return
    }

    _unlockSlow()
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _unlockSlow() {
    while true {
      switch storage._futexUnlock() {
      case 0:
        // Unlocked!
        return

      // EINTR  - "A FUTEX_WAIT or FUTEX_WAIT_BITSET operation was interrupted
      //           by a signal (see signal(7)). Before Linux 2.6.22, this error
      //           could also be returned for a spurious wakeup; since Linux
      //           2.6.22, this no longer happens."
      case 4:
        continue

      // EPERM  - "The caller does not own the lock represented by the futex
      //           word."
      case 1:
        // TODO: Replace with a colder function / one that takes a StaticString
        fatalError(
          "Call to unlock Mutex on a thread which hasn't acquired the lock"
        )

      // This handles all of the following errors which generally aren't
      // applicable to this implementation:
      //
      // EACCES - "No read access to the memory of a futex word."
      // EFAULT - "A required pointer argument did not point to a valid
      //           user-space address."
      // EINVAL - "The operation in futex_op is one of those that employs a
      //           timeout, but the supplied timeout argument was invalid
      //           (tv_sec was less than zero, or tv_nsec was not less than
      //           1,000,000,000)."
      //          OR
      //          "The operation specified in futex_op employs one or both of
      //           the pointers uaddr and uaddr2, but one of these does not
      //           point to a valid object—that is, the address is not four-
      //           byte-aligned."
      //          OR
      //          "The kernel detected an inconsistency between the user-space
      //           state at uaddr and the kernel state. This indicates either
      //           state corruption or that the kernel found a waiter on uaddr
      //           which is waiting via FUTEX_WAIT or FUTEX_WAIT_BITSET."
      //          OR
      //          "Invalid argument."
      // ENOSYS - "Invalid operation specified in futex_op."
      //          OR
      //          "A run-time check determined that the operation is not
      //           available. The PI-futex operations are not implemented on all
      //           architectures and are not supported on some CPU variants."
      // EPERM  - "The caller is not allowed to attach itself to the futex at
      //           uaddr (This may be caused by a state corruption in user
      //           space.)"
      default:
        // TODO: Replace with a colder function / one that takes a StaticString
        fatalError("Unknown error occured while attempting to release a Mutex")
      }
    }
  }
}

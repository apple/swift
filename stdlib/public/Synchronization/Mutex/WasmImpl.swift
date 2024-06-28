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

// Note: All atomic accesses on WASM are sequentially consistent regardless of
// what ordering we tell LLVM to use.

@_extern(c, "llvm.wasm.memory.atomic.wait32")
internal func _swift_stdlib_wait(
  on: UnsafePointer<UInt32>,
  expected: UInt32,
  timeout: Int64
) -> UInt32

@_extern(c, "llvm.wasm.memory.atomic.notify")
internal func _swift_stdlib_wake(on: UnsafePointer<UInt32>, count: UInt32) -> UInt32

extension Atomic where Value == _MutexHandle.State {
  internal borrowing func _wait(expected: _MutexHandle.State) {
    #if _runtime(_multithreaded)
    _ = _swift_stdlib_wait(
      on: .init(_rawAddress),
      expected: expected.rawValue,

      // A timeout of < 0 means indefinitely.
      timeout: -1
    )
    #endif
  }

  internal borrowing func _wake() {
    #if _runtime(_multithreaded)
    // Only wake up 1 thread
    _ = _swift_stdlib_wake(on: .init(_rawAddress), count: 1)
    #endif
  }
}

@available(SwiftStdlib 6.0, *)
extension _MutexHandle {
  @available(SwiftStdlib 6.0, *)
  @frozen
  @usableFromInline
  internal enum State: UInt32, AtomicRepresentable {
    case unlocked
    case locked
    case contended
  }
}

@available(SwiftStdlib 6.0, *)
@frozen
@_staticExclusiveOnly
public struct _MutexHandle: ~Copyable {
  @usableFromInline
  let storage: Atomic<State>

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init() {
    storage = Atomic(.unlocked)
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _lock() {
    // Note: We could probably merge this cas into a do/while style loop, but we
    // really want to perform the strong variant before attempting to do weak
    // ones in the loop.

    var (exchanged, state) = storage.compareExchange(
      expected: .unlocked,
      desired: .locked,
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    )

    if _fastPath(exchanged) {
      // Locked!
      return
    }

    while !exchanged {
      // If we're not already contended, go ahead and transition the mutex state
      // into being contended. If when we do this that the value stored there
      // was unlocked, then we know we unintentionally acquired the lock. A
      // weird quirk that occurs if this happens is that we go directly from
      // .unlocked -> .contended when in fact the lock may not be contended.
      // We may be able to do another atomic access and change it to .locked if
      // acquired it, but it may cause more problems than just potentially
      // calling wake with no waiters.
      if state != .contended, storage.exchange(
        .contended,
        ordering: .acquiring
      ) == .unlocked {
        // Locked!
        return
      }

      // Block until unlock has been called. This will return early if the call
      // to unlock happened between attempting to acquire and attempting to
      // wait while nobody else managed to acquire it yet.
      storage._wait(expected: .contended)

      (exchanged, state) = storage.weakCompareExchange(
        expected: .unlocked,
        desired: .locked,
        successOrdering: .acquiring,
        failureOrdering: .relaxed
      )
    }

    // Locked!
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _tryLock() -> Bool {
    storage.compareExchange(
      expected: .unlocked,
      desired: .locked,
      successOrdering: .acquiring,
      failureOrdering: .relaxed
    ).exchanged
  }

  @available(SwiftStdlib 6.0, *)
  @usableFromInline
  internal borrowing func _unlock() {
    // Transition our state from being either .locked or .contended to .unlocked.
    // At this point the mutex is freely acquirable. If the value that was
    // stored in the mutex was .locked, then no one else was waiting on this
    // mutex so we can just skip trying to wake up a thread.
    guard storage.exchange(.unlocked, ordering: .releasing) == .contended else {
      // Unlocked!
      return
    }

    // Otherwise, wake up our next lucky random thread to acquire the mutex.
    // (Assuming no new thread acquires the lock before it does)
    storage._wake()

    // Unlocked!
  }
}

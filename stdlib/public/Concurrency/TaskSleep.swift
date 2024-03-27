//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
import Swift
@_implementationOnly import _SwiftConcurrencyShims
@_implementationOnly import Synchronization

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
@available(SwiftStdlib 5.1, *)
extension Task where Success == Never, Failure == Never {
  @available(*, deprecated, renamed: "Task.sleep(nanoseconds:)")
  /// Suspends the current task for at least the given duration
  /// in nanoseconds.
  ///
  /// This function doesn't block the underlying thread.
  public static func sleep(_ duration: UInt64) async {
    return await Builtin.withUnsafeContinuation {
      (continuation: Builtin.RawUnsafeContinuation) -> Void in
      let job = _taskCreateNullaryContinuationJob(
          priority: Int(Task.currentPriority.rawValue),
          continuation: continuation)
      _enqueueJobGlobalWithDelay(duration, job)
    }
  }

  /// The type of continuation used in the implementation of
  /// sleep(nanoseconds:).
  typealias SleepContinuation = UnsafeContinuation<(), Error>

  /// Describes the state of a sleep() operation.
  enum SleepState: RawRepresentable, AtomicRepresentable {
    /// The sleep continuation has not yet begun.
    case notStarted

    // The sleep continuation has been created and is available here.
    case activeContinuation(SleepContinuation)

    /// The sleep has finished.
    case finished

    /// The sleep was canceled.
    case cancelled

    /// The sleep was canceled before it even got started.
    case cancelledBeforeStarted

    /// Decode sleep state from the word of storage.
    init?(rawValue: UInt) {
      switch rawValue & 0x03 {
      case 0:
        let continuationBits = rawValue & ~0x03
        if continuationBits == 0 {
          self = .notStarted
        } else {
          let continuation = unsafeBitCast(
            continuationBits, to: SleepContinuation.self)
          self = .activeContinuation(continuation)
        }

      case 1:
        self = .finished

      case 2:
        self = .cancelled

      case 3:
        self = .cancelledBeforeStarted

      default:
        return nil
      }
    }

    /// Encode sleep state into a word of storage.
    var rawValue: UInt {
      switch self {
      case .notStarted:
        return 0

      case .activeContinuation(let continuation):
        let continuationBits = unsafeBitCast(continuation, to: UInt.self)
        return continuationBits

      case .finished:
        return 1

      case .cancelled:
        return 2

      case .cancelledBeforeStarted:
        return 3
      }
    }
  }

  /// Called when the sleep(nanoseconds:) operation woke up without being
  /// canceled.
  static func onSleepWake(_ token: borrowing Atomic<SleepState>) {
    while true {
      let state = token.load(ordering: .relaxed)
      switch state {
      case .notStarted:
        fatalError("Cannot wake before we even started")

      case .activeContinuation(let continuation):
        // We have an active continuation, so try to transition to the
        // "finished" state.
        let (won, _) = token.compareExchange(expected: state, desired: .finished, ordering: .acquiring)
        if won {
          // The sleep finished, so invoke the continuation: we're done.
          continuation.resume()
          return
        }

        // Try again!
        continue

      case .finished:
        fatalError("Already finished normally, can't do that again")

      case .cancelled:
        // The task was cancelled, which means the continuation was
        // called by the cancellation handler.
        return

      case .cancelledBeforeStarted:
        // Nothing to do;
        return
      }
    }
  }


  /// Called when the sleep(nanoseconds:) operation has been canceled before
  /// the sleep completed.
  static func onSleepCancel(_ token: borrowing Atomic<SleepState>) {
    while true {
      let state = token.load(ordering: .relaxed)
      switch state {
      case .notStarted:
        // We haven't started yet, so try to transition to the cancelled-before
        // started state.
        let (won, _) = token.compareExchange(
          expected: state, desired: .cancelledBeforeStarted, ordering: .acquiring)
        if won {
          return
        }

        // Try again!
        continue

      case .activeContinuation(let continuation):
        // We have an active continuation, so try to transition to the
        // "cancelled" state.
        let (won, _) = token.compareExchange(
          expected: state, desired: .cancelled, ordering: .acquiring)
        if won {
          // We recorded the task cancellation before the sleep finished, so
          // invoke the continuation with the cancellation error.
          continuation.resume(throwing: _Concurrency.CancellationError())
          return
        }

        // Try again!
        continue

      case .finished, .cancelled, .cancelledBeforeStarted:
        // The operation already finished, so there is nothing more to do.
        return
      }
    }
  }

  /// Suspends the current task for at least the given duration
  /// in nanoseconds.
  ///
  /// If the task is canceled before the time ends,
  /// this function throws `CancellationError`.
  ///
  /// This function doesn't block the underlying thread.
  public static func sleep(nanoseconds duration: UInt64) async throws {
    // Create a token which will initially have the value "not started", which
    // means the continuation has neither been created nor completed.
    let token = Atomic<SleepState>(.notStarted)

    do {
      // Install a cancellation handler to resume the continuation by
      // throwing CancellationError.
      try await withTaskCancellationHandler {
        let _: () = try await withUnsafeThrowingContinuation { continuation in
          while true {
            let state = token.load(ordering: .relaxed)
            switch state {
            case .notStarted:
              // Try to swap in the continuation state.
              let newState = SleepState.activeContinuation(continuation)
              let (won, _) = token.compareExchange(
                expected: state, desired: newState, ordering: .acquiring)
              if !won {
                // Keep trying!
                continue
              }

              // Create a task that resumes the continuation normally if it
              // finishes first. Enqueue it directly with the delay, so it fires
              // when we're done sleeping.
              let sleepTaskFlags = taskCreateFlags(
                priority: nil, isChildTask: false, copyTaskLocals: false,
                inheritContext: false, enqueueJob: false,
                addPendingGroupTaskUnconditionally: false,
                isDiscardingTask: false)
              let (sleepTask, _) = Builtin.createAsyncTask(sleepTaskFlags) {
                onSleepWake(token)
              }
              _enqueueJobGlobalWithDelay(
                  duration, Builtin.convertTaskToJob(sleepTask))
              return

            case .activeContinuation, .finished:
              fatalError("Impossible to have multiple active continuations")

            case .cancelled:
              fatalError("Impossible to have cancelled before we began")

            case .cancelledBeforeStarted:
              // Finish the continuation normally. We'll throw later, after
              // we clean up.
              continuation.resume()
              return
          }
        }
        }
      } onCancel: {
        onSleepCancel(token)
      }

      // Determine whether we got cancelled before we even started.
      let cancelledBeforeStarted: Bool
      switch token.load(ordering: .relaxed) {
      case .notStarted, .activeContinuation, .cancelled:
        fatalError("Invalid state for non-cancelled sleep task")

      case .cancelledBeforeStarted:
        cancelledBeforeStarted = true

      case .finished:
        cancelledBeforeStarted = false
      }

      // If we got cancelled before we even started, through the cancellation
      // error now.
      if cancelledBeforeStarted {
        throw _Concurrency.CancellationError()
      }
    } catch {
      // The task was cancelled; propagate the error. The "on wake" task is
      // responsible for deallocating the flag word and continuation, if it's
      // still running.
      throw error
    }
  }
}
#else
@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
extension Task where Success == Never, Failure == Never {
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public static func sleep(_ duration: UInt64) async {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public static func sleep(nanoseconds duration: UInt64) async throws {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
}
#endif

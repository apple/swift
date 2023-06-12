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

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
@available(SwiftStdlib 5.7, *)
extension Task where Success == Never, Failure == Never {
  @available(SwiftStdlib 5.7, *)
  internal static func _sleep(
    until seconds: Int64, _ nanoseconds: Int64,
    tolerance: Duration?,
    clock: _ClockID
  ) async throws {
    // Allocate storage for the storage word.
    let wordPtr = UnsafeMutablePointer<Builtin.Word>.allocate(capacity: 1)

    // Initialize the flag word to "not started", which means the continuation
    // has neither been created nor completed.
    Builtin.atomicstore_seqcst_Word(
        wordPtr._rawValue, SleepState.notStarted.word._builtinWordValue)

    do {
      // Install a cancellation handler to resume the continuation by
      // throwing CancellationError.
      try await withTaskCancellationHandler {
        let _: () = try await withUnsafeThrowingContinuation { continuation in
          while true {
            let state = SleepState(loading: wordPtr)
            switch state {
            case .notStarted:
              // The word that describes the active continuation state.
              let continuationWord =
                SleepState.activeContinuation(continuation).word

              // Try to swap in the continuation word.
              let (_, won) = Builtin.cmpxchg_seqcst_seqcst_Word(
                  wordPtr._rawValue,
                  state.word._builtinWordValue,
                  continuationWord._builtinWordValue)
              if !Bool(_builtinBooleanLiteral: won) {
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
                onSleepWake(wordPtr)
              }
              let toleranceSeconds: Int64
              let toleranceNanoseconds: Int64
              if let components = tolerance?.components {
                toleranceSeconds = components.seconds
                toleranceNanoseconds = components.attoseconds / 1_000_000_000
              } else {
                toleranceSeconds = 0
                toleranceNanoseconds = -1
              }

              _enqueueJobGlobalWithDeadline(
                  seconds, nanoseconds,
                  toleranceSeconds, toleranceNanoseconds,
                  clock.rawValue, Builtin.convertTaskToJob(sleepTask))
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
        onSleepCancel(wordPtr)
      }

      // Determine whether we got cancelled before we even started.
      let cancelledBeforeStarted: Bool
      switch SleepState(loading: wordPtr) {
      case .notStarted, .activeContinuation, .cancelled:
        fatalError("Invalid state for non-cancelled sleep task")

      case .cancelledBeforeStarted:
        cancelledBeforeStarted = true

      case .finished:
        cancelledBeforeStarted = false
      }

      // We got here without being cancelled, so deallocate the storage for
      // the flag word and continuation.
      wordPtr.deallocate()

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

  /// Suspends the current task until the given deadline within a tolerance.
  ///
  /// If the task is canceled before the time ends, this function throws 
  /// `CancellationError`.
  ///
  /// This function doesn't block the underlying thread.
  ///
  ///       try await Task.sleep(until: .now + .seconds(3))
  ///
  @available(SwiftStdlib 5.7, *)
  public static func sleep<C: Clock>(
    until deadline: C.Instant,
    tolerance: C.Instant.Duration? = nil,
    clock: C = ContinuousClock()
  ) async throws {
    try await clock.sleep(until: deadline, tolerance: tolerance)
  }
  
  /// Suspends the current task for the given duration.
  ///
  /// If the task is cancelled before the time ends, this function throws 
  /// `CancellationError`.
  ///
  /// This function doesn't block the underlying thread.
  ///
  ///       try await Task.sleep(for: .seconds(3))
  ///
  @available(SwiftStdlib 5.7, *)
  @_alwaysEmitIntoClient
  public static func sleep<C: Clock>(
    for duration: C.Instant.Duration,
    tolerance: C.Instant.Duration? = nil,
    clock: C = ContinuousClock()
  ) async throws {
    try await clock.sleep(for: duration, tolerance: tolerance)
  }
}
#else
@available(SwiftStdlib 5.7, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
extension Task where Success == Never, Failure == Never {
  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public static func sleep<C: Clock>(
    until deadline: C.Instant,
    tolerance: C.Instant.Duration? = nil,
    clock: C = ContinuousClock()
  ) async throws {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  @_alwaysEmitIntoClient
  public static func sleep<C: Clock>(
    for duration: C.Instant.Duration,
    tolerance: C.Instant.Duration? = nil,
    clock: C = ContinuousClock()
  ) async throws {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
}
#endif

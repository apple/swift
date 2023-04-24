//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

#if SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
@globalActor public final actor MainActor: GlobalActor {
  public static let shared = MainActor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
    #if compiler(>=5.5) && $BuiltinBuildMainExecutor
    return UnownedSerialExecutor(Builtin.buildMainActorExecutorRef())
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  @inlinable
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
    #if compiler(>=5.5) && $BuiltinBuildMainExecutor
    return UnownedSerialExecutor(Builtin.buildMainActorExecutorRef())
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
    _enqueueOnMain(job)
  }
}
#else
/// A singleton actor whose executor is equivalent to the main
/// dispatch queue.
@available(SwiftStdlib 5.1, *)
@globalActor public final actor MainActor: GlobalActor {
  public static let shared = MainActor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
    #if compiler(>=5.5) && $BuiltinBuildMainExecutor
    return UnownedSerialExecutor(Builtin.buildMainActorExecutorRef())
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  @inlinable
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
    #if compiler(>=5.5) && $BuiltinBuildMainExecutor
    return UnownedSerialExecutor(Builtin.buildMainActorExecutorRef())
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
    _enqueueOnMain(job)
  }
}
#endif

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
@available(SwiftStdlib 5.1, *)
extension MainActor {
  /// Execute the given body closure on the main actor.
  ///
  /// Historical ABI entry point, superseded by the Sendable version that is
  /// also inlined to back-deploy a semantic fix where this operation would
  /// not hop back at the end.
  @usableFromInline
  static func run<T>(
    resultType: T.Type = T.self,
    body: @MainActor @Sendable () throws -> T
  ) async rethrows -> T {
    return try await body()
  }

  /// Execute the given body closure on the main actor.
  @_alwaysEmitIntoClient
  public static func run<T: Sendable>(
    resultType: T.Type = T.self,
    body: @MainActor @Sendable () throws -> T
  ) async rethrows -> T {
    return try await body()
  }
}

@available(SwiftStdlib 5.1, *)
extension MainActor {
  /// A safe way to synchronously assume that the current execution context belongs to the MainActor.
  ///
  /// This API should only be used as last resort, when it is not possible to express the current
  /// execution context definitely belongs to the main actor in other ways. E.g. one may need to use
  /// this in a delegate style API, where a synchronous method is guaranteed to be called by the
  /// main actor, however it is not possible to annotate this legacy API with `@MainActor`.
  ///
  /// - Warning: If the current executor is *not* the MainActor's serial executor, this function will crash.
  ///
  /// Note that this check is performed against the MainActor's serial executor, meaning that
  /// if another actor uses the same serial executor--by using ``MainActor/sharedUnownedExecutor``
  /// as its own ``Actor/unownedExecutor``--this check will succeed, as from a concurrency safety
  /// perspective, the serial executor guarantees mutual exclusion of those two actors.
  @available(SwiftStdlib 5.1, *)
  @backDeployed(before: SwiftStdlib 5.9)
  @_unavailableFromAsync(message: "await the call to the @MainActor closure directly")
  public static func assumeIsolated<T>(
      _ operation: @MainActor () throws -> T,
      file: StaticString = #fileID, line: UInt = #line
  ) rethrows -> T {

    typealias YesActor = @MainActor () throws -> T
    typealias NoActor = () throws -> T

    /// This is guaranteed to be fatal if the check fails,
    /// as this is our "safe" version of this API.
    let executor: Builtin.Executor = Self.shared.unownedExecutor.executor
    guard _taskIsCurrentExecutor(executor) else {
      // TODO: offer information which executor we actually got
      fatalError("Incorrect actor executor assumption; Expected same executor as \(self).", file: file, line: line)
    }

    // To do the unsafe cast, we have to pretend it's @escaping.
    return try withoutActuallyEscaping(operation) {
      (_ fn: @escaping YesActor) throws -> T in
      let rawFn = unsafeBitCast(fn, to: NoActor.self)
      return try rawFn()
    }
  }
}
#endif

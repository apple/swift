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

import Swift
import SwiftShims

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

// ==== -----------------------------------------------------------------------
// MARK: Precondition executors

@available(SwiftStdlib 5.1, *)
extension SerialExecutor {
  /// Stops program execution if the current task is not executing on this
  /// serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration), stops
  ///   program execution.
  ///
  /// - Note: Because this check is performed against the actor's serial executor,
  ///   if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed. From a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default value is
  ///           the file where this method was called.
  ///   - line: The line number to print if the assertion fails The default value is
  ///           the line where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public func preconditionIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() || _isReleaseAssertConfiguration() else {
      return
    }

    let expectationCheck = _taskIsCurrentExecutor(self.asUnownedSerialExecutor().executor)

    /// TODO: implement the logic in-place perhaps rather than delegating to precondition()?
    precondition(expectationCheck,
        // TODO: offer information which executor we actually got
        "Incorrect actor executor assumption; Expected '\(self)' executor. \(message())",
        file: file, line: line) // short-cut so we get the exact same failure reporting semantics
  }
}

@available(SwiftStdlib 5.1, *)
extension Actor {
  /// Stops program execution if the current task is not executing on this
  /// actor's serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration), stops
  ///   program execution.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed , as from a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public nonisolated func preconditionIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() || _isReleaseAssertConfiguration() else {
      return
    }

    // NOTE: This method will CRASH in new runtime versions,
    // if it would have previously returned `false`.
    // It will call through to SerialExecutor.checkIsolated` as a last resort.
    let expectationCheck = _taskIsCurrentExecutor(self.unownedExecutor.executor)

    precondition(expectationCheck,
        "Incorrect actor executor assumption; Expected '\(self.unownedExecutor)' executor. \(message())",
        file: file, line: line)
  }
}

@available(SwiftStdlib 5.1, *)
extension GlobalActor {
  /// Stops program execution if the current task is not executing on this
  /// actor's serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration), stops
  ///   program execution.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed , as from a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public static func preconditionIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    Self.shared.preconditionIsolated(message(), file: file, line: line)
  }
}

// ==== -----------------------------------------------------------------------
// MARK: Assert executors

@available(SwiftStdlib 5.1, *)
extension SerialExecutor {
  /// Stops program execution if the current task is not executing on this
  /// serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration),
  ///   the isolation check is not performed and there are no effects.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed , as from a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public func assertIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() else {
      return
    }

    guard _taskIsCurrentExecutor(self.asUnownedSerialExecutor().executor) else {
      // TODO: offer information which executor we actually got
      let msg = "Incorrect actor executor assumption; Expected '\(self)' executor. \(message())"
      /// TODO: implement the logic in-place perhaps rather than delegating to precondition()?
      assertionFailure(msg, file: file, line: line)
      return
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension Actor {
  /// Stops program execution if the current task is not executing on this
  /// actor's serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration),
  ///   the isolation check is not performed and there are no effects.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed , as from a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public nonisolated func assertIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() else {
      return
    }

    guard _taskIsCurrentExecutor(self.unownedExecutor.executor) else {
      let msg = "Incorrect actor executor assumption; Expected '\(self.unownedExecutor)' executor. \(message())"
      /// TODO: implement the logic in-place perhaps rather than delegating to precondition()?
      assertionFailure(msg, file: file, line: line) // short-cut so we get the exact same failure reporting semantics
      return
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension GlobalActor {
  /// Stops program execution if the current task is not executing on this
  /// actor's serial executor.
  ///
  /// This function's effect varies depending on the build flag used:
  ///
  /// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
  ///   configuration), stops program execution in a debuggable state after
  ///   printing `message`.
  ///
  /// * In `-O` builds (the default for Xcode's Release configuration),
  ///   the isolation check is not performed and there are no effects.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   that actor's serial executor as its own ``Actor/unownedExecutor``--this
  ///   check will succeed , as from a concurrency safety perspective, the
  ///   serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - message: The message to print if the assertion fails.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableInEmbedded
  public static func assertIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    Self.shared.assertIsolated(message(), file: file, line: line)
  }
}

// ==== -----------------------------------------------------------------------
// MARK: Assume Executor

@available(SwiftStdlib 5.1, *)
extension Actor {
  /// Assume that the current task is executing on this actor's serial executor,
  /// or stop program execution otherwise.
  ///
  /// You call this method to *assume and verify* that the currently
  /// executing synchronous function is actually executing on the serial
  /// executor of this actor.
  ///
  /// If that is the case, the operation is invoked with an `isolated` version
  /// of the actor, allowing synchronous access to actor local state without
  /// hopping through asynchronous boundaries.
  ///
  /// If the current context is not running on the actor's serial executor, or
  /// if the actor is a reference to a remote actor, this method will crash
  /// with a fatal error (similar to ``preconditionIsolated()``).
  ///
  /// Note that this check is performed against the passed in actor's serial
  /// executor, meaning that if another actor uses the same serial executor--by
  /// using that actor's ``Actor/unownedExecutor`` as its own
  /// ``Actor/unownedExecutor``--this check will succeed, as from a concurrency
  /// safety perspective, the serial executor guarantees mutual exclusion of
  /// those two actors.
  ///
  /// This method can only be used from synchronous functions, as asynchronous
  /// functions should instead perform a normal method call to the actor, which
  /// will hop task execution to the target actor if necessary.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   another actor's executor as its own ``Actor/unownedExecutor``
  ///   --this check will succeed , as from a concurrency safety perspective,
  ///   the serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - operation: the operation that will be executed if the current context
  ///                is executing on the actors serial executor.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  /// - Returns: the return value of the `operation`
  /// - Throws: rethrows the `Error` thrown by the operation if it threw
  @available(SwiftStdlib 5.1, *)
  #if !$Embedded
  @backDeployed(before: SwiftStdlib 5.9)
  #endif
  @_unavailableFromAsync(message: "express the closure as an explicit function declared on the specified 'actor' instead")
  @_unavailableInEmbedded
  public nonisolated func assumeIsolated<T>(
      _ operation: (isolated Self) throws -> T,
      file: StaticString = #fileID, line: UInt = #line
  ) rethrows -> T {
    typealias YesActor = (isolated Self) throws -> T
    typealias NoActor = (Self) throws -> T

    /// This is guaranteed to be fatal if the check fails,
    /// as this is our "safe" version of this API.
    let executor: Builtin.Executor = self.unownedExecutor.executor
    guard _taskIsCurrentExecutor(executor) else {
      // TODO: offer information which executor we actually got
      fatalError("Incorrect actor executor assumption; Expected same executor as \(self).", file: file, line: line)
    }

    #if $TypedThrows
    // To do the unsafe cast, we have to pretend it's @escaping.
    return try withoutActuallyEscaping(operation) {
      (_ fn: @escaping YesActor) throws -> T in
      let rawFn = unsafeBitCast(fn, to: NoActor.self)
      return try rawFn(self)
    }
    #else
    fatalError("unsupported compiler")
    #endif
  }
}

#endif // not SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

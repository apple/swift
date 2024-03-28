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
import _Concurrency

// ==== -----------------------------------------------------------------------
// MARK: Precondition APIs

@available(SwiftStdlib 5.9, *)
extension DistributedActor {
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
  @available(SwiftStdlib 5.9, *)
  public nonisolated func preconditionIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() || _isReleaseAssertConfiguration() else {
      return
    }

    let unownedExecutor = self.unownedExecutor
    let expectationCheck = _taskIsCurrentExecutor(unownedExecutor._executor)

    precondition(expectationCheck,
        "Incorrect actor executor assumption; Expected '\(self.unownedExecutor)' executor. \(message())",
        file: file, line: line)
  }
}

// ==== -----------------------------------------------------------------------
// MARK: Assert APIs

@available(SwiftStdlib 5.9, *)
extension DistributedActor {
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
  @available(SwiftStdlib 5.9, *)
  @_transparent
  public nonisolated func assertIsolated(
      _ message: @autoclosure () -> String = String(),
      file: StaticString = #fileID, line: UInt = #line
  ) {
    guard _isDebugAssertConfiguration() else {
      return
    }

    let unownedExecutor = self.unownedExecutor
    guard _taskIsCurrentExecutor(unownedExecutor._executor) else {
      let msg = "Incorrect actor executor assumption; Expected '\(unownedExecutor)' executor. \(message())"
      /// TODO: implement the logic in-place perhaps rather than delegating to precondition()?
      assertionFailure(msg, file: file, line: line) // short-cut so we get the exact same failure reporting semantics
      return
    }
  }
}


// ==== -----------------------------------------------------------------------
// MARK: Assume APIs

@available(SwiftStdlib 5.9, *)
extension DistributedActor {

  /// Assume that the current task is executing on this (local) distributed
  /// actor's serial executor, or stop program execution otherwise.
  ///
  /// This method allows to *assume and verify* that the currently
  /// executing synchronous function is actually executing on the serial
  /// executor of the this (local) distributed actor.
  ///
  /// If that is the case, the operation is invoked isolated to the main actor
  /// (`@MainActor () -> T`), allowing synchronous access to actor local state
  /// without hopping through asynchronous boundaries.
  ///
  /// If the current context is not running on the actor's serial executor,
  /// this method will crash with a fatal error (similar
  /// to ``preconditionIsolated()``).
  ///
  /// This method can only be used from synchronous functions, as asynchronous
  /// functions should instead perform a normal method call to the actor, which
  /// will hop task execution to the target actor if necessary.
  ///
  /// - Note: This check is performed against the actor's serial executor,
  ///   meaning that / if another actor uses the same serial executor--by using
  ///   another actor's executor as its own ``DistributedActor/unownedExecutor``
  ///   --this check will succeed , as from a concurrency safety perspective,
  ///   the serial executor guarantees mutual exclusion of those two actors.
  ///
  /// - Parameters:
  ///   - operation: the operation that will be executed if the current context
  ///                is executing on the actors serial executor, and the actor
  ///                is a local reference.
  ///   - file: The file name to print if the assertion fails. The default is
  ///           where this method was called.
  ///   - line: The line number to print if the assertion fails The default is
  ///           where this method was called.
  /// - Returns: the return value of the `operation`
  /// - Throws: rethrows the `Error` thrown by the operation if it threw
  @available(SwiftStdlib 5.9, *)
  @_unavailableFromAsync(message: "express the closure as an explicit function declared on the specified 'distributed actor' instead")
  public nonisolated func assumeIsolated<T>(
      _ operation: (isolated Self) throws -> T,
      file: StaticString = #fileID, line: UInt = #line
  ) rethrows -> T {
    typealias YesActor = (isolated Self) throws -> T
    typealias NoActor = (Self) throws -> T

    guard __isLocalActor(self) else {
      fatalError("Cannot assume to be 'isolated \(Self.self)' since distributed actor '\(self)' is a remote actor reference.")
    }

    let unownedExecutor = self.unownedExecutor
    guard _taskIsCurrentExecutor(unownedExecutor._executor) else {
      // TODO: offer information which executor we actually got when
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

/// WARNING: This function will CRASH rather than return `false` in new runtimes
///
/// It eventually calls into `SerialExecutor.checkIsolated` which allows even
/// for non Task code to assume isolation in certain situations, however this
/// API cannot be made "return false", and instead will always crash if it
/// were to return false.
@available(SwiftStdlib 5.1, *)
@usableFromInline
@_silgen_name("swift_task_isCurrentExecutor")
func _taskIsCurrentExecutor(_ executor: Builtin.Executor) -> Bool

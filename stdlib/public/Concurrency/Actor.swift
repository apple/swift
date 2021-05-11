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

/// Common protocol to which all actors conform.
///
/// The \c Actor protocol generalizes over all actor types. Actor types
/// implicitly conform to this protocol.
@available(SwiftStdlib 5.5, *)
public protocol Actor: AnyObject, Sendable {

  /// Retrieve the executor for this actor as an optimized, unowned
  /// reference.
  ///
  /// This property must always evaluate to the same executor for a
  /// given actor instance, and holding on to the actor must keep the
  /// executor alive.
  ///
  /// This property will be implicitly accessed when work needs to be
  /// scheduled onto this actor.  These accesses may be merged,
  /// eliminated, and rearranged with other work, and they may even
  /// be introduced when not strictly required.  Visible side effects
  /// are therefore strongly discouraged within this property.
  nonisolated var unownedExecutor: UnownedSerialExecutor { get }
}

/// Called to initialize the default actor instance in an actor.
/// The implementation will call this within the actor's initializer.
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_defaultActor_initialize")
public func _defaultActorInitialize(_ actor: AnyObject)

/// Called to destroy the default actor instance in an actor.
/// The implementation will call this within the actor's deinit.
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_defaultActor_destroy")
public func _defaultActorDestroy(_ actor: AnyObject)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_enqueueMainExecutor")
@usableFromInline
internal func _enqueueOnMain(_ job: UnownedJob)

/// A singleton actor whose executor is equivalent to 
/// \c DispatchQueue.main, which is the main dispatch queue.
@available(SwiftStdlib 5.5, *)
@globalActor public final actor MainActor: SerialExecutor {
  public static let shared = MainActor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
    return asUnownedSerialExecutor()
  }

  @inlinable
  public nonisolated func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    return UnownedSerialExecutor(ordinary: self)
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
    _enqueueOnMain(job)
  }
}

// Used by the concurrency runtime
@available(SwiftStdlib 5.5, *)
@_silgen_name("_swift_task_getMainExecutor")
internal func _getMainExecutor() -> UnownedSerialExecutor {
  return MainActor.shared.unownedExecutor
}

@available(SwiftStdlib 5.5, *)
extension MainActor {
  /// Execute the given body closure on the main actor.
  public static func run<T>(
    resultType: T.Type = T.self,
    body: @MainActor @Sendable () throws -> T
  ) async rethrows -> T {
    @MainActor func runOnMain(body: @MainActor @Sendable () throws -> T) async rethrows -> T {
      return try body()
    }

    return try await runOnMain(body: body)
  }
}

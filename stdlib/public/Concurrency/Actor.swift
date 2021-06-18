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
/// The `Actor` protocol generalizes over all actor types. Actor types
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

// Used by the concurrency runtime
@available(SwiftStdlib 5.5, *)
@_silgen_name("_swift_task_getMainExecutor")
internal func _getMainExecutor() -> UnownedSerialExecutor {
  return MainActor.shared.unownedExecutor
}

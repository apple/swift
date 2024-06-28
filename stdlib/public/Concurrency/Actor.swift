//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
@_implementationOnly import _SwiftConcurrencyShims

/// Common marker protocol providing a shared "base" for both (local) `Actor`
/// and (potentially remote) `DistributedActor` types.
///
/// The `AnyActor` marker protocol generalizes over all actor types, including
/// distributed ones. In practice, this protocol can be used to restrict
/// protocols, or generic parameters to only be usable with actors, which
/// provides the guarantee that calls may be safely made on instances of given
/// type without worrying about the thread-safety of it -- as they are
/// guaranteed to follow the actor-style isolation semantics.
///
/// While both local and distributed actors are conceptually "actors", there are
/// some important isolation model differences between the two, which make it
/// impossible for one to refine the other.
@available(SwiftStdlib 5.1, *)
@available(*, deprecated, message: "Use 'any Actor' with 'DistributedActor.asLocalActor' instead")
@available(swift, obsoleted: 6.0, message: "Use 'any Actor' with 'DistributedActor.asLocalActor' instead")
public typealias AnyActor = AnyObject & Sendable

/// Common protocol to which all actors conform.
///
/// The `Actor` protocol generalizes over all `actor` types. Actor types
/// implicitly conform to this protocol.
///
/// ### Actors and SerialExecutors
/// By default, actors execute tasks on a shared global concurrency thread pool.
/// This pool is shared by all default actors and tasks, unless an actor or task
/// specified a more specific executor requirement.
///
/// It is possible to configure an actor to use a specific ``SerialExecutor``,
/// as well as impact the scheduling of default tasks and actors by using
/// a ``TaskExecutor``.
///
/// - SeeAlso: ``SerialExecutor``
/// - SeeAlso: ``TaskExecutor``
@available(SwiftStdlib 5.1, *)
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
  ///
  /// - SeeAlso: ``SerialExecutor``
  /// - SeeAlso: ``TaskExecutor``
  nonisolated var unownedExecutor: UnownedSerialExecutor { get }
}

/// Called to initialize the default actor instance in an actor.
/// The implementation will call this within the actor's initializer.
@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_defaultActor_initialize")
public func _defaultActorInitialize(_ actor: AnyObject)

@available(SwiftStdlib 5.9, *)
@_silgen_name("swift_nonDefaultDistributedActor_initialize")
public func _nonDefaultDistributedActorInitialize(_ actor: AnyObject)

/// Called to destroy the default actor instance in an actor.
/// The implementation will call this within the actor's deinit.
@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_defaultActor_destroy")
public func _defaultActorDestroy(_ actor: AnyObject)

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_task_enqueueMainExecutor")
@usableFromInline
internal func _enqueueOnMain(_ job: UnownedJob)

#if $Macros
/// Produce a reference to the actor to which the enclosing code is
/// isolated, or `nil` if the code is nonisolated.
///
/// If the type annotation provided for `#isolation` is not `(any Actor)?`,
/// the type must match the enclosing actor type. If no type annotation is
/// provided, the type defaults to `(any Actor)?`.
@available(SwiftStdlib 5.1, *)
@freestanding(expression)
public macro isolation<T>() -> T = Builtin.IsolationMacro
#endif

#if $IsolatedAny
@_alwaysEmitIntoClient
@available(SwiftStdlib 5.1, *)
public func extractIsolation<each Arg, Result>(
  _ fn: @escaping @isolated(any) (repeat each Arg) async throws -> Result
) -> (any Actor)? {
  return Builtin.extractFunctionIsolation(fn)
}
#endif

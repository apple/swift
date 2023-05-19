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
import _Concurrency

@available(SwiftStdlib 5.9, *)
@usableFromInline
internal final class DistributedRemoteActorReferenceExecutor: SerialExecutor {
  static let _shared: DistributedRemoteActorReferenceExecutor = DistributedRemoteActorReferenceExecutor()
  static var sharedUnownedExecutor: UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: _shared)
  }

  internal init() {}

  @inlinable
  public func enqueue(_ job: consuming ExecutorJob) {
    let jobDescription = job.description
    fatalError("Attempted to enqueue ExecutorJob (\(jobDescription)) on executor of remote distributed actor reference!")
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

/// Obtain the unowned `SerialExecutor` that is used by by remote distributed actor references.
/// The executor is shared between all remote default executor remote distributed actors,
/// and it will crash if any job is enqueued on it.
///
/// It is possible to obtain the executor e.g. for logging or general debugging,
/// however attempting to enqueue work on what might potentially be a remote actor
/// is a programming error and therefore will crash if the actor is potentially.
///
/// If one intends to use a distributed actor's executor to schedule work on it,
/// one should programmatically ensure that that actor is local, e.g. using the `whenLocal`
/// functionality of distributed actors, or by other means (e.g. "knowing that it definitely must be local")
@available(SwiftStdlib 5.9, *)
public func buildDefaultDistributedRemoteActorExecutor<Act>(
    _ actor: Act
) -> UnownedSerialExecutor where Act: DistributedActor {
  return DistributedRemoteActorReferenceExecutor.sharedUnownedExecutor
}

//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
import _Concurrency

@available(SwiftStdlib 5.5, *)
public protocol ActorTransport: Sendable {

  // ==== ---------------------------------------------------------------------
  // - MARK: Initialization

  /// Resolve a local or remote actor address to a real actor instance, or throw if unable to.
  /// The returned value is either a local actor or proxy to a remote actor.
  func resolve<Act>(identity: ActorIdentity, as actorType: Act.Type) throws -> ActorResolved<Act>
      where Act: DistributedActor

  // ==== ---------------------------------------------------------------------
  // - MARK: Actor Lifecycle

  /// Create an `ActorIdentity` for the passed actor type.
  ///
  /// This function is invoked by an distributed actor during its initialization,
  /// and the returned address value is stored along with it for the time of its
  /// lifetime.
  ///
  /// The address MUST uniquely identify the actor, and allow resolving it.
  /// E.g. if an actor is created under address `addr1` then immediately invoking
  /// `transport.resolve(address: addr1, as: Greeter.self)` MUST return a reference
  /// to the same actor.
  func assignAddress<Act>(_ actorType: Act.Type) -> ActorIdentity
      where Act: DistributedActor

  func actorReady<Act>(_ actor: Act) where Act: DistributedActor

  /// Called during actor deinit/destroy.
  func resignAddress(_ identity: ActorIdentity)

  // ==== ---------------------------------------------------------------------
  // - MARK: Actor Identity

  // TODO: `to encoder` or `to container`, not sure yet...
  // TODO: the alternative is to return some 'bytes' but not sure we'd want to get forced into [UInt8] here... caching those might be tricky... and we'd definitely not want to allocate for every encode
  func encodeIdentity<Act>(_ actor: Act, to encoder: Encoder) throws
      where Act: DistributedActor

  func decodeIdentity<Act>(type: Act.Type, from decoder: Decoder) throws -> ActorIdentity
      where Act: DistributedActor

  func hashIdentity(_ identity: ActorIdentity, into hasher: inout Hasher)

  // To be able to implement `==` on an actor while we cnanot make
  func compareIdentities(_ lhs: ActorIdentity, _ rhs:  ActorIdentity) -> Bool


}

@available(SwiftStdlib 5.5, *)
public enum ActorResolved<Act: DistributedActor> {
  case resolved(Act)
  case makeProxy
}

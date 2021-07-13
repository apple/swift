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

// ==== Any Actor -------------------------------------------------------------

/// Shared "base" protocol for both (local) `Actor` and (potentially remote)
/// `DistributedActor`.
///
/// FIXME: !!! We'd need Actor to also conform to this, but don't want to add that conformance in _Concurrency yet.
@_marker
public protocol AnyActor: AnyObject {}

// ==== Distributed Actor -----------------------------------------------------

/// Common protocol to which all distributed actors conform implicitly.
///
/// It is not possible to conform to this protocol manually explicitly.
/// Only a 'distributed actor' declaration or protocol with 'DistributedActor'
/// requirement may conform to this protocol.
///
/// The 'DistributedActor' protocol provides the core functionality of any
/// distributed actor, which involves transforming actor
/// which involves enqueuing new partial tasks to be executed at some
/// point.
@available(SwiftStdlib 5.5, *)
public protocol DistributedActor: AnyActor, Codable, Hashable, Identifiable {

    /// Creates new (local) distributed actor instance, bound to the passed transport.
    ///
    /// Upon initialization, the `actorAddress` field is populated by the transport,
    /// with an address assigned to this actor.
    ///
    /// - Parameter transport: the transport this distributed actor instance will
    ///   associated with.
    init(transport: ActorTransport)

    /// Resolves the passed in `address` against the `transport`,
    /// returning either a local or remote actor reference.
    ///
    /// The transport will be asked to `resolve` the address and return either
    /// a local instance or determine that a proxy instance should be created
    /// for this address. A proxy actor will forward all invocations through
    /// the transport, allowing it to take over the remote messaging with the
    /// remote actor instance.
    ///
    /// - Parameter address: the address to resolve, and produce an instance or proxy for.
    /// - Parameter transport: transport which should be used to resolve the `address`.
    init(resolve identity: ActorIdentity, using transport: ActorTransport) throws

    /// The `ActorTransport` associated with this actor.
    /// It is immutable and equal to the transport passed in the local/resolve
    /// initializer.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var actorTransport: ActorTransport { get } // TODO: rename to `transport`?

    /// Logical address which this distributed actor represents.
    ///
    /// An address is always uniquely pointing at a specific actor instance.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var id: ActorIdentity { get } // NOTE: we cannot make this just ActorIdentity because the protocol would want to be hashable and equatable... and we can't do that heh
}

// ==== Hashable conformance ---------------------------------------------------

extension DistributedActor {
  public func hash(into hasher: inout Hasher) {
    self.transport.hashIdentity(underlying, into: &hasher)
  }

  public static func == (lhs: DistributedActor, rhs: DistributedActor) -> Bool {
    // TODO: not super great... we use the left's transport but either should/could be used
    lhs.transport.compareIdentities(lhs.id, rhs.id)
    // TODO: would we need to `&& rhs.transport.compare...` as well perhaps?
  }
}

// ==== Codable conformance ----------------------------------------------------

extension CodingUserInfoKey {
    @available(SwiftStdlib 5.5, *)
    static let actorTransportKey = CodingUserInfoKey(rawValue: "$dist_act_trans")!
}

@available(SwiftStdlib 5.5, *)
extension DistributedActor {
    nonisolated public init(from decoder: Decoder) throws {
//        guard let transport = decoder.userInfo[.actorTransportKey] as? ActorTransport else {
//            throw DistributedActorCodingError(message:
//            "ActorTransport not available under the decoder.userInfo")
//        }
//
//        var container = try decoder.singleValueContainer()
//        let address = try container.decode(ActorAddress.self)
//         self = try Self(resolve: address, using: transport) // FIXME: This is going to be solved by the init() work!!!!
        fatalError("\(#function) is not implemented yet for distributed actors'")
    }

    nonisolated public func encode(to encoder: Encoder) throws {
//        var container = encoder.singleValueContainer()
//        try container.encode(self.actorAddress)
//  /Users/ktoso/code/swift-project/swift/stdlib/public/Distributed/DistributedActor.swift:110:13: error: protocol 'ActorIdentity' as a type cannot conform to 'Encodable'
//  try container.encode(self.actorAddress)
//      ^
//      /Users/ktoso/code/swift-project/swift/stdlib/public/Distributed/DistributedActor.swift:110:13: note: only concrete types such as structs, enums and classes can conform to protocols
//  try container.encode(self.actorAddress)
//      ^
//      /Users/ktoso/code/swift-project/swift/stdlib/public/Distributed/DistributedActor.swift:110:13: note: required by instance method 'encode' where 'T' = 'ActorIdentity'
//  try container.encode(self.actorAddress)
//      ^

      try self.actorTransport.encodeIdentity(self.id, to: encoder)
    }
}

// ==== Equatable & Hashable conformance --------------------------------------

@available(SwiftStdlib 5.5, *)
extension DistributedActor {
  func hash(into hasher: inout Hasher) {
    self.id.hash(into: &hasher)
  }
}

/******************************************************************************/
/***************************** Actor Identity *********************************/
/******************************************************************************/

/// Uniquely identifies a distributed actor, and enables sending messages and identifying remote actors.
@available(SwiftStdlib 5.5, *)
public protocol ActorIdentity:
//    Codable,
    Sendable
    // , Equatable, Hashable
{
}

//@available(SwiftStdlib 5.5, *)
//public struct ActorIdentity: ActorIdentity {
//  private let underlying: ActorIdentity
//  private let transport: ActorTransport
//
//  public init(underlying identity: ActorIdentity, transport: ActorTransport) {
//    self.underlying = identity
//    self.transport = transport
//  }
//
//  public func hash(into hasher: inout Hasher) {
//    self.transport.hashIdentity(underlying, into: &hasher)
//  }
//
//  public static func == (lhs: ActorIdentity, rhs: ActorIdentity) -> Bool {
//    // TODO: not super great... we use the left's transport but either should/could be used
//    lhs.transport.compareIdentities(lhs.underlying, rhs.underlying)
//  }
//}

/******************************************************************************/
/******************************** Misc ****************************************/
/******************************************************************************/

/// Error protocol to which errors thrown by any `ActorTransport` should conform.
@available(SwiftStdlib 5.5, *)
public protocol ActorTransportError: Error {}

@available(SwiftStdlib 5.5, *)
public struct DistributedActorCodingError: ActorTransportError {
    public let message: String

    public init(message: String) {
        self.message = message
    }

    public static func missingTransportUserInfo<Act>(_ actorType: Act.Type) -> Self
        where Act: DistributedActor {
        .init(message: "Missing ActorTransport userInfo while decoding")
    }
}

/******************************************************************************/
/************************* Runtime Functions **********************************/
/******************************************************************************/

// ==== isRemote / isLocal -----------------------------------------------------

@_silgen_name("swift_distributed_actor_is_remote")
func __isRemoteActor(_ actor: AnyObject) -> Bool

func __isLocalActor(_ actor: AnyObject) -> Bool {
    return !__isRemoteActor(actor)
}

// ==== Proxy Actor lifecycle --------------------------------------------------

/// Called to initialize the distributed-remote actor 'proxy' instance in an actor.
/// The implementation will call this within the actor's initializer.
@_silgen_name("swift_distributedActor_remote_initialize")
func _distributedActorRemoteInitialize(_ actor: AnyObject)

/// Called to destroy the default actor instance in an actor.
/// The implementation will call this within the actor's deinit.
///
/// This will call `actorTransport.resignAddress(self.actorAddress)`.
@_silgen_name("swift_distributedActor_destroy")
func _distributedActorDestroy(_ actor: AnyObject)

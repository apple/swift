//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// Macros supporting distributed actor features.
//===----------------------------------------------------------------------===//

import Swift
import _Concurrency

#if $Macros

/// The distributed macro is a way to simply introduce "stub" types,
/// which concrete types named the same as the protocol they stub, however
/// prefixed with a `$`.
///
/// A distributed "stub" type cannot be instantiated and can only be used to
/// call ``resolve(id:using)`` on, which returns a remote distributed actor
/// reference identified by the passed in ID.
///
/// ## Examples
/// A distributed "greeter" protocol which can work with any distributed actor
/// system that
/// ```
/// import Distributed
///
/// public protocol Greeter<ActorSystem>: DistributedActor
///   where ActorSystem: DistributedActorSystem<any Codable> {
///
///   distributed func greet(name: String) -> String
/// }
/// ```
@attached(peer, names: prefixed(`$`)) // provides $Greeter concrete stub type
@attached(extension, names: arbitrary) // provides extension for Greeter & _DistributedActorStub
public macro _DistributedProtocol() =
  #externalMacro(module: "SwiftMacros", type: "DistributedProtocolMacro")

#endif

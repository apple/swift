// REQUIRES: swift_swift_parser, asserts
//
// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t-scratch)

// RUN: %target-swift-frontend -typecheck -verify -disable-availability-checking -plugin-path %swift-plugin-dir -I %t -dump-macro-expansions %s 2>&1 | %FileCheck %s

import Distributed

typealias System = LocalTestingDistributedActorSystem

@Resolvable
protocol Base: DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func base() -> Int
}
// CHECK: distributed actor $Base<ActorSystem>: Base
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>
// CHECK: {
// CHECK: }

// CHECK: extension Base where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func base() -> Int {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }

// ==== ------------------------------------------------------------------------

@Resolvable
protocol G3<ActorSystem>: DistributedActor, Base where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func get() -> String
  distributed func greet(name: String) -> String
}

// CHECK: distributed actor $G3<ActorSystem>: G3
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>
// CHECK: {
// CHECK: }

// CHECK: extension G3 where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func get() -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK:   distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }


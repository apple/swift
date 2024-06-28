// REQUIRES: swift_swift_parser, asserts
//
// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t-scratch)

// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -typecheck -verify -disable-availability-checking -plugin-path %swift-plugin-dir -parse-as-library -I %t %S/../Inputs/FakeDistributedActorSystems.swift -dump-macro-expansions %s -dump-macro-expansions 2>&1 | %FileCheck %s

import Distributed

@Resolvable
protocol Greeter: DistributedActor where ActorSystem == FakeActorSystem {
  distributed func greet(name: String) -> String
}
// CHECK: distributed actor $Greeter: Greeter,
// CHECK:    Distributed._DistributedActorStub
// CHECK: {
// CHECK:   typealias ActorSystem = FakeActorSystem
// CHECK: }

// CHECK: extension Greeter where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }

@Resolvable
protocol Greeter2: DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func greet(name: String) -> String
}
// CHECK: distributed actor $Greeter2<ActorSystem>: Greeter2,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>
// CHECK: {
// CHECK: }

// CHECK: extension Greeter2 where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }

extension String: CustomSerializationProtocol {
  public func toBytes() throws -> [UInt8] { [] }
  public static func fromBytes(_ bytes: [UInt8]) throws -> Self { "" }
}

@Resolvable
protocol Greeter3: DistributedActor where ActorSystem: DistributedActorSystem<any CustomSerializationProtocol> {
  distributed func greet(name: String) -> String
}
// CHECK: distributed actor $Greeter3<ActorSystem>: Greeter3,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any CustomSerializationProtocol>
// CHECK: {
// CHECK: }

// CHECK: extension Greeter3 where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }

@Resolvable
public protocol Greeter4: DistributedActor where ActorSystem == FakeActorSystem {
  distributed func greet(name: String) -> String
}
// CHECK: public distributed actor $Greeter4: Greeter4,
// CHECK:    Distributed._DistributedActorStub
// CHECK: {
// CHECK:   public typealias ActorSystem = FakeActorSystem
// CHECK: }

// CHECK: extension Greeter4 where Self: Distributed._DistributedActorStub {
// CHECK:   public distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }

@Resolvable
public protocol GreeterMore: DistributedActor where ActorSystem == FakeActorSystem {
  distributed var name: String { get }
  distributed func greet(name: String) -> String
  distributed func another(string: String, int: Int) async throws -> Double
  distributed func generic<T: Codable>(value: T, int: Int) async throws -> T
}
// CHECK: public distributed actor $GreeterMore: GreeterMore,
// CHECK:    Distributed._DistributedActorStub
// CHECK: {
// CHECK:   public typealias ActorSystem = FakeActorSystem
// CHECK: }

// CHECK: extension GreeterMore where Self: Distributed._DistributedActorStub {
// CHECK:   public distributed var  name : String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK:   public distributed func greet(name: String) -> String {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK:   public distributed func another(string: String, int: Int) async throws -> Double {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK:   public distributed func generic<T: Codable>(value: T, int: Int) async throws -> T {
// CHECK:     if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError()
// CHECK:     }
// CHECK:   }
// CHECK: }


// Should not fail:
public protocol MyActorWithSystemRequirement: DistributedActor where ActorSystem == FakeActorSystem {}

@Resolvable
public protocol MyActorWithSystemRequirementImpl: MyActorWithSystemRequirement {
  distributed func example()
}

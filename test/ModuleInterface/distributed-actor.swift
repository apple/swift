// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module -module-name Library \
// RUN:     -swift-version 5 -enable-library-evolution \
// RUN:     -o %t/Library.swiftmodule \
// RUN:     -emit-module-interface-path %t/Library.swiftinterface \
// RUN:     %t/Library.swift

/// Verify the interface
// RUN: %FileCheck %s < %t/Library.swiftinterface

/// Verify that we can build from the Library.swiftmodule
// RUN: %target-swift-frontend -typecheck -module-name Client \
// RUN:     -swift-version 5 \
// RUN:     %t/Client.swift -I%t

/// Verify what we can build from a swiftinterface, when swiftmodule was deleted
// RUN: rm %t/Library.swiftmodule
// RUN: %target-swift-frontend -typecheck -module-name Client \
// RUN:     -swift-version 5 \
// RUN:     %t/Client.swift -I%t

// REQUIRES: distributed

//--- Library.swift

import Distributed

// CHECK-NOT:  #if compiler(>=5.3) && $Actors
// CHECK:      @available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *)
// CHECK-NEXT: distributed public actor DA {
@available(SwiftStdlib 5.7, *)
public distributed actor DA {
  // CHECK: @_compilerInitialized nonisolated final public let id: Distributed.LocalTestingDistributedActorSystem.ActorID
  // CHECK: nonisolated final public let actorSystem: Library.DA.ActorSystem
  // CHECK: public typealias ActorSystem = Distributed.LocalTestingDistributedActorSystem
  public typealias ActorSystem = LocalTestingDistributedActorSystem

  // CHECK:       public static func resolve(id: Distributed.LocalTestingDistributedActorSystem.ActorID, using system: Library.DA.ActorSystem) throws -> Library.DA
  // CHECK:       public typealias ID = Distributed.LocalTestingDistributedActorSystem.ActorID
  // CHECK:       public typealias SerializationRequirement = any Swift.Decodable & Swift.Encodable
  // CHECK:       {{@objc deinit|deinit}}
  // CHECK:       nonisolated public var hashValue: Swift.Int {
  // CHECK-NEXT:    get
  // CHECK-NEXT:  }
  // CHECK:       public init(actorSystem system: Library.DA.ActorSystem)
  // CHECK:       @available(iOS 16.0, tvOS 16.0, watchOS 9.0, macOS 13.0, *)
  // CHECK-NEXT:  @_semantics("defaultActor") nonisolated final public var unownedExecutor: _Concurrency.UnownedSerialExecutor {
  // CHECK-NEXT:    get
  // CHECK-NEXT:  }
}

// CHECK-NOT: #if compiler(>=5.3) && $Actors
// CHECK: @available({{.*}})
// CHECK-NEXT: distributed public actor DAG<ActorSystem> where ActorSystem : Distributed.DistributedActorSystem, ActorSystem.SerializationRequirement == any Swift.Decodable & Swift.Encodable {
@available(SwiftStdlib 6.0, *)
public distributed actor DAG<ActorSystem> where ActorSystem: DistributedActorSystem<any Codable> {
  // CHECK: @_compilerInitialized nonisolated final public let id: ActorSystem.ActorID
  // CHECK: nonisolated final public let actorSystem: ActorSystem

// CHECK: public static func resolve(id: ActorSystem.ActorID, using system: ActorSystem) throws -> Library.DAG<ActorSystem>
// CHECK: public typealias ID = ActorSystem.ActorID
// CHECK: public typealias SerializationRequirement = any Swift.Decodable & Swift.Encodable
// CHECK: {{@objc deinit|deinit}}
// CHECK: nonisolated public var hashValue: Swift.Int {
// CHECK:   get
// CHECK: }
// CHECK: public init(actorSystem system: ActorSystem)
// CHECK: @available(iOS 18.0, tvOS 18.0, watchOS 11.0, visionOS 2.0, macOS 15.0, *)
// CHECK: @_semantics("defaultActor") nonisolated final public var unownedExecutor: _Concurrency.UnownedSerialExecutor {
// CHECK:   get
// CHECK: }
}

// CHECK-NOT: #if compiler(>=5.3) && $Actors
// CHECK:     @available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *)
// CHECK-NEXT:extension Library.DA : Distributed.DistributedActor {}
// CHECK-NOT: #if compiler(>=5.3) && $Actors
// CHECK:     @available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *)
// CHECK-NEXT:extension Library.DA : Swift.Encodable {}
// CHECK-NOT: #if compiler(>=5.3) && $Actors
// CHECK:     @available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *)
// CHECK-NEXT:extension Library.DA : Swift.Decodable {}

// CHECK-NOT: #if compiler(>=5.3) && $Actors
// CHECK: @available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *)
// CHECK-NEXT: extension Library.DAG : Distributed.DistributedActor {}

//--- Client.swift

import Distributed
import Library

@available(SwiftStdlib 6.0, *)
func main() {
  let da: DA? = nil
  _ = da

  let dag: DAG<LocalTestingDistributedActorSystem>? = nil
  _ = dag
}

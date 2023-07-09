// RUN: %target-swift-frontend -emit-ir %s -swift-version 5 -disable-availability-checking | %IRGenFileCheck %s
// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed

// Type descriptor.
// CHECK-LABEL: @"$s17distributed_actor7MyActorC2id11Distributed012LocalTestingD2IDVvpWvd"
@available(SwiftStdlib 5.6, *)
public distributed actor MyActor {
  public typealias ActorSystem = LocalTestingDistributedActorSystem
  // nothing
}

/// This combination of DistributedActor + Codable used to trigger a crash in DistributedAccessor::emit (rdar://111664985)
/// So returning it from distributed methods in the types below covers this radar.
public protocol ClusterSingleton: DistributedActor, Codable {}

@available(SwiftStdlib 5.6, *)
public distributed actor MyActorGenerics {
  public typealias ActorSystem = LocalTestingDistributedActorSystem

  distributed func find<Act: ClusterSingleton>(byName name: String) -> Act {
    fatalError("mock impl")
  }
}

@available(SwiftStdlib 5.6, *)
public distributed actor MyActorGenericsOnType<Act: ClusterSingleton> {
  public typealias ActorSystem = LocalTestingDistributedActorSystem

  distributed func find(byName name: String) -> Act {
    fatalError("mock impl")
  }
}

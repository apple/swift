// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -typecheck -verify -enable-experimental-distributed -disable-availability-checking -I %t 2>&1 %s
// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed
import FakeDistributedActorSystems


typealias DefaultDistributedActorSystem = FakeActorSystem

protocol SerializableButNotCodable {}
func takeSerializableButNotCodable<A: SerializableButNotCodable>(actor: A) {}

// ==== ------------------------------------------------------------------------

distributed actor DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>: SerializableButNotCodable
  where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
        ActorSystem.ActorID: SerializableButNotCodable {
}

func test_DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>(actorSystem: ActorSystem)
    where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
          ActorSystem.ActorID: SerializableButNotCodable {
  takeSerializableButNotCodable(actor: DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>(actorSystem: actorSystem))
}
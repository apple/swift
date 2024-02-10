// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -typecheck -verify -enable-experimental-distributed -disable-availability-checking -I %t 2>&1 %s
// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed
import FakeDistributedActorSystems

func takeCodable<A: Codable>(actor: A) {}
// expected-note@-1{{where 'A' = 'DAG<FakeActorSystem>'}}
// expected-note@-2{{where 'A' = 'DAG<FakeActorSystem>'}}
// expected-note@-3{{where 'A' = 'DAG_ActorSystem_ActorID_Custom<ActorSystem>'}}
// expected-note@-4{{where 'A' = 'DAG_ActorSystem_ActorID<FakeActorSystem>'}}
// expected-note@-5{{where 'A' = 'DAG_ID<FakeActorSystem>'}}
// expected-note@-6{{where 'A' = 'DAG_ActorSystem_ActorID_Custom<ActorSystem>'}}
// expected-note@-7{{where 'A' = 'DAG_ActorSystem_ActorID<FakeActorSystem>'}}
// expected-note@-8{{where 'A' = 'DAG_ID<FakeActorSystem>'}}

// ==== Actors -----------------------------------------------------------------

distributed actor DA {
  typealias ActorSystem = FakeActorSystem
}

distributed actor DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>: SerializableButNotCodable
  where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
  ActorSystem.ActorID: SerializableButNotCodable {
}

func test_DA(actorSystem: FakeActorSystem) {
  takeCodable(actor: DA(actorSystem: actorSystem)) // ok
}

// ==== Generic actors ---------------------------------------------------------

// --- Not codable cases -------------------------------------------------------

distributed actor DAG<ActorSystem>
  where ActorSystem: DistributedActorSystem<any Codable> {
}
func test_DAG(actorSystem: FakeActorSystem) {
  // expected-error@+2{{global function 'takeCodable(actor:)' requires that 'DAG<FakeActorSystem>' conform to 'Decodable'}}
  // expected-error@+1{{global function 'takeCodable(actor:)' requires that 'DAG<FakeActorSystem>' conform to 'Encodable'}}
  takeCodable(actor: DAG<FakeActorSystem>(actorSystem: actorSystem))
}

distributed actor DAG_ID<ActorSystem>
  where ActorSystem: DistributedActorSystem<any Codable>,
        ID: Codable { // expected-error{{cannot find type 'ID' in scope}}
}
func test_DAG_ID(actorSystem: FakeActorSystem) {
  // expected-error@+2{{global function 'takeCodable(actor:)' requires that 'DAG_ID<FakeActorSystem>' conform to 'Decodable'}}
  // expected-error@+1{{global function 'takeCodable(actor:)' requires that 'DAG_ID<FakeActorSystem>' conform to 'Encodable'}}
  takeCodable(actor: DAG_ID<FakeActorSystem>(actorSystem: actorSystem))
}

distributed actor DAG_ActorSystem_ActorID<ActorSystem> where ActorSystem: DistributedActorSystem<any Codable>,
  ActorSystem.ActorID: Codable {
}
func test_DAG_ActorSystem_ActorID(actorSystem: FakeActorSystem) {
  // expected-error@+2{{global function 'takeCodable(actor:)' requires that 'DAG_ActorSystem_ActorID<FakeActorSystem>' conform to 'Decodable'}}
  // expected-error@+1{{global function 'takeCodable(actor:)' requires that 'DAG_ActorSystem_ActorID<FakeActorSystem>' conform to 'Encodable'}}
  takeCodable(actor: DAG_ActorSystem_ActorID<FakeActorSystem>(actorSystem: actorSystem))
}

protocol SerializableButNotCodable {
  func serialize()
}
extension DistributedActor where ActorSystem.ActorID: SerializableButNotCodable {
  nonisolated func serialize() {
    // get id and serialize it
  }
}

func takeSerializableButNotCodable<A: SerializableButNotCodable>(actor: A) {}

distributed actor DAG_ActorSystem_ActorID_Custom<ActorSystem>
    where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
  ActorSystem.ActorID: SerializableButNotCodable {
}
func test_DAG_ActorSystem_ActorID_Custom<ActorSystem>(actorSystem: ActorSystem)
    where ActorSystem: DistributedActorSystem<any SerializableButNotCodable> {
  // expected-error@+1{{type 'ActorSystem.ActorID' does not conform to protocol 'SerializableButNotCodable'}}
  takeCodable(actor: DAG_ActorSystem_ActorID_Custom<ActorSystem>(actorSystem: actorSystem))
}

func test_DAG_ActorSystem_ActorID_Custom<ActorSystem>(actorSystem: ActorSystem)
    where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
          ActorSystem.ActorID: SerializableButNotCodable {
  // expected-error@+2{{global function 'takeCodable(actor:)' requires that 'DAG_ActorSystem_ActorID_Custom<ActorSystem>' conform to 'Decodable'}}
  // expected-error@+1{{global function 'takeCodable(actor:)' requires that 'DAG_ActorSystem_ActorID_Custom<ActorSystem>' conform to 'Encodable'}}
  takeCodable(actor: DAG_ActorSystem_ActorID_Custom<ActorSystem>(actorSystem: actorSystem))
}

func test_DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>(actorSystem: ActorSystem)
    where ActorSystem: DistributedActorSystem<any SerializableButNotCodable>,
          ActorSystem.ActorID: SerializableButNotCodable {
  takeSerializableButNotCodable(actor: DAG_ActorSystem_ActorID_SerializableButNotCodable_Explicitly<ActorSystem>(actorSystem: actorSystem))
}
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main  -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Foundation
import Distributed

final class LocalActorSystem : DistributedActorSystem {
  typealias ActorID = LocalTestingActorID
  typealias ResultHandler = LocalTestingInvocationResultHandler
  typealias InvocationEncoder = LocalTestingInvocationEncoder
  typealias InvocationDecoder = LocalTestingInvocationDecoder
  typealias SerializationRequirement = any Codable

  func makeInvocationEncoder() -> InvocationEncoder { LocalTestingDistributedActorSystem().makeInvocationEncoder() }

  func resolve<Act>(id: ActorID, as actorType: Act.Type) throws -> Act? where Act: DistributedActor {
    nil
  }

  func actorReady<Act>(_ actor: Act) where Act: DistributedActor, Act.ID == ActorID {
  }

  func resignID(_ id: ActorID) {}

  func assignID<Act>(_ actorType: Act.Type) -> ActorID
    where Act: DistributedActor {
    .init(id: "42")
  }

  func remoteCall<Act, Err, Res>(on actor: Act,
                                 target: RemoteCallTarget,
                                 invocation: inout InvocationEncoder,
                                 throwing: Err.Type,
                                 returning: Res.Type) async throws -> Res
    where Act: DistributedActor,
          Act.ID == ActorID,
          Err: Error,
          Res: Codable {
    let decoder = JSONDecoder()
    return try! decoder.decode(Res.self, from: await fetchData())
  }

  func remoteCallVoid<Act, Err>(on actor: Act,
                                target: RemoteCallTarget,
                                invocation: inout InvocationEncoder,
                                throwing errorType: Err.Type) async throws
    where Act: DistributedActor,
          Act.ID == ActorID,
          Err: Error {
    fatalError("not implemented: \(#function)")
  }

  func fetchData() async -> Data {
    "42".data(using: .ascii)!
  }
}

distributed actor NotCodableDA<ActorSystem>
  where ActorSystem: DistributedActorSystem<any Codable> {

  init(actorSystem: ActorSystem) async {
    self.actorSystem = actorSystem

    print(try! await self.request())
  }

  func request() async throws -> Int {
    let target = RemoteCallTarget("test.request")
    var encoder = actorSystem.makeInvocationEncoder()
    return try await actorSystem.remoteCall(on: self,
      target: target,
      invocation: &encoder,
      throwing: Error.self,
      returning: Int.self)
  }
}

distributed actor CodableDA<ActorSystem>: Codable
  where ActorSystem: DistributedActorSystem<any Codable>,
        ActorSystem.ActorID: Codable {

  init(actorSystem: ActorSystem) async {
    self.actorSystem = actorSystem

    print(try! await self.request())
  }

  func request() async throws -> Int {
    let target = RemoteCallTarget("test.request")
    var encoder = actorSystem.makeInvocationEncoder()
    return try await actorSystem.remoteCall(on: self,
      target: target,
      invocation: &encoder,
      throwing: Error.self,
      returning: Int.self)
  }
}

distributed actor CodableIDDA<ActorSystem>
  where ActorSystem: DistributedActorSystem<any Codable>,
        ActorSystem.ActorID: Codable {

  init(actorSystem: ActorSystem) async {
    self.actorSystem = actorSystem

    print(try! await self.request())
  }

  func request() async throws -> Int {
    let target = RemoteCallTarget("test.request")
    var encoder = actorSystem.makeInvocationEncoder()
    return try await actorSystem.remoteCall(on: self,
      target: target,
      invocation: &encoder,
      throwing: Error.self,
      returning: Int.self)
  }
}

@main struct Main {
  static func main() async throws {
    if #available(SwiftStdlib 5.9, *) {
      let system = LocalActorSystem()
      let ncda = await NotCodableDA(actorSystem: system)
      let cidda = await CodableIDDA(actorSystem: system)
      let cda = await CodableDA(actorSystem: system)

      try await ncda.whenLocal {
        let got = try await $0.request()

        // CHECK: got = 42
        print("got = \(got)")
      }

      try await cidda.whenLocal {
        let got = try await $0.request()

        // CHECK: got = 42
        print("got = \(got)")
      }

      let c: any (DistributedActor & Codable) = cda
      try await cda.whenLocal {
        let got = try await $0.request()

        // CHECK: got = 42
        print("got = \(got)")
      }

      // CHECK: OK
      print("OK")
    }
  }
}
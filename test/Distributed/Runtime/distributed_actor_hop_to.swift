// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main -Xfrontend -enable-experimental-distributed -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows, isRemote always returns false. rdar://82593574
// UNSUPPORTED: OS=windows-msvc


import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeRoundtripActorSystem

protocol LifecycleWatch: DistributedActor where ActorSystem == FakeRoundtripActorSystem {
}

extension LifecycleWatch {
  func watch<T: Codable>(x: Int, _ y: T) async throws {
    // nothing here
    print("executed: \(#function) - x = \(x), y = \(y)")
  }

  distributed func test<T: Codable>(x: Int, _ y: T) async throws {
    print("executed: \(#function)")
    try await self.watch(x: x, y)
    print("done executed: \(#function)")
  }
}

distributed actor Worker: LifecycleWatch {
}

@main struct Main {
  static func main() async {
    let actorSystem = DefaultDistributedActorSystem()

    let worker: any LifecycleWatch = Worker(actorSystem: actorSystem)
    try! await worker.test(x: 42, "on protocol")

    // CHECK: executed: test(x:_:)
    // CHECK: executed: watch(x:_:) - x = 42, y = on protocol
    // CHECK: done executed: test(x:_:)

    // ==== --------------------------------------------------------------------

    let remote = try! Worker.resolve(id: worker.id, using: actorSystem)
    try! await remote.test(x: 42, "direct") // Remote call
    // CHECK: >> remoteCallVoid: on:main.Worker, target:test(x:_:)
    // CHECK: << onReturnVoid: ()

    let remoteAny: any LifecycleWatch = remote
    try! await remoteAny.test(x: 42, "direct") // Remote call
    // CHECK: >> remoteCallVoid: on:main.Worker, target:test(x:_:)
    // CHECK: << onReturnVoid: ()

    let remoteAnyOptional: (any LifecycleWatch)? = remote
    try! await remoteAnyOptional?.test(x: 42, "direct") // Remote call
    // CHECK: >> remoteCallVoid: on:main.Worker, target:test(x:_:)
    // CHECK: << onReturnVoid: ()

    print("OK") // CHECK: OK
  }
}

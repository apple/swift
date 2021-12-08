// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-distributed -parse-as-library) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import _Distributed

@available(SwiftStdlib 5.6, *)
distributed actor SomeSpecificDistributedActor {

  distributed func hello() async throws {
     print("hello from \(self.id)")
  }

  distributed func echo(int: Int) async throws -> Int {
    int
  }
}

// ==== Execute ----------------------------------------------------------------

@_silgen_name("swift_distributed_actor_is_remote")
func __isRemoteActor(_ actor: AnyObject) -> Bool

func __isLocalActor(_ actor: AnyObject) -> Bool {
  return !__isRemoteActor(actor)
}

// ==== Fake Transport ---------------------------------------------------------

@available(SwiftStdlib 5.6, *)
struct ActorAddress: Sendable, Hashable, Codable {
  let address: String
  init(parse address : String) {
    self.address = address
  }
}

@available(SwiftStdlib 5.6, *)
struct FakeActorSystem: DistributedActorSystem {

  func resolve<Act>(id: ActorID, as actorType: Act.Type) throws -> Act?
      where Act: DistributedActor,
            Act.ID == ActorID {
    return nil
  }

  func assignID<Act>(_ actorType: Act.Type) -> AnyActorIdentity
      where Act: DistributedActor {
    .init(ActorAddress(parse: ""))
  }

  public func actorReady<Act>(_ actor: Act)
      where Act: DistributedActor {
    print("\(#function):\(actor)")
  }

  func resignID(_ id: AnyActorIdentity) {}
}

@available(SwiftStdlib 5.6, *)
typealias DefaultDistributedActorSystem = FakeActorSystem

// ==== Execute ----------------------------------------------------------------

@available(SwiftStdlib 5.6, *)
func test_initializers() {
  let address = ActorAddress(parse: "")
  let transport = FakeActorSystem()

  _ = SomeSpecificDistributedActor(transport: transport)
  _ = try! SomeSpecificDistributedActor.resolve(.init(address), using: transport)
}

@available(SwiftStdlib 5.6, *)
func test_address() {
  let transport = FakeActorSystem()

  let actor = SomeSpecificDistributedActor(transport: transport)
  _ = actor.id
}

@available(SwiftStdlib 5.6, *)
func test_run(transport: FakeActorSystem) async {
  let actor = SomeSpecificDistributedActor(transport: transport)

  print("before") // CHECK: before
  try! await actor.hello()
  print("after") // CHECK: after
}

@available(SwiftStdlib 5.6, *)
func test_echo(transport: FakeActorSystem) async {
  let actor = SomeSpecificDistributedActor(transport: transport)

  let echo = try! await actor.echo(int: 42)
  print("echo: \(echo)") // CHECK: echo: 42
}

@available(SwiftStdlib 5.6, *)
@main struct Main {
  static func main() async {
    await test_run(transport: FakeActorSystem())
    await test_echo(transport: FakeActorSystem())
  }
}

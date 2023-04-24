// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows, isRemote always returns false. rdar://82593574
// UNSUPPORTED: OS=windows-msvc

import Distributed

actor A {}

distributed actor DA {
  init(system: FakeActorSystem) {
    self.actorSystem = system
  }
}

distributed actor DA_userDefined {
  init(system: FakeActorSystem) {
    self.actorSystem = system
  }

  nonisolated deinit {}
}

distributed actor DA_userDefined2 {
  init(system: FakeActorSystem) {
    self.actorSystem = system
  }

  nonisolated deinit {
    print("Deinitializing \(self.id) remote:\(__isRemoteActor(self))")
  }
}

distributed actor DA_state {
  var name = "Hello"
  var age = 42

  init(system: FakeActorSystem) {
    self.actorSystem = system
  }

  nonisolated deinit {
    print("Deinitializing \(self.id) remote:\(__isRemoteActor(self))")
    return
  }
}

// ==== Fake Transport ---------------------------------------------------------

struct ActorAddress: Sendable, Hashable, Codable {
  let address: String
  init(parse address : String) {
    self.address = address
  }
}

final class FakeActorSystem: @unchecked Sendable, DistributedActorSystem {
  typealias ActorID = ActorAddress
  typealias SerializationRequirement = Codable
  typealias InvocationDecoder = FakeDistributedInvocationEncoder
  typealias InvocationEncoder = FakeDistributedInvocationEncoder
  typealias ResultHandler = FakeResultHandler

  var n = 0

  func resolve<Act>(id: ActorID, as actorType: Act.Type) throws -> Act?
      where Act: DistributedActor,
            Act.ID == ActorID {
    print("resolve type:\(actorType), address:\(id)")
    return nil
  }

  func assignID<Act>(_ actorType: Act.Type) -> ActorID
      where Act: DistributedActor,
            Act.ID == ActorID {
    n += 1
    let address = ActorAddress(parse: "addr-\(n)")
    print("assign type:\(actorType), address:\(address)")
    return address
  }

  func actorReady<Act>(_ actor: Act)
      where Act: DistributedActor,
      Act.ID == ActorID {
    print("ready actor:\(actor), address:\(actor.id)")
  }

  func resignID(_ id: ActorID) {
    print("resign address:\(id)")
  }

  @inlinable func makeInvocationEncoder() -> InvocationEncoder {
    .init()
  }

  func remoteCall<Act, Err, Res>(
    on actor: Act,
    target: RemoteCallTarget,
    invocation invocationEncoder: inout InvocationEncoder,
    throwing: Err.Type,
    returning: Res.Type
  ) async throws -> Res
    where Act: DistributedActor,
    Act.ID == ActorID,
    Err: Error,
    Res: SerializationRequirement {
    fatalError("not implemented: \(#function)")
  }

  func remoteCallVoid<Act, Err>(
    on actor: Act,
    target: RemoteCallTarget,
    invocation invocationEncoder: inout InvocationEncoder,
    throwing: Err.Type
  ) async throws
    where Act: DistributedActor,
    Act.ID == ActorID,
    Err: Error {
    fatalError("not implemented: \(#function)")
  }
}

class FakeDistributedInvocationEncoder: DistributedTargetInvocationEncoder, DistributedTargetInvocationDecoder {
  typealias SerializationRequirement = Codable

  func recordGenericSubstitution<T>(_ type: T.Type) throws { }
  func recordArgument<Value: SerializationRequirement>(_ argument: RemoteCallArgument<Value>) throws { }
  func recordReturnType<R: SerializationRequirement>(_ type: R.Type) throws { }
  func recordErrorType<E: Error>(_ type: E.Type) throws { }
  func doneRecording() throws { }

  // === Receiving / decoding -------------------------------------------------

  func decodeGenericSubstitutions() throws -> [Any.Type] {
    []
  }
  func decodeNextArgument<Argument: SerializationRequirement>() throws -> Argument {
    fatalError()
  }
  func decodeReturnType() throws -> Any.Type? {
    nil
  }
  func decodeErrorType() throws -> Any.Type? {
    nil
  }
}

@available(SwiftStdlib 5.5, *)
public struct FakeResultHandler: DistributedTargetInvocationResultHandler {
  public typealias SerializationRequirement = Codable

  public func onReturn<Success: SerializationRequirement>(value: Success) async throws {
    fatalError("Not implemented: \(#function)")
  }

  public func onReturnVoid() async throws {
    fatalError("Not implemented: \(#function)")
  }

  public func onThrow<Err: Error>(error: Err) async throws {
    fatalError("Not implemented: \(#function)")
  }
}

typealias DefaultDistributedActorSystem = FakeActorSystem

// ==== Execute ----------------------------------------------------------------

func test() {
  let system = DefaultDistributedActorSystem()

  // no lifecycle things make sense for a normal actor, double check we didn't emit them
  print("before A")
  _ = A()
  print("after A")
  // CHECK: before A
  // CHECK: after A

  _ = { () -> DA in
    DA(system: system)
  }()
  // CHECK: assign type:DA, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA, address:ActorAddress(address: "[[ADDR1:addr-[0-9]]]")
  // CHECK: resign address:ActorAddress(address: "[[ADDR1]]")

  _ = { () -> DA_userDefined in
    DA_userDefined(system: system)
  }()
  // CHECK: assign type:DA_userDefined, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined, address:ActorAddress(address: "[[ADDR2:addr-[0-9]]]")
  // CHECK: resign address:ActorAddress(address: "[[ADDR2]]")

  // resign must happen as the _last thing_ after user-deinit completed
  _ = { () -> DA_userDefined2 in
    DA_userDefined2(system: system)
  }()
  // CHECK: assign type:DA_userDefined2, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined2, address:ActorAddress(address: "[[ADDR3:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR3]]") remote:false
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR3]]")

  // resign must happen as the _last thing_ after user-deinit completed
  _ = { () -> DA_state in
    DA_state(system: system)
  }()
  // CHECK: assign type:DA_state, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_state, address:ActorAddress(address: "[[ADDR4:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR4]]") remote:false
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR4]]")

  // a remote actor should not resign it's address, it was never "assigned" it
  let address = ActorAddress(parse: "remote-1")
  _ = { () -> DA_userDefined2 in
    try! DA_userDefined2.resolve(id: address, using: system)
  }()
  // CHECK-NEXT: resolve type:DA_userDefined2, address:ActorAddress(address: "[[ADDR5:remote-1]]")
  // MUST NOT run deinit body for a remote distributed actor
  // CHECK-NOT: Deinitializing ActorAddress(address: "remote-1") remote:true

  print("DONE")
  // CHECK-NEXT: DONE
}

@main struct Main {
  static func main() async {
    test()
  }
}

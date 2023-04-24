// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-stdlib -parse-as-library) | %FileCheck %s

// REQUIRES: libdispatch
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows, isRemote always returns false. rdar://82593574
// UNSUPPORTED: OS=windows-msvc

import Swift
import _Concurrency
import Distributed
import Dispatch

#if canImport(Darwin)
import Darwin
typealias ThreadID = pthread_t
func getCurrentThreadID() -> ThreadID { pthread_self() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { pthread_equal(a, b) != 0 }
#elseif canImport(Glibc)
import Glibc
typealias ThreadID = pthread_t
func getCurrentThreadID() -> ThreadID { pthread_self() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { pthread_equal(a, b) != 0 }
#elseif os(Windows)
import WinSDK
typealias ThreadID = UInt32
func getCurrentThreadID() -> ThreadID { GetCurrentThreadId() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { a == b }
#endif

var mainThread: ThreadID?
func isMainThread() -> Bool {
    return equalThreadIDs(getCurrentThreadID(), mainThread!)
}

@_silgen_name("swift_task_isCurrentExecutor")
private func isCurrentExecutor(_ executor: Builtin.Executor) -> Bool

func getExecutor(_ a: AnyActor) -> Builtin.Executor {
  let pack = (a, UnsafeRawPointer?.none)
  return unsafeBitCast(pack, to: Builtin.Executor.self)
}

func isCurrent(_ a: AnyActor) -> Bool {
  return isCurrentExecutor(getExecutor(a))
}

func isMainExecutor() -> Bool {
  isCurrentExecutor(Builtin.buildMainActorExecutorRef())
}

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

distributed actor DA_userDefined_nonisolated {
  init(system: FakeActorSystem) {
    self.actorSystem = system
  }

  nonisolated deinit {
    print("Deinitializing \(self.id) remote:\(__isRemoteActor(self)) isolated:\(isCurrent(self)) mainThread:\(isMainThread())")
  }
}

distributed actor DA_userDefined_isolated {
  init(system: FakeActorSystem) {
    self.actorSystem = system
  }

  isolated deinit {
    print("Deinitializing \(self.id) remote:\(__isRemoteActor(self)) isolated:\(isCurrent(self)) mainThread:\(isMainThread())")
  }
}

distributed actor DA_state_nonisolated {
  var name: String
  var age: Int

  init(name: String, age: Int, system: FakeActorSystem) {
    self.name = name
    self.age = age
    self.actorSystem = system
  }

  nonisolated deinit {
    print("Deinitializing \(self.id) name=\(name) age=\(age) remote:\(__isRemoteActor(self)) isolated:\(isCurrent(self)) mainThread:\(isMainThread())")
    return
  }
}

distributed actor DA_state_isolated {
  var name: String
  var age: Int

  init(name: String, age: Int, system: FakeActorSystem) {
    self.name = name
    self.age = age
    self.actorSystem = system
  }

  isolated deinit {
    print("Deinitializing \(self.id) name=\(name) age=\(age) remote:\(__isRemoteActor(self)) isolated:\(isCurrent(self)) mainThread:\(isMainThread())")
    return
  }
}

@globalActor actor AnotherActor: GlobalActor {
  static let shared = AnotherActor()
}

distributed actor DA_state_isolated_on_another {
  let name: String
  let age: Int

  init(name: String, age: Int, system: FakeActorSystem) {
    self.name = name
    self.age = age
    self.actorSystem = system
  }

  @AnotherActor
  deinit {
    print("Deinitializing \(self.id) name=\(name) age=\(age) remote:\(__isRemoteActor(self)) isolated-self:\(isCurrent(self)) isolated-other:\(isCurrent(AnotherActor.shared)) mainThread:\(isMainThread())")
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
  let group: DispatchGroup
  
  init(group: DispatchGroup) {
    self.group = group
  }
  
  deinit {
    print("Deinit ActorSystem: mainExecutor=\(isMainExecutor()) mainThread=\(isMainThread())")
    group.leave()
  }

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
  let group = DispatchGroup()

  // no lifecycle things make sense for a normal actor, double check we didn't emit them
  print("before A")
  _ = A()
  print("after A")
  // CHECK: before A
  // CHECK: after A

  group.enter()
  _ = { () -> DA in
    DA(system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA, address:ActorAddress(address: "[[ADDR1:addr-[0-9]]]")
  // CHECK: resign address:ActorAddress(address: "[[ADDR1]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true

  group.enter()
  _ = { () -> DA_userDefined in
    DA_userDefined(system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_userDefined, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined, address:ActorAddress(address: "[[ADDR2:addr-[0-9]]]")
  // CHECK: resign address:ActorAddress(address: "[[ADDR2]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true

  // resign must happen as the _last thing_ after user-deinit completed
  group.enter()
  _ = { () -> DA_userDefined_nonisolated in
    DA_userDefined_nonisolated(system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_userDefined_nonisolated, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined_nonisolated, address:ActorAddress(address: "[[ADDR3:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR3]]") remote:false isolated:false mainThread:true
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR3]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true
  
  // resign must happen as the _last thing_ after user-deinit completed
  group.enter()
  _ = { () -> DA_userDefined_isolated in
    DA_userDefined_isolated(system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_userDefined_isolated, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined_isolated, address:ActorAddress(address: "[[ADDR4:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR4]]") remote:false isolated:true mainThread:true
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR4]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=false mainThread=true

  // resign must happen as the _last thing_ after user-deinit completed
  group.enter()
  _ = { () -> DA_state_nonisolated in
    DA_state_nonisolated(name: "Foo", age:37, system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_state_nonisolated, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_state_nonisolated, address:ActorAddress(address: "[[ADDR5:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR5]]") name=Foo age=37 remote:false isolated:false mainThread:true
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR5]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true
  
  // resign must happen as the _last thing_ after user-deinit completed
  group.enter()
  _ = { () -> DA_state_isolated in
    DA_state_isolated(name: "Bar", age:42, system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_state_isolated, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_state_isolated, address:ActorAddress(address: "[[ADDR6:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR6]]") name=Bar age=42 remote:false isolated:true mainThread:true
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR6]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=false mainThread=true
  
  // resign must happen as the _last thing_ after user-deinit completed
  group.enter()
  _ = { () -> DA_state_isolated_on_another in
    DA_state_isolated_on_another(name: "Baz", age:57, system: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK: assign type:DA_state_isolated_on_another, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_state_isolated_on_another, address:ActorAddress(address: "[[ADDR6:addr-[0-9]]]")
  // CHECK: Deinitializing ActorAddress(address: "[[ADDR6]]") name=Baz age=57 remote:false isolated-self:false isolated-other:true mainThread:false
  // CHECK-NEXT: resign address:ActorAddress(address: "[[ADDR6]]")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=false mainThread=false

  // a remote actor should not resign it's address, it was never "assigned" it
  group.enter()
  _ = { () -> DA_userDefined_nonisolated in
    let address = ActorAddress(parse: "remote-1")
    return try! DA_userDefined_nonisolated.resolve(id: address, using: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK-NEXT: resolve type:DA_userDefined_nonisolated, address:ActorAddress(address: "remote-1")
  // MUST NOT run deinit body for a remote distributed actor
  // CHECK-NOT: Deinitializing ActorAddress(address: "remote-1")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true
  
  // a remote actor should not resign it's address, it was never "assigned" it
  group.enter()
  _ = { () -> DA_userDefined_isolated in
    let address = ActorAddress(parse: "remote-2")
    return try! DA_userDefined_isolated.resolve(id: address, using: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK-NEXT: resolve type:DA_userDefined_isolated, address:ActorAddress(address: "remote-2")
  // MUST NOT run deinit body for a remote distributed actor
  // CHECK-NOT: Deinitializing ActorAddress(address: "remote-2")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true
  
  // a remote actor should not resign it's address, it was never "assigned" it
  group.enter()
  _ = { () -> DA_state_isolated_on_another in
    let address = ActorAddress(parse: "remote-3")
    return try! DA_state_isolated_on_another.resolve(id: address, using: DefaultDistributedActorSystem(group: group))
  }()
  group.wait()
  // CHECK-NEXT: resolve type:DA_state_isolated_on_another, address:ActorAddress(address: "remote-3")
  // MUST NOT run deinit body for a remote distributed actor
  // CHECK-NOT: Deinitializing ActorAddress(address: "remote-3")
  // CHECK-NEXT: Deinit ActorSystem: mainExecutor=true mainThread=true

  print("DONE")
  // CHECK-NEXT: DONE
}

@main struct Main {
  static func main() async {
    mainThread = getCurrentThreadID()
    test()
  }
}

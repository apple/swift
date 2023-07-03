// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -typecheck -verify -verify-ignore-unknown -disable-availability-checking -I %t 2>&1 %s
// REQUIRES: concurrency
// REQUIRES: distributed

// TODO(distributed): rdar://82419661 remove -verify-ignore-unknown here, no warnings should be emitted for our
//  generated code but right now a few are, because of Sendability checks -- need to track it down more.

import Distributed
import FakeDistributedActorSystems

@available(SwiftStdlib 5.5, *)
typealias DefaultDistributedActorSystem = FakeActorSystem

// ==== ----------------------------------------------------------------------------------------------------------------

actor LocalActor_1 {
  let name: String = "alice"
  var mutable: String = ""

  distributed func nope() {
    // expected-error@-1{{'distributed' method can only be declared within 'distributed actor'}}
  }
}

struct NotCodableValue { }

distributed actor DistributedActor_1 {

  let name: String = "alice" // expected-note{{access to property 'name' is only permitted within distributed actor 'DistributedActor_1'}}
  var mutable: String = "alice" // expected-note{{access to property 'mutable' is only permitted within distributed actor 'DistributedActor_1'}}
  var computedMutable: String {
    get {
      "hey"
    }
    set {
      _ = newValue
    }
  }

  distributed let letProperty: String = "" // expected-error{{property 'letProperty' cannot be 'distributed', only computed properties can}}
  distributed var varProperty: String = "" // expected-error{{property 'varProperty' cannot be 'distributed', only computed properties can}}

  distributed var computed: String {
    "computed"
  }

  distributed var computedNotCodable: NotCodableValue { // expected-error{{result type 'NotCodableValue' of distributed property 'computedNotCodable' does not conform to serialization requirement 'Codable'}}
    .init()
  }

  distributed var getSet: String { // expected-error{{'distributed' computed property 'getSet' cannot have setter}}
    get {
      "computed"
    }
    set {
      _ = newValue
    }
  }

  distributed static func distributedStatic() {} // expected-error{{'distributed' method cannot be 'static'}}
  distributed class func distributedClass() {}
  // expected-error@-1{{class methods are only allowed within classes; use 'static' to declare a static method}}
  // expected-error@-2{{'distributed' method cannot be 'static'}}

  func hello() {} // expected-note{{distributed actor-isolated instance method 'hello()' declared here}}
  func helloAsync() async {} // expected-note{{distributed actor-isolated instance method 'helloAsync()' declared here}}
  func helloAsyncThrows() async throws {} // expected-note{{distributed actor-isolated instance method 'helloAsyncThrows()' declared here}}

  distributed func distHello() { } // ok
  distributed func distHelloAsync() async { } // ok
  distributed func distHelloThrows() throws { } // ok
  distributed func distHelloAsyncThrows() async throws { } // ok

  distributed func distInt() async throws -> Int { 42 } // ok
  distributed func distInt(int: Int) async throws -> Int { int } // ok
  distributed func distIntString(int: Int, two: String) async throws -> (String) { "\(int) + \(two)" } // ok

  distributed func dist(notCodable: NotCodableValue) async throws {
    // expected-error@-1 {{parameter 'notCodable' of type 'NotCodableValue' in distributed instance method does not conform to serialization requirement 'Codable'}}
  }
  distributed func distBadReturn(int: Int) async throws -> NotCodableValue {
    // expected-error@-1 {{result type 'NotCodableValue' of distributed instance method 'distBadReturn' does not conform to serialization requirement 'Codable'}}
    fatalError()
  }

  distributed func varargs(int: Int...) {
    // expected-error@-1{{cannot declare variadic argument 'int' in distributed instance method 'varargs(int:)'}}
  }

  distributed func closure(close: () -> String) {
    // expected-error@-1{{parameter 'close' of type '() -> String' in distributed instance method does not conform to serialization requirement 'Codable'}}
  }

  distributed func noInout(inNOut burger: inout String) {
    // expected-error@-1{{cannot declare 'inout' argument 'burger' in distributed instance method 'noInout(inNOut:)'}}{{43-49=}}
  }

  distributed func distReturnGeneric<T: Codable & Sendable>(item: T) async throws -> T { // ok
    item
  }
  distributed func distReturnGenericWhere<T: Sendable>(item: Int) async throws -> T where T: Codable { // ok
    fatalError()
  }
  distributed func distBadReturnGeneric<T: Sendable>(int: Int) async throws -> T {
    // expected-error@-1 {{result type 'T' of distributed instance method 'distBadReturnGeneric' does not conform to serialization requirement 'Codable'}}
    fatalError()
  }

  distributed func distGenericParam<T: Codable & Sendable>(value: T) async throws { // ok
    fatalError()
  }
  distributed func distGenericParamWhere<T: Sendable>(value: T) async throws -> T where T: Codable { // ok
    value
  }
  distributed func distBadGenericParam<T: Sendable>(int: T) async throws {
    // expected-error@-1 {{parameter 'int' of type 'T' in distributed instance method does not conform to serialization requirement 'Codable'}}
    fatalError()
  }

  static func staticFunc() -> String { "" } // ok

  @MainActor
  static func staticMainActorFunc() -> String { "" } // ok

  static distributed func staticDistributedFunc() -> String {
    // expected-error@-1{{'distributed' method cannot be 'static'}}{10-21=}
    fatalError()
  }

  func test_inside() async throws {
    _ = self.name
    _ = self.computed
    _ = self.computedMutable

    _ = try await self.distInt()
    _ = try await self.distInt(int: 42)

    self.hello()
    _ = await self.helloAsync()
    _ = try await self.helloAsyncThrows()

    self.distHello()
    await self.distHelloAsync()
    try self.distHelloThrows()
    try await self.distHelloAsyncThrows()

    // Hops over to the global actor.
    _ = await DistributedActor_1.staticMainActorFunc()
  }
}

func test_outside(
  local: LocalActor_1,
  distributed: DistributedActor_1
) async throws {
  // ==== properties
  _ = distributed.id // ok
  distributed.id = ActorAddress(parse: "mock://1.1.1.1:8080/#123121") // expected-error{{cannot assign to property: 'id' is immutable}}

  _ = local.name // ok, special case that let constants are okey
  let _: String = local.mutable // ok, special case that let constants are okey
  _ = distributed.name // expected-error{{distributed actor-isolated property 'name' can not be accessed from a non-isolated context}}
  _ = distributed.mutable // expected-error{{distributed actor-isolated property 'mutable' can not be accessed from a non-isolated context}}

  // ==== special properties (nonisolated, implicitly replicated)
  // the distributed actor's special fields may always be referred to
  _ = distributed.id
  _ = distributed.actorSystem

  // ==== static functions
  _ = distributed.staticFunc() // expected-error{{static member 'staticFunc' cannot be used on instance of type 'DistributedActor_1'}}
  _ = DistributedActor_1.staticFunc()

  // ==== non-distributed functions
  distributed.hello() // expected-error{{only 'distributed' instance methods can be called on a potentially remote distributed actor}}
  _ = await distributed.helloAsync() // expected-error{{only 'distributed' instance methods can be called on a potentially remote distributed actor}}
  _ = try await distributed.helloAsyncThrows() // expected-error{{only 'distributed' instance methods can be called on a potentially remote distributed actor}}
}

// ==== Protocols and static (non isolated functions)

protocol P {
  static func hello() -> String
}
extension P {
  static func hello() -> String { "" }
}

distributed actor ALL: P {
}

// ==== Codable parameters and return types ------------------------------------

func test_params(
  distributed: DistributedActor_1
) async throws {
  _ = try await distributed.distInt() // ok
  _ = try await distributed.distInt(int: 42) // ok
  _ = try await distributed.dist(notCodable: .init())
}

// Actor initializer isolation (through typechecking only!)
distributed actor DijonMustard {
  nonisolated init(system: FakeActorSystem) {} // expected-warning {{'nonisolated' on an actor's synchronous initializer is invalid; this is an error in Swift 6}} {{3-15=}}

  convenience init(conv: FakeActorSystem) { // expected-warning {{initializers in actors are not marked with 'convenience'; this is an error in Swift 6}}{{3-15=}}
    self.init(system: conv)
    self.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous nonisolated context}}
  }

  func f() {} // expected-note {{calls to instance method 'f()' from outside of its actor context are implicitly asynchronous}}

  nonisolated init(conv2: FakeActorSystem) { // expected-warning {{'nonisolated' on an actor's synchronous initializer is invalid; this is an error in Swift 6}} {{3-15=}}
    self.init(system: conv2)
  }
}

// ==== Larger example with protocols and extensions ---------------------------


protocol Greeting: DistributedActor {
  distributed func greeting() -> String
  distributed func greetingAsyncThrows() async throws -> String
}

extension Greeting {
  func greetLocal(name: String) async throws { // expected-note{{distributed actor-isolated instance method 'greetLocal(name:)' declared here}}
    try await print("\(greetingAsyncThrows()), \(name)!") // requirement is async throws, things work
  }

  func greetLocal2(name: String) {
    print("\(greeting()), \(name)!")
  }
}

extension Greeting where SerializationRequirement == Codable {
  // okay, uses Codable to transfer arguments.
  distributed func greetDistributed(name: String) async throws {
  // okay, we're on the actor
  try await greetLocal(name: name)
}

  distributed func greetDistributed2(name: String) async throws {
  // okay, we're on the actor
  greetLocal2(name: name)
}

  func greetDistributedNon(name: String) async throws {
    // okay, we're on the actor
    greetLocal2(name: name)
  }
}

extension Greeting where SerializationRequirement == Codable {
  nonisolated func greetAliceALot() async throws {
    try await greetLocal(name: "Alice") // expected-error{{only 'distributed' instance methods can be called on a potentially remote distributed actor}}
  }
}

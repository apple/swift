// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -swift-version 6 -emit-module -emit-module-path %t/other_global_actor_inference.swiftmodule -module-name other_global_actor_inference -strict-concurrency=complete %S/Inputs/other_global_actor_inference.swift

// RUN: %target-swift-frontend -swift-version 6 -I %t -disable-availability-checking %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency

import other_global_actor_inference

actor SomeActor { }

@globalActor
struct SomeGlobalActor {
  static let shared = SomeActor()
}

@globalActor
struct OtherGlobalActor {
  static let shared = SomeActor()
}


// MARK: Property Wrappers

@propertyWrapper
actor WrapperActor<Wrapped: Sendable> {
  var storage: Wrapped

  init(wrappedValue: Wrapped) {
    storage = wrappedValue
  }

  nonisolated var wrappedValue: Wrapped {
    get { }
    set { }
  }

  nonisolated var projectedValue: Wrapped {
    get { }
    set { }
  }
}

@propertyWrapper
@OtherGlobalActor
struct WrapperOnActor<Wrapped: Sendable> {
  private var stored: Wrapped

  nonisolated init(wrappedValue: Wrapped) {
    stored = wrappedValue
  }

  @MainActor var wrappedValue: Wrapped { // expected-note {{property declared here}}
    get { }
    set { }
  }

  @SomeGlobalActor var projectedValue: Wrapped {
    get {  }
    set { }
  }
}

@MainActor
@propertyWrapper
public struct WrapperOnMainActor<Wrapped> {
  // Make sure inference of @MainActor on wrappedValue doesn't crash.

  // expected-note@+1 {{mutation of this property is only permitted within the actor}}
  public var wrappedValue: Wrapped // expected-note {{property declared here}}

  public var accessCount: Int

  nonisolated public init(wrappedValue: Wrapped) {
    // expected-error@+1 {{main actor-isolated property 'wrappedValue' can not be mutated from a non-isolated context}}
    self.wrappedValue = wrappedValue
  }
}

struct HasMainActorWrappedProp {
  @WrapperOnMainActor var thing: Int = 1 // expected-note {{property declared here}}

  var plainStorage: Int

  var computedProp: Int { 0 }

  nonisolated func testErrors() {
    _ = thing // expected-error {{main actor-isolated property 'thing' can not be referenced from a non-isolated context}}
    _ = _thing.wrappedValue // expected-error {{main actor-isolated property 'wrappedValue' can not be referenced from a non-isolated context}}

    _ = _thing
    _ = _thing.accessCount

    _ = plainStorage

    _ = computedProp
  }
}

struct HasWrapperOnActor {
  @WrapperOnActor var synced: Int = 0
  // expected-note@-1 2{{property declared here}}

  // expected-note@+1 3{{to make instance method 'testErrors()'}}
  func testErrors() {
    _ = synced // expected-error{{main actor-isolated property 'synced' can not be referenced from a non-isolated context}}
    _ = $synced // expected-error{{global actor 'SomeGlobalActor'-isolated property '$synced' can not be referenced from a non-isolated context}}
    _ = _synced
    _ = _synced.wrappedValue // expected-error{{main actor-isolated property 'wrappedValue' can not be referenced from a non-isolated context}}
  }

  @MainActor mutating func testOnMain() {
    _ = synced
    synced = 17
  }

  @WrapperActor var actorSynced: Int = 0 // expected-error{{'nonisolated' is not supported on properties with property wrappers}}

  func testActorSynced() {
    _ = actorSynced
    _ = $actorSynced
    _ = _actorSynced
  }
}

struct Carbon {
  @IntWrapper var atomicWeight: Int // expected-note {{property declared here}}

  nonisolated func getWeight() -> Int {
    return atomicWeight // expected-error {{main actor-isolated property 'atomicWeight' can not be referenced from a non-isolated context}}
  }
}

@MainActor
protocol InferMainActor {}

@propertyWrapper
@preconcurrency @MainActor
struct Wrapper<T> {
  var wrappedValue: T {
    fatalError()
  }

  init() {}
}

@MainActor
class C {
  nonisolated init() {}
}

struct S: InferMainActor {
  @Wrapper var value: C // okay, 'S' is isolated to 'MainActor'
}

protocol InferMainActorInherited: InferMainActor {
  func f() // expected-note{{mark the protocol requirement 'f()' 'async' to allow actor-isolated conformances}}
  func g()
}

@SomeGlobalActor
protocol InferSomeGlobalActor { }

protocol InferenceConflict: InferMainActorInherited, InferSomeGlobalActor { }

struct S2: InferMainActorInherited {
  func f() { } // okay, 'f' is MainActor isolated, as is the requirement
  @MainActor func g() { } // okay for the same reasons, but more explicitly
}

@SomeGlobalActor
struct S3: InferenceConflict {
  nonisolated func g() { }
}

extension S3 {
  func f() { }
  // expected-error@-1{{global actor 'SomeGlobalActor'-isolated instance method 'f()' cannot be used to satisfy main actor-isolated protocol requirement}}
  //expected-note@-2{{add 'nonisolated' to 'f()' to make this instance method not isolated to the actor}}
}

@MainActor
func onMain() {}

@MainActor
class MainActorSuperclass {}

protocol InferMainFromSuperclass: MainActorSuperclass {
  func f()
}

class C1: MainActorSuperclass, InferMainFromSuperclass {
  func f() {
    onMain() // okay
  }
}

protocol InferenceConflictWithSuperclass: MainActorSuperclass, InferSomeGlobalActor {
  func g()
  // expected-note@-1 {{mark the protocol requirement 'g()' 'async' to allow actor-isolated conformances}}
}


class C2: MainActorSuperclass, InferenceConflictWithSuperclass {
//expected-note@-1 {{add '@preconcurrency' to the 'InferenceConflictWithSuperclass' conformance to defer isolation checking to run time}}

  func f() {}

  func g() {}
  // expected-error@-1 {{main actor-isolated instance method 'g()' cannot be used to satisfy nonisolated protocol requirement}}
  // expected-note@-2 {{add 'nonisolated' to 'g()' to make this instance method not isolated to the actor}}
}



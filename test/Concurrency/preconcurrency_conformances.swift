// RUN: %target-swift-frontend -swift-version 5 -disable-availability-checking %s -emit-sil -o /dev/null -verify -enable-upcoming-feature DynamicActorIsolation -strict-concurrency=complete -verify-additional-prefix complete-tns-

// REQUIRES: concurrency
// REQUIRES: asserts

protocol Q {
}

do {
  class K {}

  struct A : @preconcurrency Q {} // Ok
  // expected-warning@-1 {{@preconcurrency attribute on conformance to 'Q' has no effect}}

  struct B : @preconcurrency K {
    // expected-error@-1 {{'preconcurrency' attribute cannot apply to non-protocol type 'K'}}
    var x: @preconcurrency Int
    // expected-error@-1 {{'preconcurrency' attribute only applies in inheritance clauses}}
  }

  typealias T = @preconcurrency Q
  // expected-error@-1 {{'preconcurrency' attribute only applies in inheritance clauses}}

  func test(_: @preconcurrency K) {}
  // expected-error@-1 {{'preconcurrency' attribute only applies in inheritance clauses}}
}

protocol InvalidUseOfPreconcurrencyAttr : @preconcurrency Q {
  // expected-error@-1 {{'preconcurrency' attribute only applies in inheritance clauses}}
}

struct TestPreconcurrencyAttr {}
extension TestPreconcurrencyAttr : @preconcurrency Q { // Ok
  // expected-warning@-1 {{@preconcurrency attribute on conformance to 'Q' has no effect}}
}

class NonSendable {}

protocol TestSendability {
  var x: NonSendable { get }
  func test(_: NonSendable?) -> [NonSendable]
}

// Make sure that preconcurrency conformances suppress Sendable diagnostics,
// because @preconcurrency assumes that the witness will always be called
// from within the same isolation domain (with a dynamic check).
@MainActor
struct Value : @preconcurrency TestSendability {
  var x: NonSendable { NonSendable() }
  // expected-note@-1 2 {{property declared here}}

  func test(_: NonSendable?) -> [NonSendable] {
    // expected-note@-1 2 {{calls to instance method 'test' from outside of its actor context are implicitly asynchronous}}
    []
  }
}

// Make sure we don't spuriously say the @preconcurrency is unnecessary.
@MainActor
struct OtherValue : @preconcurrency TestSendability {
  var x: NonSendable { NonSendable() }

  nonisolated func test(_: NonSendable?) -> [NonSendable] {
    []
  }
}

// Make sure that references to actor isolated witness is diagnosed

// expected-note@+1 2 {{add '@MainActor' to make global function 'test(value:)' part of global actor 'MainActor'}}
func test(value: Value) {
  _ = value.x
  // expected-error@-1 {{main actor-isolated property 'x' can not be referenced from a non-isolated context}}
  _ = value.test(nil)
  // expected-error@-1 {{call to main actor-isolated instance method 'test' in a synchronous nonisolated context}}
}

actor MyActor {
  var value: Value? = nil
}

extension MyActor : @preconcurrency TestSendability {
  var x: NonSendable { NonSendable() }

  func test(_: NonSendable?) -> [NonSendable] {
    []
  }

  func test_ref_diagnostics() {
    _ = value?.x
    // expected-error@-1 {{main actor-isolated property 'x' can not be referenced on a non-isolated actor instance}}
    _ = value?.test(nil)
    // expected-error@-1 {{call to main actor-isolated instance method 'test' in a synchronous actor-isolated context}}
  }
}

protocol Initializable {
  init()
  // expected-note@-1{{mark the protocol requirement 'init()' 'async' to allow actor-isolated conformances}}
}

final class K : @preconcurrency Initializable {
  // expected-warning@-1 {{@preconcurrency attribute on conformance to 'Initializable' has no effect}}
  init() {} // Ok
}

@MainActor
final class MainActorK: Initializable {
  // expected-note@-1{{add '@preconcurrency' to the 'Initializable' conformance to defer isolation checking to run time}}{{25-25=@preconcurrency }}
  init() { } // expected-warning{{main actor-isolated initializer 'init()' cannot be used to satisfy nonisolated protocol requirement}}
  // expected-note@-1{{add 'nonisolated' to 'init()' to make this initializer not isolated to the actor}}
}

protocol WithAssoc {
  associatedtype T
  func test() -> T
}

struct TestConditional<T> {}

extension TestConditional : @preconcurrency WithAssoc where T == Int {
  @MainActor func test() -> T { 42 } // Ok
}

@globalActor
struct GlobalActor {
  static let shared: MyActor = MyActor()
}

protocol WithIndividuallyIsolatedRequirements {
  @MainActor var a: Int { get set }
  @GlobalActor var b: Int { get set }
  // expected-note@-1 {{'b' declared here}}

  @GlobalActor func test()
  // expected-note@-1 {{mark the protocol requirement 'test()' 'async' to allow actor-isolated conformances}}
}

do {
  @MainActor
  struct TestExplicitGlobalActorAttrs : @preconcurrency WithIndividuallyIsolatedRequirements {
    // expected-warning@-1 {{@preconcurrency attribute on conformance to 'WithIndividuallyIsolatedRequirements' has no effect}}

    var a: Int = 42

    @MainActor var b: Int {
      // expected-warning@-1 {{main actor-isolated property 'b' cannot be used to satisfy global actor 'GlobalActor'-isolated protocol requirement}}
      get { 0 }
      set {}
    }

    @MainActor func test() {
      // expected-warning@-1 {{main actor-isolated instance method 'test()' cannot be used to satisfy global actor 'GlobalActor'-isolated protocol requirement}}
    }
  }
}

@MainActor
protocol WithNonIsolated {
  var prop: Int { get set }
  // expected-note@-1 {{'prop' declared here}}
  nonisolated func test()
  // expected-note@-1 {{mark the protocol requirement 'test()' 'async' to allow actor-isolated conformances}}
}

do {
  class TestExplicitOtherIsolation : @preconcurrency WithNonIsolated {
    // expected-warning@-1 {{@preconcurrency attribute on conformance to 'WithNonIsolated' has no effect}}{{38-54=}}

    @GlobalActor var prop: Int = 42
    // expected-warning@-1 {{global actor 'GlobalActor'-isolated property 'prop' cannot be used to satisfy main actor-isolated protocol requirement}}

    @MainActor func test() {}
    // expected-warning@-1 {{main actor-isolated instance method 'test()' cannot be used to satisfy nonisolated protocol requirement}}
  }
}

do {
  class InferredGlobalActorAttrs : @preconcurrency WithNonIsolated {
    // expected-warning@-1 {{@preconcurrency attribute on conformance to 'WithNonIsolated' has no effect}}{{36-52=}}
    var prop: Int = 42
    func test() {}
  }
}

// https://github.com/apple/swift/issues/74294
protocol Parent {
  func a()
}

protocol Child: Parent {
  func b()
}

do {
    actor Test: @preconcurrency Child {
      func a() {} // Ok
      func b() {} // Ok
    }
}

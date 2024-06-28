// RUN: %target-swift-frontend -disable-availability-checking -swift-version 6 %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency

@preconcurrency func unsafelySendableClosure(_ closure: @Sendable () -> Void) { }

@preconcurrency func unsafelyMainActorClosure(_ closure: @MainActor () -> Void) { }

@preconcurrency func unsafelyDoEverythingClosure(_ closure: @MainActor @Sendable () -> Void) { }

struct X {
  @preconcurrency func unsafelyDoEverythingClosure(_ closure: @MainActor @Sendable () -> Void) { }

  @preconcurrency var sendableVar: @Sendable () -> Void
  @preconcurrency var mainActorVar: @MainActor () -> Void

  @preconcurrency
  subscript(_: @MainActor () -> Void) -> (@Sendable () -> Void) { {} }

  @preconcurrency
  static subscript(statically _: @MainActor () -> Void) -> (@Sendable () -> Void) { { } }
}

@MainActor func onMainActor() { }

func testInAsync(x: X) async {
  let _: Int = unsafelySendableClosure // expected-error{{type '@Sendable (@Sendable () -> Void) -> ()'}}
  let _: Int = unsafelyMainActorClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = unsafelyDoEverythingClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = x.unsafelyDoEverythingClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = X.unsafelyDoEverythingClosure // expected-error{{type '@Sendable (X) -> @Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = (X.unsafelyDoEverythingClosure)(x) // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}

  let _: Int = x.sendableVar // expected-error{{type '@Sendable () -> Void'}}
  let _: Int = x.mainActorVar // expected-error{{type '@MainActor @Sendable () -> Void'}}

  let _: Int = x[{ onMainActor() }] // expected-error{{type '@Sendable () -> Void'}}
  let _: Int = X[statically: { onMainActor() }] // expected-error{{type '@Sendable () -> Void'}}
}

func testElsewhere(x: X) {
  let _: Int = unsafelySendableClosure // expected-error{{type '@Sendable (@Sendable () -> Void) -> ()'}}
  let _: Int = unsafelyMainActorClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = unsafelyDoEverythingClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = x.unsafelyDoEverythingClosure // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = X.unsafelyDoEverythingClosure // expected-error{{type '@Sendable (X) -> @Sendable (@MainActor @Sendable () -> Void) -> ()'}}
  let _: Int = (X.unsafelyDoEverythingClosure)(x) // expected-error{{type '@Sendable (@MainActor @Sendable () -> Void) -> ()'}}

  let _: Int = x.sendableVar // expected-error{{type '@Sendable () -> Void'}}
  let _: Int = x.mainActorVar // expected-error{{type '@MainActor @Sendable () -> Void'}}

  let _: Int = x[{ onMainActor() }] // expected-error{{type '@Sendable () -> Void'}}
  let _: Int = X[statically: { onMainActor() }] // expected-error{{type '@Sendable () -> Void'}}
}

@MainActor @preconcurrency func onMainActorAlways() { }
// expected-note@-1{{are implicitly asynchronous}}

@preconcurrency @MainActor class MyModelClass {
 func f() { }
  // expected-note@-1{{are implicitly asynchronous}}
}

func testCalls(x: X) {
  // expected-note@-1 2{{add '@MainActor' to make global function 'testCalls(x:)' part of global actor 'MainActor'}}
  onMainActorAlways() // expected-error{{call to main actor-isolated global function 'onMainActorAlways()' in a synchronous nonisolated context}}

  let _: () -> Void = onMainActorAlways // expected-error{{converting function value of type '@MainActor @Sendable () -> ()' to '() -> Void' loses global actor 'MainActor'}}

  let c = MyModelClass() // okay, synthesized init() is 'nonisolated'

  c.f() // expected-error{{call to main actor-isolated instance method 'f()' in a synchronous nonisolated context}}
}

func testCallsWithAsync() async {
  onMainActorAlways() // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{calls to global function 'onMainActorAlways()' from outside of its actor context are implicitly asynchronous}}

  let _: () -> Void = onMainActorAlways // expected-error{{converting function value of type '@MainActor @Sendable () -> ()' to '() -> Void' loses global actor 'MainActor'}}

  let c = MyModelClass() // okay, synthesized init() is 'nonisolated'

  c.f() // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{calls to instance method 'f()' from outside of its actor context are implicitly asynchronous}}
}

// ---------------------------------------------------------------------------
// Protocols that inherit Sendable and predate concurrency.
// ---------------------------------------------------------------------------
@preconcurrency protocol P: Sendable { }
protocol Q: P { }

class NS { } // expected-note 3{{class 'NS' does not conform to the 'Sendable' protocol}}

struct S1: P {
  var ns: NS // expected-error{{stored property 'ns' of 'Sendable'-conforming struct 'S1' has non-sendable type 'NS'}}
}

struct S2: Q {
  var ns: NS // expected-error{{stored property 'ns' of 'Sendable'-conforming struct 'S2' has non-sendable type 'NS'}}
}

struct S3: Q, Sendable {
  var ns: NS // expected-error{{stored property 'ns' of 'Sendable'-conforming struct 'S3' has non-sendable type 'NS'}}
}

// ---------------------------------------------------------------------------
// Historical attribute names do nothing (but are permitted)
// ---------------------------------------------------------------------------
func aFailedExperiment(@_unsafeSendable _ body: @escaping () -> Void) { }
// expected-warning@-1{{'_unsafeSendable' attribute has been removed in favor of @preconcurrency}}

func anothingFailedExperiment(@_unsafeMainActor _ body: @escaping () -> Void) { }
// expected-warning@-1{{'_unsafeMainActor' attribute has been removed in favor of @preconcurrency}}

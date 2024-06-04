// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify -verify-additional-prefix complete- -verify-additional-prefix typechecker-only- -DTYPECHECKER_ONLY %s -o /dev/null -disable-region-based-isolation-with-strict-concurrency -enable-upcoming-feature GlobalActorIsolatedTypesUsability
// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify -verify-additional-prefix tns-  %s -o /dev/null -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// This run validates that for specific test cases around closures, we properly
// emit errors in the type checker before we run sns. This ensures that we know that
// these cases can't happen when SNS is enabled.
//
// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify -verify-additional-prefix typechecker-only- -DTYPECHECKER_ONLY %s -o /dev/null -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

/// Classes are always non-sendable, so this is non-sendable
class NonSendableKlass { // expected-complete-note 53{{}}
  // expected-typechecker-only-note @-1 3{{}}
  // expected-tns-note @-2 {{}}
  var field: NonSendableKlass? = nil

  init() {}
  init(_ x: NonSendableKlass) {
  }

  func asyncCall() async {}
  func asyncCallWithIsolatedParameter(isolation: isolated (any Actor)? = #isolation) async {
  }
}

class SendableKlass : @unchecked Sendable {}

actor MyActor {
  var klass = NonSendableKlass()
  // expected-typechecker-only-note @-1 7{{property declared here}}
  final var finalKlass = NonSendableKlass()

  func useKlass(_ x: NonSendableKlass) {}

  func useSendableFunction(_: @Sendable () -> Void) {}
  func useNonSendableFunction(_: () -> Void) {}
  func doSomething() {}
}

final actor FinalActor {
  var klass = NonSendableKlass()
  func useKlass(_ x: NonSendableKlass) {}
}

@MainActor
class MainActorIsolatedKlass {
  var klass = NonSendableKlass()
  let klassLet = NonSendableKlass()
}

@MainActor
final class FinalMainActorIsolatedKlass {
  var klass = NonSendableKlass()
}

func useInOut<T>(_ x: inout T) {}
func useValue<T>(_ x: T) {}
func useValueAsync<T>(_ x: T) async {}

@MainActor func transferToMain<T>(_ t: T) async {}

var booleanFlag: Bool { false }

struct SingleFieldKlassBox { // expected-complete-note 2{{consider making struct 'SingleFieldKlassBox' conform to the 'Sendable' protocol}}
  var k = NonSendableKlass()
}

struct TwoFieldKlassBox { // expected-typechecker-only-note 2{{}}
  var k1 = NonSendableKlass()
  var k2 = NonSendableKlass()
}

class TwoFieldKlassClassBox {
  var k1 = NonSendableKlass()
  var k2 = NonSendableKlass()
  var recursive: TwoFieldKlassClassBox? = nil
}

////////////////////////////
// MARK: Actor Self Tests //
////////////////////////////

extension MyActor {
  func warningIfCallingGetter() async {
    await self.klass.asyncCall() // expected-complete-warning {{passing argument of non-sendable type 'NonSendableKlass' outside of actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'self.klass' to nonisolated instance method 'asyncCall()' risks causing data races between nonisolated and 'self'-isolated uses}}
  }

  func warningIfCallingAsyncOnFinalField() async {
    // Since we are calling finalKlass directly, we emit a warning here.
    await self.finalKlass.asyncCall() // expected-complete-warning {{passing argument of non-sendable type 'NonSendableKlass' outside of actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'self.finalKlass' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'self.finalKlass' to nonisolated instance method 'asyncCall()' risks causing data races between nonisolated and 'self'-isolated uses}}
  }

  // We do not warn on this since we warn in the caller of our getter instead.
  var klassGetter: NonSendableKlass {
    self.finalKlass
  }
}

extension FinalActor {
  func warningIfCallingAsyncOnFinalField() async {
    // Since our whole class is final, we emit the error directly here.
    await self.klass.asyncCall() // expected-complete-warning {{passing argument of non-sendable type 'NonSendableKlass' outside of actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'self.klass' to nonisolated instance method 'asyncCall()' risks causing data races between nonisolated and 'self'-isolated uses}}
  }
}

/////////////////////////////
// MARK: Closure Formation //
/////////////////////////////

// This test makes sure that we can properly pattern match project_box.
func formClosureWithoutCrashing() {
 var c = NonSendableKlass() // expected-warning {{variable 'c' was never mutated; consider changing to 'let' constant}}
  let _ = { print(c) }
}

// In this test, closure is not promoted into its box form. As a result, we
// treat assignments into contents as a merge operation rather than an assign.
func closureInOut(_ a: MyActor) async {
  var contents = NonSendableKlass()
  let ns0 = NonSendableKlass()
  let ns1 = NonSendableKlass()

  contents = ns0
  contents = ns1

  var closure = {}
  closure = { useInOut(&contents) }

  await a.useKlass(ns0)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass'}}
  // expected-tns-warning @-2 {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-3 {{sending 'ns0' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}

  if await booleanFlag {
    // This is not an actual use since we are passing values to the same
    // isolation domain.
    await a.useKlass(ns1)
    // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass'}}
  } else {
    closure() // expected-tns-note {{access can happen concurrently}}
  }
}

func closureInOutDifferentActor(_ a: MyActor, _ a2: MyActor) async {
  var contents = NonSendableKlass()
  let ns0 = NonSendableKlass()
  let ns1 = NonSendableKlass()

  contents = ns0
  contents = ns1

  var closure = {}
  closure = { useInOut(&contents) }

  await a.useKlass(ns0)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass'}}
  // expected-tns-warning @-2 {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-3 {{sending 'ns0' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}

  // We only emit a warning on the first use we see, so make sure we do both
  // the use and the closure.
  if await booleanFlag {
    await a2.useKlass(ns1) // expected-tns-note {{access can happen concurrently}}
    // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass'}}
  } else {
    closure() // expected-tns-note {{access can happen concurrently}}
  }
}


func closureInOut2(_ a: MyActor) async {
  var contents = NonSendableKlass()
  let ns0 = NonSendableKlass()
  let ns1 = NonSendableKlass()

  contents = ns0
  contents = ns1

  var closure = {}

  await a.useKlass(ns0) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass'}}

  closure = { useInOut(&contents) } // expected-tns-note {{access can happen concurrently}}

  await a.useKlass(ns1) // expected-tns-warning {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass'}}

  closure() // expected-tns-note {{access can happen concurrently}}
}

func closureNonInOut(_ a: MyActor) async {
  var contents = NonSendableKlass()
  let ns0 = NonSendableKlass()
  let ns1 = NonSendableKlass()

  contents = ns0
  contents = ns1

  var closure = {}

  await a.useKlass(ns0)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass'}}

  closure = { useValue(contents) }

  await a.useKlass(ns1) // expected-tns-warning {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass'}}

  closure() // expected-tns-note {{access can happen concurrently}}
}

// TODO: Rework the nonisolated closure so we only treat nonisolated closures
// that capture self as being nonisolated. When we capture from outside of a
// method, we cannot access the inside of the actor without using an await, so
// this is safe.
func transferNonIsolatedNonAsyncClosureTwice() async {
  let a = MyActor()

  // This is non-isolated and non-async... we can transfer it safely.
  var actorCaptureClosure = { print(a) }
  await transferToMain(actorCaptureClosure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  actorCaptureClosure = { print(a) }
  await transferToMain(actorCaptureClosure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

extension MyActor {
  // Simple just capture self and access field.
  func simpleClosureCaptureSelfAndTransfer() async {
    let closure: () -> () = {
      print(self.klass)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfAndTransferThroughTuple() async {
    let closure: () -> () = {
      print(self.klass)
    }
    let x = (1, closure)
    await transferToMain(x) // expected-complete-warning {{passing argument of non-sendable type '(Int, () -> ())' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'self'-isolated value of type '(Int, () -> ())' with later accesses to main actor-isolated context risks causing data races}}
  }

  func simpleClosureCaptureSelfAndTransferThroughTupleBackwards() async {
    let closure: () -> () = {
      print(self.klass)
    }

    let x = (closure, 1)
    await transferToMain(x) // expected-tns-warning {{sending 'self'-isolated value of type '(() -> (), Int)' with later accesses to main actor-isolated context risks causing data races}}
    // expected-complete-warning @-1 {{passing argument of non-sendable type '(() -> (), Int)' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }

  func simpleClosureCaptureSelfAndTransferThroughOptional() async {
    let closure: () -> () = {
      print(self.klass)
    }
    let x: Any? = (1, closure)
    await transferToMain(x) // expected-complete-warning {{passing argument of non-sendable type 'Any?' into main actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'x' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfAndTransferThroughOptionalBackwards() async {
    let closure: () -> () = {
      print(self.klass)
    }
    // In contrast to the tuple backwards case, we do error here since the value
    // we are forming is an Any?  so we actually form the tuple as an object and
    // store it all as once.
    let x: Any? = (closure, 1)
    await transferToMain(x) // expected-complete-warning {{passing argument of non-sendable type 'Any?' into main actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'x' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfWithReinit() async {
    var closure: () -> () = {
      print(self.klass)
    }

    // Error here.
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}

    closure = {}

    // But not here.
    await transferToMain(closure)
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }

  func simpleClosureCaptureSelfWithReinit2() async {
    var closure: () -> () = {}

    // No error here.
    await transferToMain(closure)
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

    closure = {
      print(self.klass)
    }

    // Error here.
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfWithReinit3() async {
    var closure: () -> () = {}

    // We get a transfer after use error.
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}

    if await booleanFlag {
      closure = {
        print(self.klass)
      }
    }

    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-note @-2 {{access can happen concurrently}}
    // expected-tns-warning @-3 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-4 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  // In this case, we reinit along both paths, but only one has an actor derived
  // thing.
  func simpleClosureCaptureSelfWithReinit4() async {
    var closure: () -> () = {}

    await transferToMain(closure)
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

    if await booleanFlag {
      closure = {
        print(self.klass)
      }
    } else {
      closure = {}
    }

    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  #if TYPECHECKER_ONLY

  func simpleClosureCaptureSelfThroughTupleWithFieldAccess() async {
    // In this case, we erase that we accessed self so we hit a type checker
    // error. We could make this a SNS error, but since in the other cases where
    // we have a non-isolated non-async we are probably going to change it to be
    // async as well making these errors go away.
    let x = (self, 1)
    let closure: () -> () = {
      print(x.0.klass) // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
    }
    await transferToMain(closure)
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }

  #endif

  func simpleClosureCaptureSelfThroughTuple() async {
    let x = (self, self.klass)
    let closure: () -> () = {
      print(x.1)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfThroughTuple2() async {
    let x = (2, self.klass)
    let closure: () -> () = {
      print(x.1)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func simpleClosureCaptureSelfThroughOptional() async {
    let x: Any? = (2, self.klass)
    let closure: () -> () = {
      print(x as Any)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }
}

func testSimpleLetClosureCaptureActor() async {
  let a = MyActor()
  let closure = { print(a) }
  await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

#if TYPECHECKER_ONLY

func testSimpleLetClosureCaptureActorField() async {
  let a = MyActor()
  let closure = { print(a.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

func testSimpleLetClosureCaptureActorFieldThroughTuple() async {
  let a = (MyActor(), 0)
  let closure = { print(a.0.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

func testSimpleLetClosureCaptureActorFieldThroughOptional() async {
  let a: MyActor? = MyActor()
  let closure = { print(a!.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

#endif

func testSimpleVarClosureCaptureActor() async {
  let a = MyActor()
  var closure = {}
  closure = { print(a) }
  await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

#if TYPECHECKER_ONLY

func testSimpleVarClosureCaptureActorField() async {
  let a = MyActor()
  var closure = {}
  closure = { print(a.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

func testSimpleVarClosureCaptureActorFieldThroughTuple() async {
  let a = (MyActor(), 0)
  var closure = {}
  closure = { print(a.0.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

func testSimpleVarClosureCaptureActorFieldThroughOptional() async {
  let a: MyActor? = MyActor()
  var closure = {}
  closure = { print(a!.klass) } // expected-typechecker-only-error {{actor-isolated property 'klass' can not be referenced from a non-isolated context}}
  await transferToMain(closure)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

#endif

extension MyActor {
  // Make sure that we properly propagate actor derived from klass into field's
  // value.
  func simplePropagateNonSendableThroughMultipleProperty() async {
    let f = self.klass.field!
    let closure: () -> () = {
      print(f)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  // Make sure that we properly propagate actor derived from klass into field's
  // value.
  func simplePropagateNonSendableThroughMultipleProperty2() async {
    let f = self.klass.field!
    let closure: () -> () = {
      print(f.field!)
    }
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }
}

extension MyActor {
  func testVarReassignStopActorDerived() async {
    var closure: () -> () = {
      print(self)
    }

    // This should error.
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'self'-isolated 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}

    // This doesnt since we re-assign
    closure = {}
    await transferToMain(closure)
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

    // This re-assignment shouldn't error.
    closure = {}
    // But this transfer should.
    await transferToMain(closure) // expected-complete-warning {{passing argument of non-sendable type '() -> ()' into main actor-isolated context may introduce data races}}
    // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    // expected-tns-warning @-2 {{sending 'closure' risks causing data races}}
    // expected-tns-note @-3 {{sending 'closure' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}

    // But this will error since we race.
    closure() // expected-tns-note {{access can happen concurrently}}
  }
}

///////////////////////////////////
// MARK: Sendable Function Tests //
///////////////////////////////////

// Make sure that we do not error on function values that are Sendable... even
// if the function is converted to a non-Sendable form by a function conversion.
func testConversionsAndSendable(a: MyActor, f: @Sendable () -> Void, f2: () -> Void) async {
  // No function conversion.
  await a.useSendableFunction(f)

  // Function conversion.
  await a.useNonSendableFunction(f)

  // Show that we error if we are not sendable.
  await a.useNonSendableFunction(f2) // expected-complete-warning {{passing argument of non-sendable type '() -> Void' into actor-isolated context may introduce data races}}
  // expected-complete-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  // expected-tns-warning @-2 {{sending 'f2' risks causing data races}}
  // expected-tns-note @-3 {{sending task-isolated 'f2' to actor-isolated instance method 'useNonSendableFunction' risks causing data races between actor-isolated and task-isolated uses}}
}

func testSendableClosureCapturesNonSendable(a: MyActor) {
  let klass = NonSendableKlass()
  let _ = { @Sendable in
    _ = klass // expected-warning {{capture of 'klass' with non-sendable type 'NonSendableKlass' in a `@Sendable` closure}}
  }
}

func testSendableClosureCapturesNonSendable2(a: FinalMainActorIsolatedKlass) {
  let klass = NonSendableKlass()
  let _ = { @Sendable @MainActor in
    a.klass = klass // expected-complete-warning {{capture of 'klass' with non-sendable type 'NonSendableKlass' in a `@Sendable` closure}}
  }
}

/////////////////////////////////////////////////////
// MARK: Multiple Field Var Assignment Merge Tests //
/////////////////////////////////////////////////////

func singleFieldVarMergeTest() async {
  var box = SingleFieldKlassBox()
  box = SingleFieldKlassBox()

  // This transfers the entire region.
  await transferToMain(box.k)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  // But since box has only a single element, this doesn't race.
  box.k = NonSendableKlass()
  useValue(box.k)
  useValue(box)

  // We transfer the box back to main.
  await transferToMain(box)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'SingleFieldKlassBox' into main actor-isolated context may introduce data races}}

  // And reassign over the entire box, so again we can use it again.
  box = SingleFieldKlassBox()

  useValue(box)
  useValue(box.k)

  await transferToMain(box) // expected-tns-warning {{sending 'box' risks causing data races}}
  // expected-tns-note @-1 {{sending 'box' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'SingleFieldKlassBox' into main actor-isolated context may introduce data races}}


  // But if we use box.k here, we emit an error since we didn't reinitialize at
  // all.
  useValue(box.k) // expected-tns-note {{access can happen concurrently}}
}

func multipleFieldVarMergeTest1() async {
  var box = TwoFieldKlassBox()
  box = TwoFieldKlassBox()

  // This transfers the entire region.
  await transferToMain(box.k1) // expected-tns-warning {{sending 'box.k1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'box.k1' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}


  // So even if we reassign over k1, since we did a merge, this should error.
  box.k1 = NonSendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(box)
}

func multipleFieldVarMergeTest2() async {
  var box = TwoFieldKlassBox()
  box = TwoFieldKlassBox()

  // This transfers the entire region.
  await transferToMain(box.k1)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  // But if we assign over box completely, we can use it again.
  box = TwoFieldKlassBox()

  useValue(box.k1)
  useValue(box.k2)
  useValue(box)

  await transferToMain(box.k2)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  // But if we assign over box completely, we can use it again.
  box = TwoFieldKlassBox()

  useValue(box.k1)
  useValue(box.k2)
  useValue(box)
}

func multipleFieldTupleMergeTest1() async {
  var box = (NonSendableKlass(), NonSendableKlass())
  box = (NonSendableKlass(), NonSendableKlass())

  // This transfers the entire region.
  await transferToMain(box.0) // expected-tns-warning {{sending 'box.0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'box.0' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  // So even if we reassign over k1, since we did a merge, this should error.
  box.0 = NonSendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(box)
}

// TODO: Tuples today do not work since I need to change how we assign into
// tuple addresses to use a single instruction. Today SILGen always uses
// multiple values. I am going to fix this in a subsequent commit.
func multipleFieldTupleMergeTest2() async {
  var box = (NonSendableKlass(), NonSendableKlass())

  // This transfers the entire region.
  await transferToMain(box.0)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  let box2 = (NonSendableKlass(), NonSendableKlass())
  // But if we assign over box completely, we can use it again.
  box = box2

  useValue(box.0)
  useValue(box.1)
  useValue(box)

  await transferToMain(box.1)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

  // But if we assign over box completely, we can use it again.
  box = (NonSendableKlass(), NonSendableKlass())

  useValue(box.0)
  useValue(box.1)
  useValue(box)
}

///////////////////////////
// MARK: ClassFieldTests //
///////////////////////////

class ClassFieldTests { // expected-complete-note 6{{}}
  let letSendableTrivial = 0
  let letSendableNonTrivial = SendableKlass()
  let letNonSendableNonTrivial = NonSendableKlass()
  var varSendableTrivial = 0
  var varSendableNonTrivial = SendableKlass()
  var varNonSendableNonTrivial = NonSendableKlass()
}

func letSendableTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letSendableNonTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}

  _ = test.letSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letNonSendableNonTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varSendableTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varSendableNonTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableNonTrivial  // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialClassFieldTest() async {
  let test = ClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'ClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

////////////////////////////////
// MARK: FinalClassFieldTests //
////////////////////////////////

final class FinalClassFieldTests { // expected-complete-note 6 {{}}
  let letSendableTrivial = 0
  let letSendableNonTrivial = SendableKlass()
  let letNonSendableNonTrivial = NonSendableKlass()
  var varSendableTrivial = 0
  var varSendableNonTrivial = SendableKlass()
  var varNonSendableNonTrivial = NonSendableKlass()
}

func letSendableTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letSendableNonTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letNonSendableNonTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varSendableTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varSendableNonTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableNonTrivial  // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialFinalClassFieldTest() async {
  let test = FinalClassFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'FinalClassFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

////////////////////////////
// MARK: StructFieldTests //
////////////////////////////

struct StructFieldTests { // expected-complete-note 31 {{}}
  let letSendableTrivial = 0
  let letSendableNonTrivial = SendableKlass()
  let letNonSendableNonTrivial = NonSendableKlass()
  var varSendableTrivial = 0
  var varSendableNonTrivial = SendableKlass()
  var varNonSendableNonTrivial = NonSendableKlass()
}

func letSendableTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letSendableNonTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letNonSendableNonTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.letNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func letSendableTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letSendableNonTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.letSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func letNonSendableNonTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.letNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

// Lets can access sendable let/var even if captured in a closure.
func letNonSendableNonTrivialLetStructFieldClosureTest() async {
  let test = StructFieldTests()
  let cls = {
    print(test)
  }
  _ = cls
  var cls2 = {}
  cls2 = {
    print(test)
  }
  _ = cls2
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.letSendableNonTrivial
  _ = z
  let z2 = test.varSendableNonTrivial
  _ = z2
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableNonTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialLetStructFieldTest() async {
  let test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.varNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func varSendableTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableNonTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  _ = test.varSendableNonTrivial
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialVarStructFieldTest() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.varNonSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

// vars cannot access sendable let/var if captured in a closure.
func varNonSendableNonTrivialLetStructFieldClosureTest1() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  let cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.letSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest2() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  let cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.varSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest3() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  let cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

// vars cannot access sendable let/var if captured in a closure.
func varNonSendableNonTrivialLetStructFieldClosureTest4() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.letSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest5() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  let z = test.varSendableNonTrivial // expected-tns-note {{access can happen concurrently}}
  _ = z
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest6() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    test = StructFieldTests()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest7() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    test.varSendableNonTrivial = SendableKlass()
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest8() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    useInOut(&test)
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureTest9() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}
  cls = {
    useInOut(&test.varSendableNonTrivial)
  }
  _ = cls
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive1() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  if await booleanFlag {
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls
  } else {
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
    // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}

    test.varSendableNonTrivial = SendableKlass()
  }

  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive2() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  if await booleanFlag {
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls

    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
    // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  }

  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)
}

// We do not error when accessing the sendable field in this example since the
// transfer is not reachable from the closure. Instead we emit an error on test.
func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive3() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  if await booleanFlag {
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls
  } else {
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
    // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  }

  test.varSendableNonTrivial = SendableKlass()
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive4() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  for _ in 0..<1024 {
    // TODO: The error here is b/c of the loop carry. We should probably store
    // the operand instead of just the require inst, so we can better separate a
    // loop carry use and an error due to multiple params. Then we could emit
    // the error on test below. This is still correct though, just not as
    // good... that is QoI though.
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
    // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}

    // This is treated as a use since test is in box form and is mutable. So we
    // treat assignment as a merge.
    test = StructFieldTests() // expected-tns-note {{access can happen concurrently}}
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls
  }

  test.varSendableNonTrivial = SendableKlass()
  useValue(test)
}

func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive5() async {
  var test = StructFieldTests()
  test = StructFieldTests()

  // The reason why we error here is that even though we reassign at the end of
  // the for loop and currently understand that test has a new value different
  // from the value assigned to test at the beginning of the for loop, when we
  // merge back through the for loop, we have to merge the regions due to the
  // union operation we perform. So the conservatism of the dataflow creates
  // this. This is a case where we are going to need to be able to have the
  // compiler explain the regions well.
  for _ in 0..<1024 {
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-tns-note @-2 {{access can happen concurrently}}
    // expected-complete-warning @-3 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
    test = StructFieldTests()
  }

  test.varSendableNonTrivial = SendableKlass()
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive6() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  // In this test case, the first transfer errors on the
  // test.varSendableNonTrivial b/c of the cls. Since we don't have cls on the
  // else path, it emits on useValue(test).
  if await booleanFlag {
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  } else {
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  }

  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test)  // expected-tns-note {{access can happen concurrently}}
}

// In this case since we are tracking the transfer from the else statement, we
// track the closure.
func varNonSendableNonTrivialLetStructFieldClosureFlowSensitive7() async {
  var test = StructFieldTests()
  test = StructFieldTests()
  var cls = {}

  if await booleanFlag {
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  } else {
    cls = {
      useInOut(&test.varSendableNonTrivial)
    }
    _ = cls
    await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructFieldTests' into main actor-isolated context may introduce data races}}
  }

  test.varSendableNonTrivial = SendableKlass() // expected-tns-note {{access can happen concurrently}}
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

////////////////////////////
// MARK: TupleFieldTests //
////////////////////////////

func varSendableTrivialLetTupleFieldTest() async {
  let test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  let z = test.0
  useValue(z)
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableNonTrivialLetTupleFieldTest() async {
  let test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  let z = test.1 // expected-tns-note {{access can happen concurrently}}
  useValue(z)
  useValue(test)
}

func varNonSendableNonTrivialLetTupleFieldTest() async {
  let test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  let z = test.2 // expected-tns-note {{access can happen concurrently}}
  useValue(z)
  useValue(test)
}

func varSendableTrivialVarTupleFieldTest() async {
  var test = (0, SendableKlass(), NonSendableKlass())
  test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  _ = test.0
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableTrivialVarTupleFieldTest2() async {
  var test = (0, SendableKlass(), NonSendableKlass())
  test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test.2) // expected-tns-warning {{sending 'test.2' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test.2' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  _ = test.0
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varSendableNonTrivialVarTupleFieldTest() async {
  var test = (0, SendableKlass(), NonSendableKlass())
  test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  _ = test.1
  useValue(test) // expected-tns-note {{access can happen concurrently}}
}

func varNonSendableNonTrivialVarTupleFieldTest() async {
  var test = (0, SendableKlass(), NonSendableKlass())
  test = (0, SendableKlass(), NonSendableKlass())
  await transferToMain(test) // expected-tns-warning {{sending 'test' risks causing data races}}
  // expected-tns-note @-1 {{sending 'test' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(Int, SendableKlass, NonSendableKlass)' into main actor-isolated context may introduce data races}}
  let z = test.2 // expected-tns-note {{access can happen concurrently}}
  useValue(z)
  useValue(test)
}

//////////////////////////////
// MARK: Control Flow Tests //
//////////////////////////////

func controlFlowTest1() async {
  let x = NonSendableKlass()

  if await booleanFlag {
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  } else {
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  useValue(x) // expected-tns-note 2{{access can happen concurrently}}
}

// This test seems like something that we should not error upon. What is
// happening here is that when we get to the bottom of the for loop, we realize
// that x's old value and x/x's new value are in different regions. But when we
// merge back into the loop header, we note that the loop entry block has x/x's
// old value in the same region so when we merge, we get that x/x's old
// value/x's new value are all in the same region. We emit the error on useValue
// as well afterwards since if we exit from the loop header, we have that large
// merge region leave the for loop.
func controlFlowTest2() async {
  var x = NonSendableKlass()

  for _ in 0..<1024 {
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
    // expected-tns-note @-2 {{access can happen concurrently}}
    // expected-complete-warning @-3 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}

    x = NonSendableKlass()
  }

  useValue(x) // expected-tns-note {{access can happen concurrently}}
}

////////////////////////
// MARK: Actor Setter //
////////////////////////

actor ActorWithSetter {
  var field = NonSendableKlass()
  var twoFieldBox = TwoFieldKlassBox()
  var twoFieldBoxInTuple = (NonSendableKlass(), TwoFieldKlassBox())
  var recursive: ActorWithSetter? = nil
  var classBox = TwoFieldKlassClassBox()

  func test1() async {
    let x = NonSendableKlass()
    self.field = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  func test2() async {
    let x = NonSendableKlass()
    self.twoFieldBox.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  func test3() async {
    let x = NonSendableKlass()
    self.twoFieldBoxInTuple.1.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  // This triggers a crash in SILGen with tns enabled.
  #if TYPECHECKER_ONLY
  func recursive() async {
    let x = NonSendableKlass()
    await self.recursive!.twoFieldBoxInTuple.1.k2 = x
    // expected-typechecker-only-warning @-1 {{non-sendable type '(NonSendableKlass, TwoFieldKlassBox)' in implicitly asynchronous access to actor-isolated property 'twoFieldBoxInTuple' cannot cross actor boundary}}
    // expected-typechecker-only-warning @-2 {{non-sendable type '(NonSendableKlass, TwoFieldKlassBox)' in implicitly asynchronous access to actor-isolated property 'twoFieldBoxInTuple' cannot cross actor boundary}}

    await transferToMain(x) // xpected-tns-warning {{call site passes `self` or a non-sendable argument of this function to another thread, potentially yielding a race with the caller}}
    // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }
  #endif

  func classBox() async {
    let x = NonSendableKlass()
    self.classBox.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }
}

final actor FinalActorWithSetter {
  var field = NonSendableKlass()
  var twoFieldBox = TwoFieldKlassBox()
  var twoFieldBoxInTuple = (NonSendableKlass(), TwoFieldKlassBox())
  var recursive: ActorWithSetter? = nil
  var classBox = TwoFieldKlassClassBox()

  func test1() async {
    let x = NonSendableKlass()
    self.field = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  func test2() async {
    let x = NonSendableKlass()
    self.twoFieldBox.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  func test3() async {
    let x = NonSendableKlass()
    self.twoFieldBoxInTuple.1.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }

  // This triggers a crash in SILGen with tns enabled.
  #if TYPECHECKER_ONLY
  func recursive() async {
    let x = NonSendableKlass()
    await self.recursive!.twoFieldBoxInTuple.1.k2 = x
    // expected-typechecker-only-warning @-1 {{non-sendable type '(NonSendableKlass, TwoFieldKlassBox)' in implicitly asynchronous access to actor-isolated property 'twoFieldBoxInTuple' cannot cross actor boundary}}
    // expected-typechecker-only-warning @-2 {{non-sendable type '(NonSendableKlass, TwoFieldKlassBox)' in implicitly asynchronous access to actor-isolated property 'twoFieldBoxInTuple' cannot cross actor boundary}}

    await transferToMain(x) // xpected-tns-warning {{call site passes `self` or a non-sendable argument of this function to another thread, potentially yielding a race with the caller}}
    // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }
  #endif

  func classBox() async {
    let x = NonSendableKlass()
    self.classBox.k1 = x
    await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  }
}

func functionArgumentIntoClosure(_ x: @escaping () -> ()) async {
  let _ = { @MainActor in
    let _ = x // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{task-isolated 'x' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    // expected-complete-warning @-2 {{capture of 'x' with non-sendable type '() -> ()' in a `@Sendable` closure}}
    // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }
}

// Make sure we handle the merge of the actor isolated and disconnected state
// appropriately.
//
// TODO: Since we are accessing the var field, we miss an
// error. rdar://126170563.
@MainActor func actorMergeInLoopVar() async {
  let a = MainActorIsolatedKlass()
  var c = NonSendableKlass()
  for _ in 0..<1024 {
    await useValueAsync(c) // expected-tns-warning {{sending 'c' risks causing data races}}
    // expected-tns-note @-1 {{sending main actor-isolated 'c' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' outside of main actor-isolated context may introduce data races}}
    c = a.klass
  }
}

@MainActor func actorMergeInLoopLet() async {
  let a = MainActorIsolatedKlass()
  var c = NonSendableKlass()
  for _ in 0..<1024 {
    await useValueAsync(c) // expected-tns-warning {{sending 'c' risks causing data races}}
    // expected-tns-note @-1 {{sending main actor-isolated 'c' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' outside of main actor-isolated context may introduce data races}}
    c = a.klassLet
  }
}

func testGetActorName() async {
  let a = MyActor()
  let x = NonSendableKlass()
  await a.useKlass(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending 'x' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  useValue(x) // expected-tns-note {{access can happen concurrently}}
}

extension MyActor {
  func testCallBangIsolatedMethod(other: MyActor) async {
    await klass.asyncCallWithIsolatedParameter(isolation: other) // expected-tns-warning {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'self.klass' to actor-isolated instance method 'asyncCallWithIsolatedParameter(isolation:)' risks causing data races between actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  }
}

extension FinalActor {
  func testCallBangIsolatedMethod(other: MyActor) async {
    await klass.asyncCallWithIsolatedParameter(isolation: other) // expected-tns-warning {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'self.klass' to actor-isolated instance method 'asyncCallWithIsolatedParameter(isolation:)' risks causing data races between actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  }
}

extension NonSendableKlass {
  func directAsyncCallWithIsolatedParameter(isolation: isolated (any Actor)? = #isolation) async {
  }
}

extension MyActor {
  func testCallBangIsolatedDirectMethod(other: MyActor) async {
    await klass.directAsyncCallWithIsolatedParameter(isolation: other) // expected-tns-warning {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'self.klass' to actor-isolated instance method 'directAsyncCallWithIsolatedParameter(isolation:)' risks causing data races between actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  }
}

extension FinalActor {
  func testCallBangIsolatedDirectMethod(other: MyActor) async {
    await klass.directAsyncCallWithIsolatedParameter(isolation: other) // expected-tns-warning {{sending 'self.klass' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'self.klass' to actor-isolated instance method 'directAsyncCallWithIsolatedParameter(isolation:)' risks causing data races between actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  }
}

actor DictionaryActorTest {
  var data: [Int: Int] = [:]

  // We used to crash on this due to isolation merging.
  func doSomething(_ key: Int) {
    assert(self.data[key] == 0)
  }
}

extension MyActor {
  // Make sure that we properly infer information from isolated parameters that
  // aren't self.
  func isolationInferenceFromNonSelfIsolatedParameters() {
    useValue {
      self.doSomething()
    }
  }

  func isolationInferenceFromNonSelfIsolatedParameters2() {
    useValue {
      self.doSomething()
      let x = NonSendableKlass()
      self.useKlass(x)
      await transferToMain(x)
      // expected-tns-warning @-1 {{sending 'x' risks causing data races}}
      // expected-tns-note @-2 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
      // expected-complete-warning @-3 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
    }
  }
}

func initAccessorTests() {
  @available(SwiftStdlib 5.1, *)
  actor NonisolatedAccessors {
    nonisolated var a: Int = 0 {
      init {
      }

      get { 0 }
      set {}
    }

    init(value: Int) {
      let escapingSelf: (NonisolatedAccessors) -> Void = { _ in }

      // a is initialized here via default value

      escapingSelf(self)

      self.a = value // Ok (nonisolated)
      print(a) // Ok (nonisolated)
    }
  }

  @available(SwiftStdlib 5.1, *)
  actor NonSendableInit {
    var first: NonSendableKlass
    var second: NonSendableKlass? = nil {
      @storageRestrictions(initializes: first)
      init(initialValue)  {
        first = initialValue!
      }

      get { fatalError() }
      set { fatalError() }
    }

    @MainActor
    var third: NonSendableKlass
    @MainActor
    var fourth: NonSendableKlass? = nil {
      @storageRestrictions(initializes: third)
      init(initialValue)  {
        third = initialValue!
      }

      get { fatalError() }
      set { fatalError() }
    }
  }
}

func differentInstanceTest(_ a: MyActor, _ b: MyActor) async {
  let x = NonSendableKlass()
  await a.useKlass(x)
  // expected-tns-warning @-1 {{sending 'x' risks causing data races}}
  // expected-tns-note @-2 {{sending 'x' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-3 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
  await b.useKlass(x) // expected-tns-note {{access can happen concurrently}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableKlass' into actor-isolated context may introduce data races}}
}

protocol AssociatedTypeTestProtocol {
  associatedtype A: Actor
}

func associatedTypeTestBasic<T: AssociatedTypeTestProtocol>(_: T, _: isolated T.A) {
}

func associatedTypeTestBasic2<T: AssociatedTypeTestProtocol>(_: T, iso: isolated T.A, x: NonSendableKlass) async {
  await transferToMain(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending 'iso'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'iso'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
}

func sendableGlobalActorIsolated() {
  let x = NonSendableKlass()
  let _ = { @Sendable @MainActor in
    print(x) // expected-tns-warning {{sending 'x' risks causing data races}}
    // expected-tns-note @-1 {{'x' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    // expected-complete-warning @-2 {{capture of 'x' with non-sendable type 'NonSendableKlass' in a `@Sendable` closure}}
  }
  print(x) // expected-tns-note {{access can happen concurrently}}
}

// We do not get an error here since we are transferring x both times to a main
// actor isolated thing function. We used to emit an error when using region
// isolation since we would trip on the store_borrow we used to materialize the
// value.
func testIndirectParameterSameIsolationNoError() async {
  let x = NonSendableKlass()
  await transferToMain(x) // expected-complete-warning {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
  await transferToMain(x) // expected-complete-warning {{passing argument of non-sendable type 'NonSendableKlass' into main actor-isolated context may introduce data races}}
}

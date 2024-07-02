// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify %s -o /dev/null -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

/// Classes are always non-sendable, so this is non-sendable
class NonSendableKlass {
  // expected-note @-1 {{}}
  var field: NonSendableKlass? = nil
  var field2: NonSendableKlass? = nil

  func asyncCall() async {}
}

class SendableKlass : @unchecked Sendable {}

actor Actor {
  var klass = NonSendableKlass()
  final var finalKlass = NonSendableKlass()

  func useKlass(_ x: NonSendableKlass) -> Int { fatalError("") }

  func useSendableFunction(_: @Sendable () -> Void) {}
  func useNonSendableFunction(_: () -> Void) {}
}

final actor FinalActor {
  var klass = NonSendableKlass()
  func useKlass(_ x: NonSendableKlass) {}
}

func useInOut<T>(_ x: inout T) {}
@discardableResult
func useValue<T>(_ x: T) -> T { x }
func useValueWrapInOptional<T>(_ x: T) -> T? { x }

@MainActor func returnValueFromMain<T>() async -> T { fatalError() }
@MainActor func transferToMain<T>(_ t: T) async {}
@MainActor func transferToMainInt<T>(_ t: T) async -> Int { 5 }
@MainActor func transferToMainIntOpt<T>(_ t: T) async -> Int? { 5 }

func transferToNonIsolated<T>(_ t: T) async {}
func transferToNonIsolatedInt<T>(_ t: T) async -> Int { 5 }
func transferToNonIsolatedIntOpt<T>(_ t: T) async -> Int? { 5 }

var booleanFlag: Bool { false }

struct SingleFieldKlassBox {
  var k = NonSendableKlass()
}

struct TwoFieldKlassBox {
  var k1 = NonSendableKlass()
  var k2 = NonSendableKlass()
}

/////////////////////////////////////
// MARK: Async Let Let Actor Tests //
/////////////////////////////////////

func asyncLet_Let_ActorIsolated_Simple1() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_Simple2() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}


  let _ = await y
  useValue(x) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_Simple3() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  // TODO: We shouldn't emit the 2nd error here given the current implementation
  // since it is only accessible along the else path but we already hit
  // useValue. But when we search for requires that we want to emit, we do not
  // take into account awaits. That being said, even though this is
  // inconsistent, a race does occur here.
  if await booleanFlag {
    let _ = await y
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
  useValue(x) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_Simple4() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  if await booleanFlag {
    useValue(x) // expected-note {{access can happen concurrently}}
    let _ = await y
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
}

func asyncLet_Let_ActorIsolated_Simple5() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  if await booleanFlag {
    let _ = await y
    useValue(x) // expected-note {{access can happen concurrently}}
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
}

// Make sure we error appropriately when accessing a field of a class in the
// async let rather than the base class.
func asyncLet_Let_ActorIsolated_AccessFieldsClass1() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsClass2() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsClass3() async {
  let x = NonSendableKlass()
  async let y = transferToMainInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.field2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure we error appropriately when accessing a field of a struct in the
// async let rather than the base struct.
func asyncLet_Let_ActorIsolated_AccessFieldsStruct1() async {
  let x = TwoFieldKlassBox()
  async let y = transferToMainInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsStruct2() async {
  let x = TwoFieldKlassBox()
  async let y = transferToMainInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.k1) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsStruct3() async {
  let x = TwoFieldKlassBox()
  async let y = transferToMainInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.k2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsStruct4() async {
  let x = TwoFieldKlassBox()
  async let y = transferToMainInt(x.k2.field2) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.k1.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure we error appropriately when accessing a field of a struct in the
// async let rather than the base struct.
func asyncLet_Let_ActorIsolated_AccessFieldsTuple1() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToMainInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsTuple2() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToMainInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.1) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsTuple3() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToMainInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.1.k2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_AccessFieldsTuple4() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToMainInt(x.1.k1.field2) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x.0.k2.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_ActorIsolated_CallBuriedInOtherExpr1() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x) + transferToMainInt(x2)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-2 {{sending 'x2' risks causing data races}}
  // expected-note @-3 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
  useValue(x2) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_CallBuriedInOtherExpr2() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x) + transferToMainInt(x2)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-2 {{sending 'x2' risks causing data races}}
  // expected-note @-3 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
  useValue(x) // expected-note {{access can happen concurrently}}
}

// Make sure we emit separate errors for x and x2.
func asyncLet_Let_ActorIsolated_CallBuriedInOtherExpr3() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x) + transferToMainInt(x2)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-2 {{sending 'x2' risks causing data races}}
  // expected-note @-3 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  // We only error on the first value captured if multiple values are captured
  // since we track a single partial_apply as a transfer instruction.
  useValue(x) // expected-note {{access can happen concurrently}}
  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure that we emit an error for transferToMainInt in the async val
// function itself.
func asyncLet_Let_ActorIsolated_CallBuriedInOtherExpr4() async {
  let x = NonSendableKlass()

  async let y = useValue(transferToMainInt(x) + transferToMainInt(x))

  let _ = await y
}

// Make sure that we do emit an error since we are sending the value to two
// different isolation domains in the async let.
func asyncLet_Let_ActorIsolated_CallBuriedInOtherExpr5() async {
  let x = NonSendableKlass()

  async let y = useValue(transferToMainInt(x) + transferToCustomInt(x))
  // expected-warning @-1 {{sending 'x' risks causing data races}}
  // expected-note @-2 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-note @-3:49 {{access can happen concurrently}}

  let _ = await y
}

// Make sure that we emit an error when the same value is used by two async let
// as part of one statement.
func asyncLet_Let_ActorIsolated_MultipleAsyncLet1() async {
  let x = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-note @-2:53 {{access can happen concurrently}}

  let _ = await y
  let _ = await z
}

// Make sure we don't error when we use different values in different async let.
func asyncLet_Let_ActorIsolated_MultipleAsyncLet2() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x2))

  let _ = await y
  let _ = await z
}

func asyncLet_Let_ActorIsolated_MultipleAsyncLet3() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x2))
  // expected-warning @-1 {{sending 'x2' risks causing data races}}
  // expected-note @-2 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
  let _ = await z
}

func asyncLet_Let_ActorIsolated_MultipleAsyncLet4() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x2))
  // expected-warning @-1 {{sending 'x' risks causing data races}}
  // expected-note @-2 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-3 {{sending 'x2' risks causing data races}}
  // expected-note @-4 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  let _ = await y
  let _ = await z
  useValue(x) // expected-note {{access can happen concurrently}}
  useValue(x2) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_MultipleAsyncLet5() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x2))
  // expected-warning @-1 {{sending 'x' risks causing data races}}
  // expected-note @-2 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-3 {{sending 'x2' risks causing data races}}
  // expected-note @-4 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  let _ = await y
  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await z
  useValue(x2) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_MultipleAsyncLet6() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToMainInt(x)), z = useValue(transferToMainInt(x2))
  // expected-warning @-1 {{sending 'x' risks causing data races}}
  // expected-note @-2 {{sending 'x' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}
  // expected-warning @-3 {{sending 'x2' risks causing data races}}
  // expected-note @-4 {{sending 'x2' to main actor-isolated global function 'transferToMainInt' risks causing data races between main actor-isolated and local nonisolated uses}}

  let _ = await y
  useValue(x) // expected-note {{access can happen concurrently}}
  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await z
}

///////////////////////////////////////
// MARK: SendNonSendable NonIsolated //
///////////////////////////////////////

func asyncLet_Let_NonIsolated_Simple1() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x)  // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_Simple2() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x)

  let _ = await y
  useValue(x)
}

func asyncLet_Let_NonIsolated_Simple3() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  // TODO: We shouldn't emit the 2nd error here given the current implementation
  // since it is only accessible along the else path but we already hit
  // useValue. But when we search for requires that we want to emit, we do not
  // take into account awaits. That being said, even though this is
  // inconsistent, a race does occur here.
  if await booleanFlag {
    let _ = await y
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
  useValue(x) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_NonIsolated_Simple4() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  if await booleanFlag {
    useValue(x) // expected-note {{access can happen concurrently}}
    let _ = await y
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
}

func asyncLet_Let_NonIsolated_Simple5() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  if await booleanFlag {
    let _ = await y
    useValue(x)
  } else {
    useValue(x) // expected-note {{access can happen concurrently}}
  }
}

// Make sure we error appropriately when accessing a field of a class in the
// async let rather than the base class.
func asyncLet_Let_NonIsolated_AccessFieldsClass1() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsClass2() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsClass3() async {
  let x = NonSendableKlass()
  async let y = transferToNonIsolatedInt(x.field) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.field2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure we error appropriately when accessing a field of a struct in the
// async let rather than the base struct.
func asyncLet_Let_NonIsolated_AccessFieldsStruct1() async {
  let x = TwoFieldKlassBox()
  async let y = transferToNonIsolatedInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsStruct2() async {
  let x = TwoFieldKlassBox()
  async let y = transferToNonIsolatedInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.k1) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsStruct3() async {
  let x = TwoFieldKlassBox()
  async let y = transferToNonIsolatedInt(x.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.k2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsStruct4() async {
  let x = TwoFieldKlassBox()
  async let y = transferToNonIsolatedInt(x.k2.field2) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.k1.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure we error appropriately when accessing a field of a struct in the
// async let rather than the base struct.
func asyncLet_Let_NonIsolated_AccessFieldsTuple1() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToNonIsolatedInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsTuple2() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToNonIsolatedInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.1) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsTuple3() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToNonIsolatedInt(x.0.k1) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.1.k2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_AccessFieldsTuple4() async {
  let x = (TwoFieldKlassBox(), TwoFieldKlassBox())
  async let y = transferToNonIsolatedInt(x.1.k1.field2) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x.0.k2.field) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NonIsolated_CallBuriedInOtherExpr1() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x) + transferToNonIsolatedInt(x2)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
  useValue(x2)
}

func asyncLet_Let_NonIsolated_CallBuriedInOtherExpr2() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x) + transferToNonIsolatedInt(x2)) // expected-warning {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' into async let risks causing data races between async let uses and local uses}}

  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
  useValue(x)
}

// Make sure we emit separate errors for x and x2.
func asyncLet_Let_NonIsolated_CallBuriedInOtherExpr3() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x) + transferToNonIsolatedInt(x2)) // expected-warning {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' into async let risks causing data races between async let uses and local uses}}
  // expected-warning @-2 {{sending 'x' risks causing data races}}
  // expected-note @-3 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  // We only error on the first value captured if multiple values are captured
  // since we track a single partial_apply as a transfer instruction.
  useValue(x) // expected-note {{access can happen concurrently}}
  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
}

// Make sure that we emit an error for transferToNonIsolatedInt in the async val
// function itself.
func asyncLet_Let_NonIsolated_CallBuriedInOtherExpr4() async {
  let x = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x) + transferToNonIsolatedInt(x))

  let _ = await y
}

// Make sure that we emit an error when the same value is used by two async let
// as part of one statement.
func asyncLet_Let_NonIsolated_MultipleAsyncLet1() async {
  let x = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x)) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}
  // expected-note @-2:60 {{access can happen concurrently}}

  let _ = await y
  let _ = await z
}

// Make sure we don't error when we use different values in different async let.
func asyncLet_Let_NonIsolated_MultipleAsyncLet2() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x2))

  let _ = await y
  let _ = await z
}

func asyncLet_Let_NonIsolated_MultipleAsyncLet3() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x2)) // expected-warning {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' into async let risks causing data races between async let uses and local uses}}

  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await y
  let _ = await z
}

func asyncLet_Let_NonIsolated_MultipleAsyncLet4() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x2))

  let _ = await y
  let _ = await z
  useValue(x)
  useValue(x2)
}

func asyncLet_Let_NonIsolated_MultipleAsyncLet5() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x2))

  let _ = await y
  useValue(x)
  let _ = await z
  useValue(x2)
}

func asyncLet_Let_NonIsolated_MultipleAsyncLet6() async {
  let x = NonSendableKlass()
  let x2 = NonSendableKlass()

  async let y = useValue(transferToNonIsolatedInt(x)), z = useValue(transferToNonIsolatedInt(x2)) // expected-warning {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' into async let risks causing data races between async let uses and local uses}}

  let _ = await y
  useValue(x)
  useValue(x2) // expected-note {{access can happen concurrently}}
  let _ = await z
}

////////////////////////////
// MARK: Normal Value Use //
////////////////////////////

func asyncLet_Let_NormalUse_Simple1() async {
  let x = NonSendableKlass()
  async let y = x // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' into async let risks causing data races between async let uses and local uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

func asyncLet_Let_NormalUse_Simple2() async {
  let x = NonSendableKlass()
  async let y = x

  let _ = await y
  useValue(x)
}

func asyncLetWithoutCapture() async {
  // Make sure that we setup y correctly as fresh.
  //
  // NOTE: Error below will go away in next commit.
  async let x: NonSendableKlass = await returnValueFromMain()
  // expected-warning @-1 {{non-sendable type 'NonSendableKlass' returned by implicitly asynchronous call to main actor-isolated function cannot cross actor boundary}}
  let y = await x
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(y) // expected-note {{access can happen concurrently}}
}

func asyncLet_Let_ActorIsolated_Method() async {
  let a = Actor()
  let x = NonSendableKlass()
  async let y = a.useKlass(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to actor-isolated instance method 'useKlass' risks causing data races between actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
  let _ = await y
}

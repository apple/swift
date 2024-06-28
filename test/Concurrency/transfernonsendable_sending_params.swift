// RUN: %target-swift-frontend -emit-sil -parse-as-library -disable-availability-checking -strict-concurrency=complete -verify %s -o /dev/null -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class Klass {}

struct NonSendableStruct {
  var first = Klass()
  var second = Klass()
}

class KlassWithNonSendableStructPair {
  var ns1: NonSendableStruct
  var ns2: (NonSendableStruct, NonSendableStruct)

  init() {
    ns1 = NonSendableStruct()
    ns2 = (ns1, ns1)
  }
}

final class FinalKlassWithNonSendableStructPair {
  var ns1: NonSendableStruct
  var ns2: (NonSendableStruct, NonSendableStruct)

  init() {
    ns1 = NonSendableStruct()
    ns2 = (ns1, ns1)
  }
}

func useValue<T>(_ t: T) {}
func getAny() -> Any { fatalError() }

actor Custom {
}

@globalActor
struct CustomActor {
    static var shared: Custom {
        return Custom()
    }
}

@MainActor func transferToMain<T>(_ t: T) {}
@CustomActor func transferToCustom<T>(_ t: T) {}

func transferArg(_ x: sending Klass) {
}

func transferArgWithOtherParam(_ x: sending Klass, _ y: Klass) {
}

func transferArgWithOtherParam2(_ x: Klass, _ y: sending Klass) {
}

func twoTransferArg(_ x: sending Klass, _ y: sending Klass) {}

@MainActor var globalKlass = Klass()

/////////////////
// MARK: Tests //
/////////////////

func testSimpleTransferLet() {
  let k = Klass()
  transferArg(k) // expected-warning {{sending 'k' risks causing data races}}
  // expected-note @-1 {{'k' used after being passed as a 'sending' parameter}}
  useValue(k) // expected-note {{access can happen concurrently}}
}

func testSimpleTransferVar() {
  var k = Klass()
  k = Klass()
  transferArg(k) // expected-warning {{sending 'k' risks causing data races}}
  // expected-note @-1 {{'k' used after being passed as a 'sending' parameter}}
  useValue(k) // expected-note {{access can happen concurrently}}
}

func testSimpleTransferUseOfOtherParamNoError() {
  let k = Klass()
  let k2 = Klass()
  transferArgWithOtherParam(k, k2)
  useValue(k2)
}

func testSimpleTransferUseOfOtherParamNoError2() {
  let k = Klass()
  let k2 = Klass()
  transferArgWithOtherParam2(k, k2)
  useValue(k)
}

@MainActor func transferToMain2(_ x: sending Klass, _ y: Klass, _ z: Klass) async {

}

// TODO: How to test this?
func testNonStrongTransferDoesntMerge() async {
}

//////////////////////////////////
// MARK: Transferring Parameter //
//////////////////////////////////

func testTransferringParameter_canTransfer(_ x: sending Klass, _ y: Klass) async {
  await transferToMain(x)
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

func testTransferringParameter_cannotTransferTwice(_ x: sending Klass, _ y: Klass) async {
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  // TODO: We should not error on this since we are sending to the same place.
  await transferToMain(x) // expected-note {{access can happen concurrently}}
}

func testTransferringParameter_cannotUseAfterTransfer(_ x: sending Klass, _ y: Klass) async {
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

actor MyActor {
  var field = Klass()

  func canTransferWithTransferringMethodArg(_ x: sending Klass, _ y: Klass) async {
    await transferToMain(x)
    await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
    // expected-note @-1 {{sending 'self'-isolated 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }

  func getNormalErrorIfTransferTwice(_ x: sending Klass) async {
    await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}
    await transferToMain(x) // expected-note {{access can happen concurrently}}
  }

  func getNormalErrorIfUseAfterTransfer(_ x: sending Klass) async {
    await transferToMain(x)  // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}
    useValue(x) // expected-note {{access can happen concurrently}}
  }

  // After assigning into the actor, we can still use x in the actor as long as
  // we don't transfer it.
  func assignTransferringIntoActor(_ x: sending Klass) async {
    field = x
    useValue(x)
  }

  // Once we assign into the actor, we cannot transfer further.
  func assignTransferringIntoActor2(_ x: sending Klass) async {
    field = x
    await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }
}

@MainActor func canAssignTransferringIntoGlobalActor(_ x: sending Klass) async {
  globalKlass = x
}

@MainActor func canAssignTransferringIntoGlobalActor2(_ x: sending Klass) async {
  globalKlass = x
  // TODO: This is incorrect! sending should be independent of @MainActor.
  await transferToCustom(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending main actor-isolated 'x' to global actor 'CustomActor'-isolated global function 'transferToCustom' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
}

@MainActor func canAssignTransferringIntoGlobalActor3(_ x: sending Klass) async {
  await transferToCustom(globalKlass) // expected-warning {{sending main actor-isolated value of type 'Klass' with later accesses to global actor 'CustomActor'-isolated context risks causing data races}}
}

func canTransferAssigningIntoLocal(_ x: sending Klass) async {
  let _ = x
  await transferToMain(x)
}

func canTransferAssigningIntoLocal2(_ x: sending Klass) async {
  let _ = x
  await transferToMain(x)
  // We do not error here since we just load the value and do not do anything
  // with it.
  //
  // TODO: We should change let _ = x so that it has a move_value '_' or
  // something like that. It will also help move checking as well.
  let _ = x
}

func canTransferAssigningIntoLocal2a(_ x: sending Klass) async {
  let _ = x
  await transferToMain(x)
  // We do not error here since we just load the value and do not do anything
  // with it.
  //
  // TODO: We should change let _ = x so that it has a move_value '_' or
  // something like that. It will also help move checking as well.
  _ = x
}

func canTransferAssigningIntoLocal3(_ x: sending Klass) async {
  let _ = x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  let y = x // expected-note {{access can happen concurrently}}
  _ = y
}

//////////////////////////////////////
// MARK: Transferring is "var" like //
//////////////////////////////////////

// Assigning into a 'sending' parameter is a merge.
func assigningIsAMerge(_ x: sending Klass) async {
  let y = Klass()

  x = y

  // We can still transfer y since x is disconnected.
  await transferToMain(y)
}

func assigningIsAMergeError(_ x: sending Klass) async {
  let y = Klass()

  x = y

  // We can still transfer y since x is disconnected.
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func assigningIsAMergeAny(_ x: sending Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  x = y

  await transferToMain(y)
}

func assigningIsAMergeAnyError(_ x: sending Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  x = y

  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func canTransferAfterAssign(_ x: sending Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  // y is transferred into x.
  await transferToMain(x)

  x = y

  useValue(x)
}

func canTransferAfterAssignButUseIsError(_ x: sending Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  // y is transferred into x.
  x = y

  // TODO: This should refer to the sending parameter.
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func assignToEntireValueEliminatesEarlierTransfer(_ x: sending Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  useValue(x)

  // Transfer x
  await transferToMain(x)

  // y is transferred into x. This shouldn't error.
  x = y

  useValue(x)
}

func mergeDoesNotEliminateEarlierTransfer(_ x: sending NonSendableStruct) async {


  // Ok, this is disconnected.
  let y = Klass()

  useValue(x)

  // Transfer x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  // y is assigned into a field of x.
  x.first = y // expected-note {{access can happen concurrently}}

  useValue(x)
}

func mergeDoesNotEliminateEarlierTransfer2(_ x: sending NonSendableStruct) async {
  // Ok, this is disconnected.
  let y = Klass()

  useValue(x)

  // Transfer x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  x.first = y  // expected-note {{access can happen concurrently}}
}

func doubleArgument() async {
  let x = Klass()
  twoTransferArg(x, x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a 'sending' parameter}}
  // expected-note @-2 {{access can happen concurrently}}
}

func testTransferSrc(_ x: sending Klass) async {
  let y = Klass()
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  x = y // expected-note {{access can happen concurrently}}
}

func testTransferOtherParam(_ x: sending Klass, y: Klass) async {
  x = y
}

func testTransferOtherParamTuple(_ x: sending Klass, y: (Klass, Klass)) async {
  x = y.0
}

func fakeInitOutside(operation: sending @escaping () async -> ()) {}

func taskIsolatedOutsideError(_ x: @escaping @MainActor () async -> ()) {
  fakeInitOutside(operation: x) // okay; x is @Sendable
}

@MainActor func actorIsolatedOutsideError(_ x: @escaping @MainActor () async -> ()) {
  fakeInitOutside(operation: x) // okay; x is @Sendable
}

func taskIsolatedInsideError(_ x: @escaping @MainActor () async -> ()) {
  func fakeInit(operation: sending @escaping () async -> ()) {}

  fakeInit(operation: x)  // okay; x is @Sendable
}

@MainActor func actorIsolatedInsideError(_ x: @escaping @MainActor () async -> ()) {
  func fakeInit(operation: sending @escaping () async -> ()) {}

  // We do not get an error here since fakeInit is main actor isolated since it
  // is defined within this function. As a result, we are sending a @MainActor
  // isolated thing to a MainActor isolated thing.
  fakeInit(operation: x)
}

// Make sure we error here on only the second since x by being assigned a part
// of y becomes task-isolated
func testMergeWithTaskIsolated(_ x: sending Klass, y: Klass) async {
  await transferToMain(x)
  x = y
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

@MainActor func testMergeWithActorIsolated(_ x: sending Klass, y: Klass) async {
  x = y
  await transferToCustom(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending main actor-isolated 'x' to global actor 'CustomActor'-isolated global function 'transferToCustom' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
}


@available(SwiftStdlib 5.1, *)
actor NonSendableInit {
  var first: Klass
  var second: Klass? = nil {
    @storageRestrictions(initializes: first)
    init(initialValue)  {
      transferArg(initialValue!) // expected-warning {{sending 'initialValue' risks causing data races}}
      // expected-note @-1 {{'self'-isolated 'initialValue' is passed as a 'sending' parameter}}
      first = initialValue!
    }

    get { fatalError() }
    set { fatalError() }
  }
}

func testNoCrashWhenSendingNoEscapeClosure() async {
  func test(_ x: sending () -> ()) async {}

  let c = Klass()
  await test { print(c) }
}

// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify -verify-additional-prefix tns-  %s -o /dev/null -parse-as-library -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {} // expected-complete-note {{}}
final class SendableKlass : Sendable {}

actor CustomActorInstance {}

@globalActor
struct CustomActor {
  static let shared = CustomActorInstance()
}

func transferToNonIsolated<T>(_ t: T) async {}
@MainActor func transferToMainActor<T>(_ t: T) async {}
@CustomActor func transferToCustomActor<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}
func useValueAsync<T>(_ t: T) async {}
@MainActor func useValueMainActor<T>(_ t: T) {}
@MainActor func mainActorFunction() {}

var booleanFlag: Bool { false }
@MainActor var mainActorIsolatedGlobal = NonSendableKlass()
@CustomActor var customActorIsolatedGlobal = NonSendableKlass()

@MainActor
class NonSendableGlobalActorIsolatedKlass {}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedKlass: Sendable {}

@MainActor
struct NonSendableGlobalActorIsolatedStruct {
  var k = NonSendableKlass()
}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedStruct: Sendable {}

@MainActor
enum NonSendableGlobalActorIsolatedEnum {
  case first
  case second(NonSendableKlass)
  case third(SendableKlass)
}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedEnum: Sendable {}


/////////////////
// MARK: Tests //
/////////////////

private class NonSendableLinkedList<T> { // expected-complete-note 5{{}}
  var listHead: NonSendableLinkedListNode<T>?

  init() { listHead = nil }
}

private class NonSendableLinkedListNode<T> { // expected-complete-note 3{{}}
  var next: NonSendableLinkedListNode?
  var data: T?

  init() { next = nil }
}

@CustomActor private var firstList = NonSendableLinkedList<Int>()
@CustomActor private var secondList = NonSendableLinkedList<Int>()

@CustomActor func useCustomActor1() async {
  let x = firstList

  await transferToMainActor(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending global actor 'CustomActor'-isolated 'x' to main actor-isolated global function 'transferToMainActor' risks causing data races between main actor-isolated and global actor 'CustomActor'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableLinkedList<Int>' into main actor-isolated context may introduce data races}}

  let y = secondList.listHead!.next!

  await transferToMainActor(y) // expected-tns-warning {{sending 'y' risks causing data races}}
  // expected-tns-note @-1 {{sending global actor 'CustomActor'-isolated 'y' to main actor-isolated global function 'transferToMainActor' risks causing data races between main actor-isolated and global actor 'CustomActor'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableLinkedListNode<Int>' into main actor-isolated context may introduce data races}}
}

@CustomActor func useCustomActor2() async {
  var x = NonSendableLinkedListNode<Int>()

  if booleanFlag {
    x = secondList.listHead!.next!
  }

  await transferToMainActor(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending global actor 'CustomActor'-isolated 'x' to main actor-isolated global function 'transferToMainActor' risks causing data races between main actor-isolated and global actor 'CustomActor'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendableLinkedListNode<Int>' into main actor-isolated context may introduce data races}}
}

@CustomActor func useCustomActor3() async {
  var x = NonSendableLinkedListNode<Int>()

  if booleanFlag {
    x = secondList.listHead!.next!
  }

  await transferToCustomActor(x)
}

@CustomActor func useCustomActor4() async {
  let x = NonSendableLinkedListNode<Int>()

  await transferToCustomActor(x)

  useValue(x)
}

@CustomActor func useCustomActor5() async {
  let x = NonSendableLinkedListNode<Int>()

  // This is ok since the nonisolated function cannot transfer x, so once we
  // return x will be isolated again.
  await transferToNonIsolated(x)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendableLinkedListNode<Int>' outside of global actor 'CustomActor'-isolated context may introduce data races}}

  useValue(x)
}

private struct StructContainingValue { // expected-complete-note 2{{}}
  var x = NonSendableLinkedList<Int>()
  var y = SendableKlass()
}

@CustomActor func useCustomActor6() async {
  var x = StructContainingValue()
  x = StructContainingValue()

  // This is ok since the nonisolated function cannot transfer x meaning after
  // we return we know that x will be disconnected upon return as well.
  await transferToNonIsolated(x)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'StructContainingValue' outside of global actor 'CustomActor'-isolated context may introduce data races}}

  useValue(x)
}

@CustomActor func useCustomActor7() async {
  var x = StructContainingValue()
  x.x = firstList

  await transferToNonIsolated(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending global actor 'CustomActor'-isolated 'x' to nonisolated global function 'transferToNonIsolated' risks causing data races between nonisolated and global actor 'CustomActor'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'StructContainingValue' outside of global actor 'CustomActor'-isolated context may introduce data races}}

  useValue(x)
}

@CustomActor func useCustomActor8() async {
  var x = (NonSendableLinkedList<Int>(), NonSendableLinkedList<Int>())
  x = (NonSendableLinkedList<Int>(), NonSendableLinkedList<Int>())

  // This is safe since the nonisolated function cannot transfer x further.
  await transferToNonIsolated(x)
  // expected-complete-warning @-1 {{passing argument of non-sendable type '(NonSendableLinkedList<Int>, NonSendableLinkedList<Int>)' outside of global actor 'CustomActor'-isolated context may introduce data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(NonSendableLinkedList<Int>, NonSendableLinkedList<Int>)' outside of global actor 'CustomActor'-isolated context may introduce data races}}

  useValue(x)
}

@CustomActor func useCustomActor9() async {
  var x = (NonSendableLinkedList<Int>(), NonSendableLinkedList<Int>())

  x.1 = firstList

  await transferToNonIsolated(x) // expected-tns-warning {{sending 'x' risks causing data races}}
  // expected-tns-note @-1 {{sending global actor 'CustomActor'-isolated 'x' to nonisolated global function 'transferToNonIsolated' risks causing data races between nonisolated and global actor 'CustomActor'-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(NonSendableLinkedList<Int>, NonSendableLinkedList<Int>)' outside of global actor 'CustomActor'-isolated context may introduce data races}}
  // expected-complete-warning @-3 {{passing argument of non-sendable type '(NonSendableLinkedList<Int>, NonSendableLinkedList<Int>)' outside of global actor 'CustomActor'-isolated context may introduce data races}}

  useValue(x)
}

struct Clock {
  public func measure<T>(
    _ work: () async throws -> T
  ) async rethrows -> T {
    try await work()
  }

  public func sleep<T>() async throws -> T { fatalError() }
}

// We used to crash when inferring the type for the diagnostic below.
@MainActor func testIndirectParametersHandledCorrectly() async {
  let c = Clock()
  let _: Int = await c.measure { // expected-tns-warning {{sending main actor-isolated value of type '() async -> Int' with later accesses to nonisolated context risks causing data races}}
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() async -> Int' outside of main actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
    try! await c.sleep()
  }
}

@CustomActor func testGlobalAndGlobalIsolatedPartialApplyMismatch() {
  let ns = customActorIsolatedGlobal

  let _ = { @MainActor in
    // TODO: The type checker seems to think that the isolation here is
    // nonisolated instead of custom actor isolated.
    print(ns) // expected-tns-warning {{sending 'ns' risks causing data races}}
    // expected-tns-note @-1 {{global actor 'CustomActor'-isolated 'ns' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    // expected-complete-warning @-2 {{capture of 'ns' with non-sendable type 'NonSendableKlass' in a `@Sendable` closure}}
  }

  useValue(ns)
}

@MainActor func testGlobalAndGlobalIsolatedPartialApplyMatch() {
  let ns = mainActorIsolatedGlobal

  // This is not a transfer since ns is already main actor isolated.
  let _ = { @MainActor in
    print(ns)
  }

  useValue(ns)
}

@MainActor func testGlobalAndGlobalIsolatedPartialApplyMatch2() {
  var ns = (NonSendableKlass(), NonSendableKlass())
  ns.0 = mainActorIsolatedGlobal

  // This is not a transfer since ns is already main actor isolated.
  let _ = { @MainActor in
    print(ns)
  }

  useValue(ns)
}

@MainActor func testGlobalAndDisconnected() {
  let ns = NonSendableKlass()

  let _ = { @MainActor in
    print(ns)
  }

  // Since useValue is running in an actor isolated context, it is ok to use the
  // transferred value 'ns' here.
  useValue(ns)
}

@MainActor func synchronousActorIsolatedClosureError() async {
  let closure = { @MainActor @Sendable in
    MainActor.assertIsolated()
  }

  let erased: () -> Void = closure

  await useValueAsync(erased) // expected-tns-warning {{sending 'erased' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'erased' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> Void' outside of main actor-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousActorIsolatedFunctionError() async {
  let erased: () -> Void = mainActorFunction

  await useValueAsync(erased) // expected-tns-warning {{sending 'erased' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'erased' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> Void' outside of main actor-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousActorIsolatedGenericFunctionError<T>(_ t: T) async {
  let erased: (T) -> Void = useValueMainActor

  await useValueAsync(erased) // expected-tns-warning {{sending 'erased' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'erased' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '(T) -> Void' outside of main actor-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousActorIsolatedClassMethodError() async {
  @MainActor class Test {
    func foo() {}
  }

  let t = Test()
  let erased: () -> Void = t.foo

  await useValueAsync(erased) // expected-tns-warning {{sending 'erased' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'erased' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> Void' outside of main actor-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousActorIsolatedFinalClassMethodError() async {
  @MainActor final class Test {
    func foo() {}
  }

  let t = Test()
  let erased: () -> Void = t.foo

  await useValueAsync(erased) // expected-tns-warning {{sending 'erased' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'erased' to nonisolated global function 'useValueAsync' risks causing data races between nonisolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> Void' outside of main actor-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousClosureCapturingGlobalActorIsolatedGlobal() async {
  let closure = {
    print(mainActorIsolatedGlobal)
  }
  // Regions: [{(closure), @MainActor}]
  await transferToCustomActor(closure) // expected-tns-warning {{sending 'closure' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'closure' to global actor 'CustomActor'-isolated global function 'transferToCustomActor' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> ()' into global actor 'CustomActor'-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

@MainActor func synchronousClosureCapturingGlobalActorIsolatedFunction() async {
  let closure = {
    mainActorFunction()
  }
  await transferToCustomActor(closure) // expected-tns-warning {{sending 'closure' risks causing data races}}
  // expected-tns-note @-1 {{sending main actor-isolated 'closure' to global actor 'CustomActor'-isolated global function 'transferToCustomActor' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> ()' into global actor 'CustomActor'-isolated context may introduce data races}}
  // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
}

// RUN: %target-swift-frontend -emit-sil -parse-as-library -strict-concurrency=complete -disable-availability-checking -verify %s -o /dev/null -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {} // expected-note {{}}

struct NonSendableStruct {
  var first = NonSendableKlass()
  var second = NonSendableKlass()
}

func getValue<T>() -> T { fatalError() }
func getValueAsync<T>() async -> T { fatalError() }
func getValueAsyncTransferring<T>() async -> sending T { fatalError() }

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

@MainActor func transferToMainIndirect<T>(_ t: T) async {}
@CustomActor func transferToCustomIndirect<T>(_ t: T) async {}
@MainActor func transferToMainDirect(_ t: NonSendableKlass) async {}
@CustomActor func transferToCustomDirect(_ t: NonSendableKlass) async {}
func useValueIndirect<T>(_ t: T) {}
func useValueDirect(_ t: NonSendableKlass) {}

func transferValueDirect(_ x: sending NonSendableKlass) {}
func transferValueIndirect<T>(_ x: sending T) {}

func transferResult() -> sending NonSendableKlass { NonSendableKlass() }
func transferResultWithArg(_ x: NonSendableKlass) -> sending NonSendableKlass { NonSendableKlass() }
func transferResultWithTransferringArg(_ x: sending NonSendableKlass) -> sending NonSendableKlass { NonSendableKlass() }
func transferResultWithTransferringArg2(_ x: sending NonSendableKlass, _ y: NonSendableKlass) -> sending NonSendableKlass { NonSendableKlass() }
func transferResultWithTransferringArg2Throwing(_ x: sending NonSendableKlass, _ y: NonSendableKlass) throws -> sending NonSendableKlass { NonSendableKlass() }

func transferAsyncResult() async -> sending NonSendableKlass { NonSendableKlass() }
func transferAsyncResultWithArg(_ x: NonSendableKlass) async -> sending NonSendableKlass { NonSendableKlass() }
func transferAsyncResultWithTransferringArg(_ x: sending NonSendableKlass) async -> sending NonSendableKlass { NonSendableKlass() }
func transferAsyncResultWithTransferringArg2(_ x: sending NonSendableKlass, _ y: NonSendableKlass) async -> sending NonSendableKlass { NonSendableKlass() }
func transferAsyncResultWithTransferringArg2Throwing(_ x: sending NonSendableKlass, _ y: NonSendableKlass) async throws -> sending NonSendableKlass { NonSendableKlass() }

@MainActor func transferAsyncResultMainActor() async -> sending NonSendableKlass { NonSendableKlass() }

@MainActor var globalNonSendableKlass = NonSendableKlass()

@MainActor
struct MainActorIsolatedStruct {
  let ns = NonSendableKlass()
}

@MainActor
enum MainActorIsolatedEnum {
  case first
  case second(NonSendableKlass)
}

/////////////////
// MARK: Tests //
/////////////////

func simpleTest() async {
  let x = NonSendableKlass()
  let y = transferResultWithArg(x)
  await transferToMainDirect(x)
  useValue(y)
}

// Since y is transfered, we should emit the error on useValue(x). We generally
// emit the first seen error on a path, so if we were to emit an error on
// useValue(y), we would have emitted that error.
func simpleTest2() async {
  let x = NonSendableKlass()
  let y = transferResultWithArg(x)
  await transferToMainDirect(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainDirect' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(y)
  useValue(x) // expected-note {{access can happen concurrently}}
}

// Make sure that later errors with y can happen.
func simpleTest3() async {
  let x = NonSendableKlass()
  let y = transferResultWithArg(x)
  await transferToMainDirect(x)
  await transferToMainDirect(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMainDirect' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(y) // expected-note {{access can happen concurrently}}
}

func transferResult() async -> sending NonSendableKlass {
  let x = NonSendableKlass()
  await transferToMainDirect(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMainDirect' risks causing data races between main actor-isolated and local nonisolated uses}}
  return x // expected-note {{access can happen concurrently}}
}

func transferInAndOut(_ x: sending NonSendableKlass) -> sending NonSendableKlass {
  x
}


func transferReturnArg(_ x: NonSendableKlass) -> sending NonSendableKlass {
  return x // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{task-isolated 'x' cannot be a 'sending' result. task-isolated uses may race with caller uses}}
}

// TODO: This will be fixed once I represent @MainActor on func types.
@MainActor func transferReturnArgMainActor(_ x: NonSendableKlass) -> sending NonSendableKlass {
  return x // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'x' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}
}

// This is safe since we are returning the whole tuple fresh. In contrast,
// (transferring NonSendableKlass, sending NonSendableKlass) would not be
// safe if we ever support that.
func transferReturnArgTuple(_ x: sending NonSendableKlass) -> sending (NonSendableKlass, NonSendableKlass) {
  return (x, x)
}

func useTransferredResultMainActor() async {
  let _ = await transferAsyncResultMainActor()
}

func useTransferredResult() async {
  let _ = await transferAsyncResult()
}

extension MainActorIsolatedStruct {
  func testNonSendableErrorReturnWithTransfer() -> sending NonSendableKlass {
    return ns // expected-warning {{sending 'self.ns' risks causing data races}}
    // expected-note @-1 {{main actor-isolated 'self.ns' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}
  }
  func testNonSendableErrorReturnNoTransfer() -> NonSendableKlass {
    return ns
  }
}

extension MainActorIsolatedEnum {
  func testSwitchReturn() -> sending NonSendableKlass? {
    switch self {
    case .first:
      return nil
    case .second(let ns):
      return ns
    }
  } // expected-warning {{sending 'ns.some' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'ns.some' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}

  func testSwitchReturnNoTransfer() -> NonSendableKlass? {
    switch self {
    case .first:
      return nil
    case .second(let ns):
      return ns
    }
  }

  func testIfLetReturn() -> sending NonSendableKlass? {
    if case .second(let ns) = self {
      return ns // TODO: The error below should be here.
    }
    return nil
  } // expected-warning {{sending 'ns.some' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'ns.some' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}} 

  func testIfLetReturnNoTransfer() -> NonSendableKlass? {
    if case .second(let ns) = self {
      return ns
    }
    return nil
  }

}

///////////////////////////
// MARK: Async Let Tests //
///////////////////////////
//
// Move these tests to async let once strict-concurrency=complete requires
// transfer non sendable.

// Make sure that we can properly construct a reabstraction thunk since
// constructNonSendableKlassAsync doesn't return the value sending but
// async let wants it to be transferring.
//
// Importantly, we should only emit the sema error here saying that one cannot
// return a non-Sendable value here.
func asyncLetReabstractionThunkTest() async {
  // With thunk.
  async let newValue: NonSendableKlass = await getValueAsync()
  let _ = await newValue

  // Without thunk.
  async let newValue2: NonSendableKlass = await getValueAsyncTransferring()
  let _ = await newValue2
}

@MainActor func asyncLetReabstractionThunkTestGlobalActor() async {
  // With thunk. We emit the sema error here.
  async let newValue: NonSendableKlass = await getValueAsync() // expected-warning {{non-sendable type 'NonSendableKlass' returned by implicitly asynchronous call to nonisolated function cannot cross actor boundary}}
  let _ = await newValue

  // Without thunk.
  async let newValue2: NonSendableKlass = await getValueAsyncTransferring()
  let _ = await newValue2
}



// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -verify %s -o /dev/null -swift-version 6

// REQUIRES: concurrency

// This test makes sure that all of our warnings are errors in swift6 mode.

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableType {}

@MainActor func transferToMain<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}
func transferValue<T>(_ t: sending T) {}

/////////////////
// MARK: Tests //
/////////////////

func testIsolationError() async {
  let x = NonSendableType()
  await transferToMain(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

func testTransferArgumentError(_ x: NonSendableType) async {
  await transferToMain(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

func testPassArgumentAsTransferringParameter(_ x: NonSendableType) async {
  transferValue(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{task-isolated 'x' is passed as a 'sending' parameter; Uses in callee may race with later task-isolated uses}}
}

func testAssignmentIntoTransferringParameter(_ x: sending NonSendableType) async {
  let y = NonSendableType()
  await transferToMain(x)
  x = y
  useValue(y)
}

func testAssigningParameterIntoTransferringParameter(_ x: sending NonSendableType, _ y: NonSendableType) async {
  x = y
}

func testIsolationCrossingDueToCapture() async {
  let x = NonSendableType()
  let _ = { @MainActor in
    print(x) // expected-error {{sending 'x' risks causing data races}}
    // expected-note @-1 {{'x' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
  useValue(x) // expected-note {{access can happen concurrently}}
}

func testIsolationCrossingDueToCaptureParameter(_ x: NonSendableType) async {
  let _ = { @MainActor in
    print(x) // expected-error {{sending 'x' risks causing data races}}
    // expected-note @-1 {{task-isolated 'x' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
  useValue(x)
}

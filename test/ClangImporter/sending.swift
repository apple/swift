// RUN: %target-swift-frontend -swift-version 6 -disable-availability-checking -emit-sil -o /dev/null %s -parse-as-library -enable-experimental-feature SendingArgsAndResults -verify -import-objc-header %S/Inputs/sending.h

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

// Make our non sendable c struct non-Sendable
@available(*, unavailable)
extension NonSendableCStruct: Sendable {}

@MainActor func sendToMain<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}

/////////////////
// MARK: Tests //
/////////////////

func funcTestSendingResult() async {
  let x = NonSendableCStruct()
  let y = sendUserDefinedFromGlobalFunction(x)
  await sendToMain(x)
  useValue(y)

  // Just to show that without the sending param, we generate diagnostics.
  let x2 = NonSendableCStruct()
  let y2 = returnUserDefinedFromGlobalFunction(x2)
  await sendToMain(x2) // expected-error {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' to main actor-isolated global function 'sendToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(y2) // expected-note {{access can happen concurrently}}
}

func funcTestSendingArg() async {
  let x = NonSendableCStruct()
  sendUserDefinedIntoGlobalFunction(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a 'sending' parameter}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

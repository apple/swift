// RUN: %target-swift-frontend -swift-version 6 -disable-availability-checking -emit-sil -o /dev/null %s -parse-as-library -verify -import-objc-header %S/Inputs/transferring.h

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

// Make our non sendable c struct non-Sendable
@available(*, unavailable)
extension NonSendableCStruct: Sendable {}

@MainActor func transferToMain<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}

/////////////////
// MARK: Tests //
/////////////////

func funcTestTransferringResult() async {
  let x = NonSendableCStruct()
  let y = transferUserDefinedFromGlobalFunction(x)
  await transferToMain(x)
  useValue(y)

  // Just to show that without the transferring param, we generate diagnostics.
  let x2 = NonSendableCStruct()
  let y2 = returnUserDefinedFromGlobalFunction(x2)
  await transferToMain(x2) // expected-error {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(y2) // expected-note {{access can happen concurrently}}
}

func funcTestTransferringArg() async {
  let x = NonSendableCStruct()
  transferUserDefinedIntoGlobalFunction(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a 'sending' parameter}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

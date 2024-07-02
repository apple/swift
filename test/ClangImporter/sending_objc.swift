// RUN: %target-swift-frontend -swift-version 6 -disable-availability-checking -emit-sil -o /dev/null %s -parse-as-library -verify -import-objc-header %S/Inputs/sending.h

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: objc_interop

////////////////////////
// MARK: Declarations //
////////////////////////

@MainActor func sendToMain<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}

/////////////////
// MARK: Tests //
/////////////////

func methodTestSendingResult() async {
  let x = MyType()
  let y = x.getSendingResult()
  await sendToMain(x)
  useValue(y)
}

func methodTestSendingArg() async {
  let x = MyType()
  let s = NSObject()
  let _ = x.getResultWithSendingArgument(s)  // expected-error {{sending 's' risks causing data races}}
  // expected-note @-1 {{'s' used after being passed as a 'sending' parameter; Later uses could race}}
  useValue(s) // expected-note {{access can happen concurrently}}
}

// Make sure we just ignore the swift_attr if it is applied to something like a
// class.
func testDoesntMakeSense() {
  let _ = DoesntMakeSense()
}

func funcTestSendingResult() async {
  let x = NSObject()
  let y = sendNSObjectFromGlobalFunction(x)
  await sendToMain(x)
  useValue(y)

  // Just to show that without the sendring param, we generate diagnostics.
  let x2 = NSObject()
  let y2 = returnNSObjectFromGlobalFunction(x2)
  await sendToMain(x2) // expected-error {{sending 'x2' risks causing data races}}
  // expected-note @-1 {{sending 'x2' to main actor-isolated global function 'sendToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(y2) // expected-note {{access can happen concurrently}}
}

func funcTestSendingArg() async {
  let x = NSObject()
  sendNSObjectToGlobalFunction(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a 'sending' parameter; Later uses could race}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

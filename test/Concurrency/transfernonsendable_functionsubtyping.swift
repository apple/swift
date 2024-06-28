// RUN: %target-typecheck-verify-swift -swift-version 6

// READ THIS! This file only contains tests that validate that the relevant
// function subtyping rules for sending work. Please do not put other tests in
// the file!

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}

protocol ProtocolWithSendingReqs {
  func sendingResult() -> sending NonSendableKlass // expected-note {{}}
  func nonSendingParam(_ x: NonSendableKlass) // expected-note {{}}
}

protocol ProtocolWithMixedReqs {
  func nonSendingParamAndSendingResult(_ x: NonSendableKlass) -> sending NonSendableKlass // expected-note 4{{}}
}

/////////////////////////////////
// MARK: Normal Function Tests //
/////////////////////////////////

func functionWithSendingResult() -> sending NonSendableKlass { fatalError() }
func functionWithoutSendingResult() -> NonSendableKlass { fatalError() }
func functionWithSendingParameter(_ x: sending NonSendableKlass)  { fatalError() }
func functionWithoutSendingParameter(_ x: NonSendableKlass)  { fatalError() }

func takeFnWithSendingResult(_ fn: () -> sending NonSendableKlass) {}
func takeFnWithoutSendingResult(_ fn: () -> NonSendableKlass) {}
func takeFnWithSendingParam(_ fn: (sending NonSendableKlass) -> ()) {}
func takeFnWithoutSendingParam(_ fn: (NonSendableKlass) -> ()) {}

func testFunctionMatching() {
  let _: (NonSendableKlass) -> () = functionWithSendingParameter
  // expected-error @-1 {{converting a value of type '@Sendable (sending NonSendableKlass) -> ()' to type '(NonSendableKlass) -> ()' risks causing data races}}
  // expected-note @-2 {{converting a function typed value with a sending parameter to one without risks allowing actor-isolated values to escape their isolation domain as an argument to an invocation of value}}
  let _: (sending NonSendableKlass) -> () = functionWithSendingParameter

  let _: (NonSendableKlass) -> () = functionWithoutSendingParameter
  let _: (sending NonSendableKlass) -> () = functionWithoutSendingParameter

  takeFnWithSendingParam(functionWithSendingParameter)
  takeFnWithoutSendingParam(functionWithSendingParameter)
  // expected-error @-1 {{converting a value of type '@Sendable (sending NonSendableKlass) -> ()' to type '(NonSendableKlass) -> ()' risks causing data races}}
  // expected-note @-2 {{converting a function typed value with a sending parameter to one without risks allowing actor-isolated values to escape their isolation domain as an argument to an invocation of value}}
  takeFnWithSendingParam(functionWithoutSendingParameter)
  takeFnWithoutSendingParam(functionWithoutSendingParameter)
}

func testReturnValueMatching() {
  let _: () -> NonSendableKlass = functionWithSendingResult
  let _: () -> sending NonSendableKlass = functionWithSendingResult
  let _: () -> NonSendableKlass = functionWithoutSendingResult
  let _: () -> sending NonSendableKlass = functionWithoutSendingResult
  // expected-error @-1 {{converting a value of type '@Sendable () -> NonSendableKlass' to type '() -> sending NonSendableKlass' risks causing data races}}
  // expected-note @-2 {{converting a function typed value without a sending result as one with risks allowing actor-isolated values to escape their isolation domain through a result of an invocation of value}}

  takeFnWithSendingResult(functionWithSendingResult)
  takeFnWithSendingResult(functionWithoutSendingResult)
  // expected-error @-1 {{converting a value of type '@Sendable () -> NonSendableKlass' to type '() -> sending NonSendableKlass' risks causing data races}}
  // expected-note @-2 {{converting a function typed value without a sending result as one with risks allowing actor-isolated values to escape their isolation domain through a result of an invocation of value}}
  let x: () -> NonSendableKlass = { fatalError() }
  takeFnWithSendingResult(x)
  // expected-error @-1 {{converting a value of type '() -> NonSendableKlass' to type '() -> sending NonSendableKlass' risks causing data races}}
  // expected-note @-2 {{converting a function typed value without a sending result as one with risks allowing actor-isolated values to escape their isolation domain through a result of an invocation of value}}

  takeFnWithoutSendingResult(functionWithSendingResult)
  takeFnWithoutSendingResult(functionWithoutSendingResult)
  takeFnWithoutSendingResult(x)
}

//////////////////////////
// MARK: Protocol Tests //
//////////////////////////

struct MatchSuccess : ProtocolWithSendingReqs, ProtocolWithMixedReqs {
  func sendingResult() -> sending NonSendableKlass { fatalError() }
  func nonSendingParam(_ x: NonSendableKlass) -> () { fatalError() }
  func nonSendingParamAndSendingResult(_ x: NonSendableKlass) -> sending NonSendableKlass { fatalError() }
}

struct FailToMatch : ProtocolWithSendingReqs, ProtocolWithMixedReqs { // expected-error 2{{}}
  func sendingResult() -> NonSendableKlass { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '() -> NonSendableKlass'}}
  func nonSendingParam(_ x: sending NonSendableKlass) -> () { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '(sending NonSendableKlass) -> ()'}}
  func nonSendingParamAndSendingResult(_ x: sending NonSendableKlass) -> NonSendableKlass { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '(sending NonSendableKlass) -> NonSendableKlass'}}
}

struct FailToMatch2 : ProtocolWithMixedReqs { // expected-error {{}}
  func nonSendingParamAndSendingResult(_ x: sending NonSendableKlass) -> NonSendableKlass { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '(sending NonSendableKlass) -> NonSendableKlass'}}
}

struct FailToMatch3 : ProtocolWithMixedReqs { // expected-error {{}}
  func nonSendingParamAndSendingResult(_ x: NonSendableKlass) -> NonSendableKlass { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '(NonSendableKlass) -> NonSendableKlass'}}
}

struct FailToMatch4 : ProtocolWithMixedReqs { // expected-error {{}}
  func nonSendingParamAndSendingResult(_ x: sending NonSendableKlass) -> sending NonSendableKlass { fatalError() }
  // expected-note @-1 {{candidate has non-matching type '(sending NonSendableKlass) -> sending NonSendableKlass'}}
}


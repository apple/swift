// RUN: %target-typecheck-verify-swift -disable-availability-checking -strict-concurrency=complete

// REQUIRES: asserts

func testArg(_ x: transferring String) {
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
}

func testResult() -> transferring String {
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
  ""
}

func testArgResult(_ x: transferring String) -> transferring String {
  // expected-warning @-1 2{{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
}

func testVarDeclDoesntWork() {
  var x: transferring String // expected-error {{'transferring' may only be used on parameter}}
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
}

func testVarDeclTupleElt() -> (transferring String, String) {} // expected-error {{'transferring' cannot be applied to tuple elements}}
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func testVarDeclTuple2(_ x: (transferring String)) {}
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
func testVarDeclTuple2(_ x: (transferring String, String)) {} // expected-error {{'transferring' cannot be applied to tuple elements}}
  // expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func testBorrowSending(_ x: transferring borrowing String) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' cannot be used together with 'borrowing'}}

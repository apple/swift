// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature SendingArgsAndResults -strict-concurrency=complete -enable-upcoming-feature RegionBasedIsolation

// REQUIRES: asserts

func testArg(_ x: sending String) {
}

func testResult() -> sending String {
  ""
}

func testArgResult(_ x: sending String) -> sending String {
}

func testVarDeclDoesntWork() {
  var x: sending String // expected-error {{'sending' may only be used on parameter}}
}

func testVarDeclTupleElt() -> (sending String, String) {} // expected-error {{'sending' cannot be applied to tuple elements}}

func testVarDeclTuple2(_ x: (sending String)) {}
func testVarDeclTuple2(_ x: (sending String, String)) {} // expected-error {{'sending' cannot be applied to tuple elements}}

func testArgWithConsumingWrongOrder(_ x: sending consuming String, _ y: sending inout String) {}
// expected-error @-1 {{'sending' must be placed after specifier 'consuming'}}
// expected-error @-2 {{'sending' must be placed after specifier 'inout'}}

func testArgWithConsumingWrongOrderType(_ x: (sending consuming String, sending inout String) -> ()) {}
// expected-error @-1 {{'sending' must be placed after specifier 'consuming'}}
// expected-error @-2 {{'sending' must be placed after specifier 'inout'}}

func testBorrowSending(_ x: borrowing sending String) {}
// expected-error @-1 {{'sending' cannot be used together with 'borrowing'}}

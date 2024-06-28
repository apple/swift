// RUN: %target-typecheck-verify-swift -strict-concurrency=complete

// REQUIRES: asserts
// REQUIRES: concurrency

// README: This test makes sure that we error when transferring is placed in the
// wrong place with respect to ownership modifiers.

func test_good(_ x: transferring Int) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func test_consuming_before_transferring(_ x: consuming transferring Int) {} // expected-error {{'transferring' must be placed before specifier 'consuming'}}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func test_borrowing_before_transferring(_ x: borrowing transferring Int) {} // expected-error {{'transferring' must be placed before specifier 'borrowing'}}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func test_inout_before_transferring(_ x: inout transferring Int) {} // expected-error {{'transferring' must be placed before specifier 'inout'}}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func test_repeated_transferring(_ x: transferring transferring Int) {} // expected-error {{parameter may have at most one 'transferring' specifier}}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-warning @-2 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func test_repeated_transferring_mixed(_ x: transferring borrowing transferring inout Int) {}
// expected-error @-1 {{'transferring' must be placed before specifier 'borrowing'}}
// expected-error @-2 {{parameter may have at most one 'transferring' specifier}}
// expected-error @-3 {{parameter may have at most one of the 'inout', 'borrowing', or 'consuming' specifiers}}
// expected-warning @-4 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-warning @-5 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-6 {{'transferring' cannot be used together with 'borrowing'}}

// Just until we get the results setup.
func test_transferring_result_in_tuple() -> (transferring Int, Int) {}
// expected-error @-1 {{'transferring' cannot be applied to tuple elements}}
// expected-warning @-2 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

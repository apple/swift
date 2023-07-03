// RUN: %target-typecheck-verify-swift -enable-experimental-feature StrictConcurrency=targeted
// REQUIRES: concurrency

class C { // expected-note{{class 'C' does not conform to the 'Sendable' protocol}}
  var counter = 0
}

func acceptsSendable<T: Sendable>(_: T) { }

func testNoConcurrency(c: C) {
  acceptsSendable(c)
}

@available(SwiftStdlib 5.1, *)
func testConcurrency(c: C) async {
  acceptsSendable(c) // expected-warning{{type 'C' does not conform to the 'Sendable' protocol}}
}


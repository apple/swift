// RUN: %target-typecheck-verify-swift -enable-experimental-concurrency
// REQUIRES: concurrency

// Synthesis of for actores.

@available(SwiftStdlib 5.5, *)
actor A1 {
  var x: Int = 17
}

@available(SwiftStdlib 5.5, *)
actor A2: Actor {
  var x: Int = 17
}

@available(SwiftStdlib 5.5, *)
actor A3<T>: Actor {
  var x: Int = 17
}

@available(SwiftStdlib 5.5, *)
actor A4: A1 { // expected-error{{actor types do not support inheritance}}
}

@available(SwiftStdlib 5.5, *)
actor A5: A2 { // expected-error{{actor types do not support inheritance}}
}

@available(SwiftStdlib 5.5, *)
actor A6: A1, Actor { // expected-error{{redundant conformance of 'A6' to protocol 'Actor'}}
  // expected-note@-1{{'A6' inherits conformance to protocol 'Actor' from superclass here}}
  // expected-error@-2{{actor types do not support inheritance}}
}

// Explicitly satisfying the requirement.

@available(SwiftStdlib 5.5, *)
actor A7 {
  // Okay: satisfy the requirement explicitly
  nonisolated func enqueue(partialTask: PartialAsyncTask) { }
}

// A non-actor can conform to the Actor protocol, if it does it properly.
@available(SwiftStdlib 5.5, *)
class C1: Actor { // expected-error{{non-final class 'C1' cannot conform to `Sendable`; use `UnsafeSendable`}}
  func enqueue(partialTask: PartialAsyncTask) { }
}

// Make sure the conformances actually happen.
@available(SwiftStdlib 5.5, *)
func acceptActor<T: Actor>(_: T.Type) { }

@available(SwiftStdlib 5.5, *)
func testConformance() {
  acceptActor(A1.self)
  acceptActor(A2.self)
  acceptActor(A3<Int>.self)
  acceptActor(A4.self)
  acceptActor(A5.self)
  acceptActor(A6.self)
  acceptActor(A7.self)
}

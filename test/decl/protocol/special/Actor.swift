// RUN: %target-typecheck-verify-swift -enable-experimental-concurrency
// REQUIRES: concurrency

// Synthesis of for actores.

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A1 {
  var x: Int = 17
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A2: Actor {
  var x: Int = 17
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A3<T>: Actor {
  var x: Int = 17
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A4: A1 {
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A5: A2 {
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A6: A1, Actor { // expected-error{{redundant conformance of 'A6' to protocol 'Actor'}}
  // expected-note@-1{{'A6' inherits conformance to protocol 'Actor' from superclass here}}
}

// Explicitly satisfying the requirement.

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
actor A7 {
  // Okay: satisfy the requirement explicitly
  nonisolated func enqueue(partialTask: PartialAsyncTask) { }
}

// A non-actor can conform to the Actor protocol, if it does it properly.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
class C1: Actor { // expected-error{{non-final class 'C1' cannot conform to `Sendable`; use `UnsafeSendable`}}
  func enqueue(partialTask: PartialAsyncTask) { }
}

// Make sure the conformances actually happen.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
func acceptActor<T: Actor>(_: T.Type) { }

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
func testConformance() {
  acceptActor(A1.self)
  acceptActor(A2.self)
  acceptActor(A3<Int>.self)
  acceptActor(A4.self)
  acceptActor(A5.self)
  acceptActor(A6.self)
  acceptActor(A7.self)
}

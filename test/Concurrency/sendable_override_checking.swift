// RUN: %target-typecheck-verify-swift
// REQUIRES: concurrency

@available(SwiftStdlib 5.1, *)
class NotSendable { // expected-note 2{{class 'NotSendable' does not conform to the 'Sendable' protocol}}
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable)
extension NotSendable: Sendable { }

@available(SwiftStdlib 5.1, *)
class Super {
  func f(_: NotSendable) async { }
  @MainActor func g1(_: NotSendable) { }
  @MainActor func g2(_: NotSendable) async { }
}

@available(SwiftStdlib 5.1, *)
class Sub: Super {
  @MainActor override func f(_: NotSendable) async { }
  // expected-warning@-1{{non-sendable type 'NotSendable' in parameter of superclass method overridden by main actor-isolated instance method 'f' cannot cross actor boundary}}

  nonisolated override func g1(_: NotSendable) { } // okay, synchronous

  nonisolated override func g2(_: NotSendable) async { }
  // expected-warning@-1{{non-sendable type 'NotSendable' in parameter of superclass method overridden by nonisolated instance method 'g2' cannot cross actor boundary}}
}

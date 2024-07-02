// RUN: %target-swift-frontend -module-name TestModule -typecheck -verify %s

struct SendableType {}
class NonSendableType {}

func testSendingOnNonSendable(input: sending NonSendableType) {
    // okay
}

func testSendingOnSendable(input: sending SendableType) {
    // expected-warning@-1{{'sending' has no effect}}{{35-43=}}
}

func testSendingOnSendable(input: sending some Sendable) {
    // expected-warning@-1{{'sending' has no effect}}{{35-43=}}
}

func testSendingOnSendable(input: sending any Sendable) {
    // expected-warning@-1{{'sending' has no effect}}{{35-43=}}
}
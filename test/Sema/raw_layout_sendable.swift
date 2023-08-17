// RUN: %target-swift-frontend -enable-experimental-feature StrictConcurrency -enable-experimental-feature RawLayout -typecheck -verify %s

func checkSendable(_: @Sendable () -> ()) {}

@_rawLayout(size: 4, alignment: 4)
struct NotAutomaticallySendableAndNotUsedAsSendable: ~Copyable {}

@_rawLayout(size: 4, alignment: 4)
struct NotAutomaticallySendable: ~Copyable {} // expected-note{{}}

func testNotAutomaticallySendable() {
    let s = NotAutomaticallySendable()

    checkSendable { _ = s } // expected-warning{{capture of 's' with non-sendable type 'NotAutomaticallySendable'}}
}

@_rawLayout(size: 4, alignment: 4)
struct UnuncheckedSendable: ~Copyable, Sendable {} // expected-warning{{@_rawLayout does not conform to the 'Sendable' protocol}}

func testUnuncheckedSendable() {
    let s = UnuncheckedSendable()

    checkSendable { _ = s }
}

@_rawLayout(size: 4, alignment: 4)
struct UncheckedSendable: ~Copyable, @unchecked Sendable {}

func testUncheckedSendable() {
    let s = UncheckedSendable()

    checkSendable { _ = s }
}


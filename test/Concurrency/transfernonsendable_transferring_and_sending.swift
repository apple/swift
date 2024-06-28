// RUN: %target-swift-frontend -emit-sil -swift-version 6 %s -verify

// REQUIRES: concurrency
// REQUIRES: asserts

// READ THIS: This test is meant to test that we properly give warnings for
// transferring/sending.

class NonSendingKlass {}

func transferringParam(_ x: transferring NonSendingKlass) -> () {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func transferringResult() -> transferring NonSendingKlass { fatalError() }
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func transferringAndSendingParam(_ x: sending transferring NonSendingKlass) -> () {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringAndSendingParam2(_ x: transferring sending NonSendingKlass) -> () {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringAndSendingResult() -> sending transferring NonSendingKlass { fatalError() }
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringAndSendingResult2() -> transferring sending NonSendingKlass { fatalError() }
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringParamInType(_ x: (transferring NonSendingKlass) -> ()) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func transferringParamInType2(_ x: (NonSendingKlass, transferring NonSendingKlass) -> ()) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func transferringParamInType3(_ x: (NonSendingKlass, transferring sending NonSendingKlass) -> ()) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}
func transferringParamInType4(_ x: (NonSendingKlass, sending transferring NonSendingKlass) -> ()) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringResultInType(_ x: () -> transferring NonSendingKlass) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}

func transferringResultInType2(_ x: () -> transferring sending NonSendingKlass) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

func transferringResultInType3(_ x: () -> sending transferring NonSendingKlass) {}
// expected-warning @-1 {{'transferring' has been renamed to 'sending' and the 'transferring' spelling will be removed shortly}}
// expected-error @-2 {{'transferring' and 'sending' may not be used together}}

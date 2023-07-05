// REQUIRES: concurrency
// REQUIRES: asserts

// RUN: %target-typecheck-verify-swift -emit-sil -strict-concurrency=complete -enable-experimental-feature DeferredSendableChecking

/*
 This file tests the experimental DeferredSendableChecking feature. This features the passing
 of non-sendable values to isolation-crossing calls to not yield diagnostics during AST passes,
 but to instead emit them later during a mandatory SIL pass.

 Together, `defer_and_silgen.swift` and `defer_and_typecheck_only.swift` test this feature by
 testing the same code, but expecting different sets of diagnostics. The former runs -emit-sil,
 and expects all diagnostics to be emitted, but the latter only runs AST typechecking via -typecheck,
 and expects some sendability diagnostics but not non-sendable arg diagnostics to be emitted.
 */

class NonSendable {
// expected-note@-1 6{{class 'NonSendable' does not conform to the 'Sendable' protocol}}
    var x = 0
}

let globalNS = NonSendable()

@available(SwiftStdlib 5.1, *)
func takesNS(_ : NonSendable) async {}

@available(SwiftStdlib 5.1, *)
func retsNS() async -> NonSendable { NonSendable() }

@available(SwiftStdlib 5.1, *)
func callActorFuncsFromNonisolated(a : A, ns : NonSendable) async {
    // Non-sendable value passed from actor isolated to nonisolated

    await a.actorTakesNS(ns)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    _ = await a.actorRetsNS()
    //expected-warning@-1{{non-sendable type 'NonSendable' returned by implicitly asynchronous call to actor-isolated function cannot cross actor boundary}}
}

@available(SwiftStdlib 5.1, *)
actor A {
    let actorNS = NonSendable()

    func actorTakesNS(_ : NonSendable) async {}

    func actorRetsNS() async -> NonSendable { NonSendable() }

    func callNonisolatedFuncsFromActor(ns: NonSendable) async {
        // Non-sendable value passed from nonisolated to actor isolated

        await takesNS(ns)
        //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' outside of actor-isolated context may introduce data races}}

        _ = await retsNS()
        //expected-warning@-1{{non-sendable type 'NonSendable' returned by implicitly asynchronous call to nonisolated function cannot cross actor boundary}}
    }

    func callActorFuncsFromDiffActor(ns : NonSendable, a : A) async {
        // Non-sendable value passed between the isolation of two different actors

        await a.actorTakesNS(ns)
        //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

        _ = await a.actorRetsNS()
        //expected-warning@-1{{non-sendable type 'NonSendable' returned by implicitly asynchronous call to actor-isolated function cannot cross actor boundary}}
    }
}

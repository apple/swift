// REQUIRES: concurrency
// REQUIRES: asserts

// RUN: %target-typecheck-verify-swift -strict-concurrency=complete -enable-experimental-feature SendNonSendable

/*
 This file tests the experimental SendNonSendable feature. This feature causes the passing
 of non-sendable values to isolation-crossing calls to not yield diagnostics during AST passes,
 but to instead emit them later during a mandatory SIL pass. This file in particular checks that
 isolation crossing via argument passing is deferred, but that isolation crossing via returned
 results is not deferred. This is done because flow-sensitive "region" checking
 (see region_based_sendability.swift) can determine that objects of non-Sendable type can be safely
 returned as arguments, but not that they can be safely returned.
 */

class NonSendable {
// expected-note@-1 3{{class 'NonSendable' does not conform to the 'Sendable' protocol}}
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
    //deferred-warning@-1{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

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
        //deferred-warning@-1{{passing argument of non-sendable type 'NonSendable' outside of actor-isolated context may introduce data races}}

        _ = await retsNS()
        //expected-warning@-1{{non-sendable type 'NonSendable' returned by implicitly asynchronous call to nonisolated function cannot cross actor boundary}}
    }

    func callActorFuncsFromDiffActor(ns : NonSendable, a : A) async {
        // Non-sendable value passed between the isolation of two different actors

        await a.actorTakesNS(ns)
        //deferred-warning@-1{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

        _ = await a.actorRetsNS()
        //expected-warning@-1{{non-sendable type 'NonSendable' returned by implicitly asynchronous call to actor-isolated function cannot cross actor boundary}}
    }
}

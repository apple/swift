// RUN: %target-swift-frontend -typecheck -disable-availability-checking -enable-experimental-async-top-level -swift-version 5 %s -verify
// RUN: %target-swift-frontend -typecheck -disable-availability-checking -swift-version 5 %s -verify

// Even though enable-experimental-async-top-level is enabled, there are no
// `await`s made from the top-level, so it is not an async context. `a` is just
// a normal top-level global variable with no actor isolation.

var a = 10

func nonIsolatedSync() {
    print(a)
    a = a + 10
}

@MainActor
func isolatedSync() { // expected-note 2 {{calls to global function 'isolatedSync()' from outside of its actor context are implicitly asynchronous}}
    print(a)
    a = a + 10
}

func nonIsolatedAsync() async {
    await print(a) // expected-warning {{no 'async' operations occur within 'await' expression}}
    a = a + 10
}

@MainActor
func isolatedAsync() async {
    print(a)
    a = a + 10
}

nonIsolatedSync()
isolatedSync() // expected-error {{call to main actor-isolated global function 'isolatedSync()' in a synchronous nonisolated context}}
nonIsolatedAsync() // expected-error {{'async' call in a function that does not support concurrency}}
isolatedAsync() // expected-error {{'async' call in a function that does not support concurrency}}

print(a)

if a > 10 {
    nonIsolatedSync()
    isolatedSync() // expected-error {{call to main actor-isolated global function 'isolatedSync()' in a synchronous nonisolated context}}
    nonIsolatedAsync() // expected-error {{'async' call in a function that does not support concurrency}}
    isolatedAsync() // expected-error {{'async' call in a function that does not support concurrency}}

    print(a)
}

// RUN: %target-typecheck-verify-swift -disable-availability-checking

// We could make this work by having `lazy` synthesize an effectful
// getter, but for now let's reject it instead of crashing.

func throwsFunc() throws -> Int { return 3 }
func asyncFunc() async -> Int { return 3 }

func localLazyWithEffects() {
  lazy var x = try throwsFunc() // expected-error {{call can throw, but errors cannot be thrown out of a lazy variable initializer}}
  lazy var y = await asyncFunc() // expected-error {{'async' call cannot occur in a lazy variable initializer}}
}

struct InstanceLazyWithEffects {
  lazy var x = try throwsFunc() // expected-error {{call can throw, but errors cannot be thrown out of a property initializer}}
  lazy var y = await asyncFunc() // expected-error {{'async' call cannot occur in a property initializer}}
}

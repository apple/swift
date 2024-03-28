// RUN: %target-swift-frontend  -disable-availability-checking -swift-version 6 -parse-as-library %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

public func takesClosure(_ closure: () -> Void) async {
  closure()
}

@MainActor func mustRunOnMainActor() {
  MainActor.assertIsolated()
}

@MainActor struct S {
  func foo() {}
}

@MainActor class Test {
  var s = S()
  func foo() {}
}

@MainActor protocol P {
  func foo()
}

/////////////////
// MARK: Tests //
/////////////////

@MainActor func testFunction() async {
  await takesClosure(mustRunOnMainActor)
  // expected-error @-1 {{synchronous main actor-isolated global function 'mustRunOnMainActor()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
}

@MainActor func testConcreteInstanceMethod() async {
  let x = Test()
  let y = x.foo
  await takesClosure(y)
  // expected-error @-1 {{synchronous main actor-isolated let 'y' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}

  await takesClosure(x.foo)
  // expected-error @-1 {{synchronous main actor-isolated instance method 'foo()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}

  await takesClosure(x.s.foo)
  // expected-error @-1 {{synchronous main actor-isolated instance method 'foo()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
}

@MainActor func testGenericInstanceMethod<T: P>(_ t: T) async {
  await takesClosure(t.foo)
  // expected-error @-1 {{synchronous main actor-isolated instance method 'foo()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
  let y = t.foo
  await takesClosure(y)
  // expected-error @-1 {{synchronous main actor-isolated let 'y' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}

  let z = t
  await takesClosure(z.foo)
  // expected-error @-1 {{synchronous main actor-isolated instance method 'foo()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
}

@MainActor func testWithCast() async {
  let x = mustRunOnMainActor as Any
  await takesClosure(x as! (@MainActor () -> ()))
  // expected-error @-1 {{synchronous '@MainActor () -> ()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
  await takesClosure(x as! (@Sendable @MainActor () -> ()))
  // expected-error @-1 {{synchronous '@MainActor @Sendable () -> ()' cannot be passed as a '() -> Void'}}
  // expected-note @-2 {{Due to type erasure, the caller of the synchronous closure will not hop to the closure's global actor before invoking it}}
  await takesClosure(x as! (@Sendable () -> ()))
}

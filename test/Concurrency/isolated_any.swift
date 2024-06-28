// RUN: %target-swift-frontend -typecheck -verify -disable-availability-checking -strict-concurrency=complete -enable-upcoming-feature InferSendableFromCaptures %s

// REQUIRES: asserts

func globalNonisolatedFunction() {}
@MainActor func globalMainActorFunction() {}

actor A {
  func actorFunction() {}
  func asyncActorFunction() async {}
  func asyncThrowsActorFunction() async throws {}
  func actorFunctionWithArgs(value: Int) async -> String { "" }
}

func testBasic_sync() {
  // TODO: this is a pretty bad diagnostic
  let fn: @isolated(any) () -> () = globalNonisolatedFunction // expected-note {{calls to let 'fn' from outside of its actor context are implicitly asynchronous}}
  fn() // expected-error {{call to @isolated(any) let 'fn' in a synchronous nonisolated context}}
}

func testBasic_async_noawait() async {
  // TODO: this is a pretty bad diagnostic
  let fn: @isolated(any) () -> () = globalNonisolatedFunction

  // expected-error @+2 {{expression is 'async' but is not marked with 'await'}}
  // expected-note @+1 {{calls to let 'fn' from outside of its actor context are implicitly asynchronous}}
  fn()
}

func testBasic_async_await() async {
  let fn: @isolated(any) () -> () = globalNonisolatedFunction
  await fn()
}

func requireSendableIsolatedAny(_ fn: @Sendable @isolated(any) () -> ()) {}

func testEraseFromNonisolated() {
  requireSendableIsolatedAny(globalNonisolatedFunction)
}

func testEraseFromGlobalActor() {
  requireSendableIsolatedAny(globalMainActorFunction)
}

// Apparently, the only way to get something that Sema will consider
// an actor-instance-isolated `@Sendable` function value is with
// `@_inheritActorContext`.
func requireSendableIsolatedAnyInheritContext(@_inheritActorContext _ fn : @Sendable @isolated(any) () -> ()) {}

extension A {
  func testEraseFromActorFunction() {
    // This actually works even without @isolated(any), which is a bug.
    // But it should definitely work with @isolated(any).
    requireSendableIsolatedAnyInheritContext {
      self.actorFunction()
    }
  }
}

func hasIsolatedArgument(actor: isolated A) {}
func requireIsolatedAnyWithActorArgument(_ fn: @Sendable @isolated(any) (A) -> ()) {}
func testEraseFromIsolatedArgument() {
  // expected-error @+1 {{cannot convert value of type '@Sendable (isolated A) -> ()' to expected argument type '@isolated(any) @Sendable (A) -> ()'}}
  requireIsolatedAnyWithActorArgument(hasIsolatedArgument)
}

func requireSendableNonIsolated(_ fn: @Sendable () -> ()) {}
func testConvertIsolatedAnyToNonIsolated(fn: @Sendable @isolated(any) () -> ()) {
  requireSendableNonIsolated(fn)
}

func requireSendableGlobalActor(_ fn: @Sendable @MainActor () -> ()) {}
func testConvertIsolatedAnyToMainActor(fn: @Sendable @isolated(any) () -> ()) {
  // expected-error @+1 {{cannot convert value of type '@isolated(any) @Sendable () -> ()' to expected argument type '@MainActor @Sendable () -> ()'}}
  requireSendableGlobalActor(fn)
}

func extractFunctionIsolation(_ fn: @isolated(any) @Sendable @escaping () async -> Void) {
  let _: (any Actor)? = extractIsolation(fn)

  let myActor = A()
  let _: (any Actor)? = extractIsolation(myActor.asyncActorFunction)
  let _: (any Actor)? = extractIsolation(myActor.asyncThrowsActorFunction)
  let _: (any Actor)? = extractIsolation(myActor.actorFunctionWithArgs(value:))
}

func extractFunctionIsolationExpr(
  _ fn1: @isolated(any) @Sendable @escaping () async -> Void,
  _ fn2: @isolated(any) @Sendable @escaping (Int, String) -> Bool
) {
  let _: (any Actor)? = fn1.isolation
  let _: (any Actor)? = fn2.isolation

  // Only `@isolated(any)` functions have `.isolation`
  let myActor = A()
  let _: (any Actor)? = myActor.actorFunction.isolation // expected-error {{value of type '@Sendable () -> ()' has no member 'isolation'}}
  let _: (any Actor)? = myActor.asyncActorFunction.isolation // expected-error {{value of type '@Sendable () async -> ()' has no member 'isolation'}}
  let _: (any Actor)? = myActor.asyncThrowsActorFunction.isolation // expected-error {{value of type '@Sendable () async throws -> ()' has no member 'isolation'}}
  let _: (any Actor)? = myActor.actorFunctionWithArgs.isolation // expected-error {{value of type '@Sendable (Int) async -> String' has no member 'isolation'}}

  let _: (any Actor)? = globalNonisolatedFunction.isolation // expected-error {{value of type '@Sendable () -> ()' has no member 'isolation'}}
  let _: (any Actor)? = globalMainActorFunction.isolation // expected-error {{value of type '@MainActor @Sendable () -> ()' has no member 'isolation'}}
}

func requireDotIsolation(_ fn: (any Actor)?) -> (any Actor)? { return fn }

func testDotIsolation() {
  let _ : (any Actor)? = requireDotIsolation(globalMainActorFunction.isolation) // expected-error {{value of type '@MainActor @Sendable () -> ()' has no member 'isolation'}}
  let _ : (any Actor)? = requireDotIsolation(globalNonisolatedFunction.isolation) // expected-error {{value of type '@Sendable () -> ()' has no member 'isolation'}}
}

func testFunctionIsolationExpr1(_ fn: (@isolated(any) () -> Void)?) -> (any Actor)? {
  return fn?.isolation
}

func testFunctionIsolationExpr2(_ fn: Optional<(@isolated(any) () -> Void)>) -> Optional<any Actor> {
  return fn?.isolation
}

func testFunctionIsolationExprTuple(
  _ fn1: (@isolated(any) @Sendable () -> Void)?,
  _ fn2: (@isolated(any) @Sendable () -> Void)?
) -> ((any Actor)?, (any Actor)?)
{
  return (fn1?.isolation, fn2?.isolation)
}

func nonSendableIsolatedAny(
  _ fn: @escaping @isolated(any) () -> Void // expected-note {{parameter 'fn' is implicitly non-sendable}}
) {
  let _: @Sendable () -> Void = fn  // expected-warning {{using non-sendable parameter 'fn' in a context expecting a @Sendable closure}}
}

// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 6 -disable-availability-checking -verify
// REQUIRES: concurrency

func takeSyncIsolatedAny(fn: @escaping @isolated(any) () -> ()) {}
func takeAsyncIsolatedAny(fn: @escaping @isolated(any) () async -> ()) {}

actor MyActor {
  func syncAction() {}
  func asyncAction() async {}
}

// If these are ever accepted, move them to isolated_any.swift and test
// that we generate the right pattern!

func testEraseSyncActorIsolatedPartialApplication(a: MyActor) {
  takeSyncIsolatedAny(fn: a.syncAction) // expected-error {{actor-isolated instance method 'syncAction()' can not be partially applied}}
}

func testEraseSyncAsAsyncActorIsolatedPartialApplication(a: MyActor) {
  takeAsyncIsolatedAny(fn: a.syncAction)  // expected-error {{actor-isolated instance method 'syncAction()' can not be partially applied}}
}

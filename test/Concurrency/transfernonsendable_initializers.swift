// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -disable-availability-checking -swift-version 6 -verify %s -o /dev/null

// REQUIRES: concurrency
// REQUIRES: asserts

// This test validates the behavior of transfernonsendable around initializers.

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}
@MainActor func transferToMain<T>(_ t: T) async {}

actor CustomActorInstance {}

@globalActor
struct CustomActor {
  static let shared = CustomActorInstance()
}

/////////////////
// MARK: Tests //
/////////////////

actor ActorWithSynchronousNonIsolatedInit {
  let k: NonSendableKlass

  init(_ newK: NonSendableKlass) {
    k = NonSendableKlass()

    helper(newK)

    // TODO: This should say actor isolated.
    let _ = { @MainActor in
      print(newK) // expected-error {{sending 'newK' risks causing data races}}
      // expected-note @-1 {{task-isolated 'newK' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    }
  }

  init(x newK: NonSendableKlass) {
    k = newK

    helper(newK)

    let _ = { @MainActor in
      // TODO: Second part should say later 'self'-isolated uses
      print(newK) // expected-error {{sending 'newK' risks causing data races}}
      // expected-note @-1 {{'self'-isolated 'newK' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    }
  }

  init(ns: NonSendableKlass) async {
    self.k = NonSendableKlass()
    self.isolatedHelper(ns)
  }

  nonisolated func helper(_ newK: NonSendableKlass) {}
  func isolatedHelper(_ newK: NonSendableKlass) {}
}

func initActorWithSyncNonIsolatedInit() {
  let k = NonSendableKlass()
  // TODO: This should say actor isolated.
  _ = ActorWithSynchronousNonIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending 'k' to actor-isolated initializer 'init(_:)' risks causing data races between actor-isolated and local nonisolated uses}}
  let _ = { @MainActor in // expected-note {{access can happen concurrently}}
    print(k)
  }
}

func initActorWithSyncNonIsolatedInit2(_ k: NonSendableKlass) {
  // TODO: This should say actor isolated.
  _ = ActorWithSynchronousNonIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'k' to actor-isolated initializer 'init(_:)' risks causing data races between actor-isolated and task-isolated uses}}
  let _ = { @MainActor in
    print(k) // expected-error {{sending 'k' risks causing data races}}
    // expected-note @-1 {{task-isolated 'k' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
}

actor ActorWithAsyncIsolatedInit {
  init(_ newK: NonSendableKlass) async {
    // TODO: This should say actor isolated.
    let _ = { @MainActor in
      print(newK) // expected-error {{sending 'newK' risks causing data races}}
      // expected-note @-1 {{'self'-isolated 'newK' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    }
  }
}

func initActorWithAsyncIsolatedInit() async {
  let k = NonSendableKlass()
  // TODO: This should say actor isolated.
  _ = await ActorWithAsyncIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending 'k' to actor-isolated initializer 'init(_:)' risks causing data races between actor-isolated and local nonisolated uses}}
  let _ = { @MainActor in // expected-note {{access can happen concurrently}}
    print(k)
  }
}

func initActorWithAsyncIsolatedInit2(_ k: NonSendableKlass) async {
  // TODO: This should say actor isolated.
  _ = await ActorWithAsyncIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'k' to actor-isolated initializer 'init(_:)' risks causing data races between actor-isolated and task-isolated uses}}
  let _ = { @MainActor in
    print(k) // expected-error {{sending 'k' risks causing data races}}
    // expected-note @-1 {{task-isolated 'k' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
}

////////////////////////////////
// MARK: Actor Isolated Class //
////////////////////////////////

@CustomActor
class ClassWithSynchronousNonIsolatedInit {
  nonisolated init(_ newK: NonSendableKlass) {
    // We do not error on this since helper is nonisolated and newK is
    // considered task isolated.
    helper(newK)

    let _ = { @MainActor in
      print(newK) // expected-error {{sending 'newK' risks causing data races}}
      // expected-note @-1 {{task-isolated 'newK' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    }
  }

  nonisolated func helper(_ newK: NonSendableKlass) {}
}

func initClassWithSyncNonIsolatedInit() {
  let k = NonSendableKlass()
  _ = ClassWithSynchronousNonIsolatedInit(k)
  let _ = { @MainActor in
    print(k)
  }
}

func initClassWithSyncNonIsolatedInit2(_ k: NonSendableKlass) {
  _ = ClassWithSynchronousNonIsolatedInit(k)
  let _ = { @MainActor in
    print(k) // expected-error {{sending 'k' risks causing data races}}
    // expected-note @-1 {{task-isolated 'k' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
}

@CustomActor
class ClassWithAsyncIsolatedInit {
  init(_ newK: NonSendableKlass) async {
    let _ = { @MainActor in
      print(newK) // expected-error {{sending 'newK' risks causing data races}}
      // expected-note @-1 {{global actor 'CustomActor'-isolated 'newK' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
    }
  }
}

func initClassWithAsyncIsolatedInit() async {
  let k = NonSendableKlass()
  // TODO: Might make sense to emit a more specific error here since the closure
  // is MainActor isolated. The actual capture is initially not isolated to
  // MainActor.
  _ = await ClassWithAsyncIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending 'k' to global actor 'CustomActor'-isolated initializer 'init(_:)' risks causing data races between global actor 'CustomActor'-isolated and local nonisolated uses}}
  let _ = { @MainActor in // expected-note {{access can happen concurrently}}
    print(k)
  }
}

func initClassWithAsyncIsolatedInit2(_ k: NonSendableKlass) async {
  _ = await ClassWithAsyncIsolatedInit(k) // expected-error {{sending 'k' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'k' to global actor 'CustomActor'-isolated initializer 'init(_:)' risks causing data races between global actor 'CustomActor'-isolated and task-isolated uses}}
  let _ = { @MainActor in
    print(k) // expected-error {{sending 'k' risks causing data races}}
    // expected-note @-1 {{task-isolated 'k' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
}

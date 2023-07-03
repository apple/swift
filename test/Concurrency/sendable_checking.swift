// RUN: %target-typecheck-verify-swift -strict-concurrency=targeted
// REQUIRES: concurrency
// REQUIRES: OS=macosx

@available(SwiftStdlib 5.1, *)
struct NS1 { }
// expected-note@-1 2{{consider making struct 'NS1' conform to the 'Sendable' protocol}}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable)
extension NS1: Sendable { }
// expected-note@-1 4{{conformance of 'NS1' to 'Sendable' has been explicitly marked unavailable here}}

@available(SwiftStdlib 5.1, *)
struct NS2 { // expected-note{{consider making struct 'NS2' conform to the 'Sendable' protocol}}
  var ns1: NS1
}

@available(SwiftStdlib 5.1, *)
struct NS3 { }

@available(SwiftStdlib 5.3, *)
extension NS3: Sendable { }

@available(SwiftStdlib 5.1, *)
class NS4 { } // expected-note{{class 'NS4' does not conform to the 'Sendable' protocol}}

@available(SwiftStdlib 5.1, *)
func acceptCV<T: Sendable>(_: T) { }

func acceptSendableFn(_: @Sendable @escaping () -> Void) { }

@available(SwiftStdlib 5.1, *)
func testCV(
  ns1: NS1, ns1array: [NS1], ns2: NS2, ns3: NS3, ns4: NS4,
  fn: @escaping () -> Void
  // expected-note@-1{{parameter 'fn' is implicitly non-sendable}}
) {
  acceptCV(ns1) // expected-warning{{conformance of 'NS1' to 'Sendable' is unavailable}}
  acceptCV(ns1array) // expected-warning{{conformance of 'NS1' to 'Sendable' is unavailable}}
  acceptCV(ns2)
  acceptCV(ns3) // expected-warning{{conformance of 'NS3' to 'Sendable' is only available in macOS 11.0 or newer}}
  // expected-note@-1{{add 'if #available' version check}}
  acceptCV(ns4)
  acceptCV(fn)
  acceptSendableFn(fn) // expected-warning{{passing non-sendable parameter 'fn' to function expecting a @Sendable closure}}
}

@available(SwiftStdlib 5.1, *)
func testCV(
  ns1: NS1, ns1array: [NS1], ns2: NS2, ns3: NS3, ns4: NS4,
  fn: @escaping () -> Void
  // expected-note@-1{{parameter 'fn' is implicitly non-sendable}}
) async {
  acceptCV(ns1) // expected-warning{{conformance of 'NS1' to 'Sendable' is unavailable}}
  acceptCV(ns1array) // expected-warning{{conformance of 'NS1' to 'Sendable' is unavailable}}
  acceptCV(ns2) // expected-warning{{type 'NS2' does not conform to the 'Sendable' protocol}}
  acceptCV(ns3) // expected-warning{{conformance of 'NS3' to 'Sendable' is only available in macOS 11.0 or newer}}
  // expected-note@-1{{add 'if #available' version check}}
  acceptCV(ns4) // expected-warning{{type 'NS4' does not conform to the 'Sendable' protocol}}
  acceptCV(fn) // expected-warning{{type '() -> Void' does not conform to the 'Sendable' protocol}}
  // expected-note@-1{{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  acceptSendableFn(fn) // expected-warning{{passing non-sendable parameter 'fn' to function expecting a @Sendable closure}}
}

// rdar://83942484 - spurious Sendable diagnostics
@available(SwiftStdlib 5.1, *)
public protocol MyProto {
    func foo<F>(aFoo: F) async where F: Sendable
}

@available(SwiftStdlib 5.1, *)
func nonisolatedAsyncFunc1(_: NS1) async { }

@available(SwiftStdlib 5.1, *)
func nonisolatedAsyncFunc2() async -> NS1 { NS1() }

@available(SwiftStdlib 5.1, *)
public actor MyActor: MyProto {
  public func foo<F>(aFoo: F) async where F: Sendable { }
  public func bar<B>(aBar: B) async where B: Sendable { }

  func g(ns1: NS1) async {
    await nonisolatedAsyncFunc1(ns1) // expected-warning{{passing argument of non-sendable type 'NS1' outside of actor-isolated context may introduce data races}}
    _ = await nonisolatedAsyncFunc2() // expected-warning{{non-sendable type 'NS1' returned by implicitly asynchronous call to nonisolated function cannot cross actor boundary}}
  }
}

// rdar://82452688 - make sure sendable checking doesn't fire for a capture
// of a value of error-type
@available(SwiftStdlib 5.1, *)
func f() async {
  let n = wobble() // expected-error{{cannot find 'wobble' in scope}}
  @Sendable func nested() {
    n.pointee += 1
  }
}

// Make sure the generic signature doesn't minimize away Sendable requirements.
@_nonSendable class NSClass { }

struct WrapClass<T: NSClass> {
  var t: T
}

extension WrapClass: Sendable where T: Sendable { }

// Make sure we don't inherit the unavailable Sendable conformance from
// our superclass.
class SendableSubclass: NSClass, @unchecked Sendable { }

@available(SwiftStdlib 5.1, *)
func testSubclassing(obj: SendableSubclass) async {
  acceptCV(obj) // okay!
}


@available(SwiftStdlib 5.1, *)
protocol P {
  func foo (x : @Sendable () -> ()) async -> ()

  func bar(x : () -> ()) async -> ()
  // expected-note@-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  func foo2<T : Sendable>(x : T) async -> ()

  func bar2<T>(x : T) async -> ()
  // expected-note@-1 {{consider making generic parameter 'T' conform to the 'Sendable' protocol}}
}

// Make sure conformance to protocols checks sendability of
// requirement parameters not witness parameters
@available(SwiftStdlib 5.1, *)
        actor A : P {
  func foo(x : () -> ()) -> () {}

  func bar(x : () -> ()) -> () {}
  // expected-warning@-1 {{non-sendable type '() -> ()' in parameter of the protocol requirement satisfied by actor-isolated instance method 'bar(x:)' cannot cross actor boundary}}

  func foo2<T>(x : T) -> () {}

  func bar2<T>(x : T) -> () {}
  // expected-warning@-1 {{non-sendable type 'T' in parameter of the protocol requirement satisfied by actor-isolated instance method 'bar2(x:)' cannot cross actor boundary}}
}

@available(SwiftStdlib 5.1, *)
class Super {
  @MainActor
  func foo (x : @Sendable () -> ()) async {}

  @MainActor
  func bar (x : () -> ()) async {}
  // expected-note@-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  @MainActor
  func foo2<T : Sendable>(x: T) async {}

  @MainActor
  func bar2<T>(x: T) async {}
  // expected-note@-1 {{consider making generic parameter 'T' conform to the 'Sendable' protocol}}
}

// Make sure isolation crossing overrides check sendability
// of superclass parameters not subclass parameters
@available(SwiftStdlib 5.1, *)
class Sub : Super {
  override nonisolated func foo(x : () -> ()) async {}

  override nonisolated func bar(x : () -> ()) async {}
  // expected-warning@-1 {{non-sendable type '() -> ()' in parameter of superclass method overridden by nonisolated instance method 'bar(x:)' cannot cross actor boundary}}

  override nonisolated func foo2<T>(x: T) async {}

  override nonisolated func bar2<T>(x: T) async {}
  // expected-warning@-1 {{non-sendable type 'T' in parameter of superclass method overridden by nonisolated instance method 'bar2(x:)' cannot cross actor boundary}}
}

@available(SwiftStdlib 5.1, *)
class SuperWSafeSubscript {
  @MainActor
  subscript<T : Sendable>(x : T) -> Int {
    get async {
      return 0
    }
  }
}

@available(SwiftStdlib 5.1, *)
class SubWSafeSubscript : SuperWSafeSubscript {
  override nonisolated subscript<T>(x : T) -> Int {
    get async {
      return 0
    }
  }
}

@available(SwiftStdlib 5.1, *)
class SuperWUnsafeSubscript {
  @MainActor
  subscript<T>(x : T) -> Int {
    // expected-note@-1 2{{consider making generic parameter 'T' conform to the 'Sendable' protocol}}
    get async {
      return 0
    }
  }
}

@available(SwiftStdlib 5.1, *)
class SubWUnsafeSubscript : SuperWUnsafeSubscript {
  override nonisolated subscript<T>(x : T) -> Int {
    get async {
      // expected-warning@-2{{non-sendable type 'T' in parameter of superclass method overridden by nonisolated subscript 'subscript(_:)' cannot cross actor boundary}}
      // expected-warning@-2{{non-sendable type 'T' in parameter of superclass method overridden by nonisolated getter '_' cannot cross actor boundary}}
      // there really shouldn't be two warnings produced here, see rdar://110846040 (Sendable diagnostics reported twice for subscript getters)
      return 0
    }
  }
}


// Test implicit conversions getting in the way
@available(SwiftStdlib 5.1, *)
extension MyActor {
  func f(_: Any) { }
  func g(_: () -> Void) { }
}

@available(SwiftStdlib 5.1, *)
func testConversionsAndSendable(a: MyActor, s: any Sendable, f: @Sendable () -> Void) async {
  await a.f(s)
  await a.g(f)
}

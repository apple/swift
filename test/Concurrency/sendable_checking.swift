// RUN: %target-swift-frontend -verify -strict-concurrency=targeted -verify-additional-prefix targeted-and-complete- -emit-sil -o /dev/null %s
// RUN: %target-swift-frontend -verify -strict-concurrency=complete -verify-additional-prefix tns- -verify-additional-prefix complete-and-tns- -emit-sil -o /dev/null %s

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: OS=macosx

@available(SwiftStdlib 5.1, *)
struct NS1 { }
// expected-note @-1 {{consider making struct 'NS1' conform to the 'Sendable' protocol}}
// expected-targeted-and-complete-note @-2 {{consider making struct 'NS1' conform to the 'Sendable' protocol}}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable)
extension NS1: Sendable { }
// expected-note@-1 4{{conformance of 'NS1' to 'Sendable' has been explicitly marked unavailable here}}

@available(SwiftStdlib 5.1, *)
struct NS2 { // expected-note {{consider making struct 'NS2' conform to the 'Sendable' protocol}}
  // expected-complete-and-tns-note @-1 {{consider making struct 'NS2' conform to the 'Sendable' protocol}}
  var ns1: NS1
}

@available(SwiftStdlib 5.1, *)
struct NS3 { }

@available(SwiftStdlib 5.3, *)
extension NS3: Sendable { }

@available(SwiftStdlib 5.1, *)
class NS4 { } // expected-note {{class 'NS4' does not conform to the 'Sendable' protocol}}
// expected-complete-and-tns-note @-1 {{class 'NS4' does not conform to the 'Sendable' protocol}}

@available(SwiftStdlib 5.1, *)
func acceptCV<T: Sendable>(_: T) { }

func acceptSendableFn(_: @Sendable @escaping () -> Void) { }

@available(SwiftStdlib 5.1, *)
func testCV(
  ns1: NS1, ns1array: [NS1], ns2: NS2, ns3: NS3, ns4: NS4,
  fn: @escaping () -> Void
  // expected-note @-1 {{parameter 'fn' is implicitly non-sendable}}
) {
  acceptCV(ns1) // expected-warning {{conformance of 'NS1' to 'Sendable' is unavailable}}

  acceptCV(ns1array) // expected-warning {{conformance of 'NS1' to 'Sendable' is unavailable}}

  acceptCV(ns2) // expected-complete-and-tns-warning {{type 'NS2' does not conform to the 'Sendable' protocol}}

  acceptCV(ns3) // expected-warning {{conformance of 'NS3' to 'Sendable' is only available in macOS 11.0 or newer}}
  // expected-note @-1 {{add 'if #available' version check}}

  acceptCV(ns4) // expected-complete-and-tns-warning {{type 'NS4' does not conform to the 'Sendable' protocol}}

  acceptCV(fn) // expected-complete-and-tns-warning {{type '() -> Void' does not conform to the 'Sendable' protocol}}
  // expected-complete-and-tns-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

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
    await nonisolatedAsyncFunc1(ns1) // expected-targeted-and-complete-warning{{passing argument of non-sendable type 'NS1' outside of actor-isolated context may introduce data races}}
    // expected-tns-warning @-1 {{sending 'ns1' risks causing data races}}
    // expected-tns-note @-2 {{sending 'self'-isolated 'ns1' to nonisolated global function 'nonisolatedAsyncFunc1' risks causing data races between nonisolated and 'self'-isolated uses}}
    _ = await nonisolatedAsyncFunc2() // expected-warning{{non-sendable type 'NS1' returned by implicitly asynchronous call to nonisolated function cannot cross actor boundary}}
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
      // expected-warning@-2{{non-sendable type 'T' in parameter of superclass method overridden by nonisolated getter for subscript 'subscript(_:)' cannot cross actor boundary}}
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

@available(SwiftStdlib 5.1, *)
actor CustomActorInstance {}

@available(SwiftStdlib 5.1, *)
@globalActor
struct CustomActor {
  static let shared = CustomActorInstance()
}

@available(SwiftStdlib 5.1, *)
final class NonSendable {
  // expected-note @-1 5 {{class 'NonSendable' does not conform to the 'Sendable' protocol}}
  // TransferNonSendable emits 3 fewer errors here.
  // expected-targeted-and-complete-note @-3 7 {{class 'NonSendable' does not conform to the 'Sendable' protocol}}
  // expected-complete-and-tns-note @-4 {{class 'NonSendable' does not conform to the 'Sendable' protocol}}
  var value = ""

  @MainActor
  func update() {
    value = "update"
  }

  func call() async {
    await update()
    // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into main actor-isolated context may introduce data races}}
    // expected-tns-warning @-2 {{sending 'self' risks causing data races}}
    // expected-tns-note @-3 {{sending task-isolated 'self' to main actor-isolated instance method 'update()' risks causing data races between main actor-isolated and task-isolated uses}}

    await self.update()
    // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into main actor-isolated context may introduce data races}}
    // expected-tns-warning @-2 {{sending 'self' risks causing data races}}
    // expected-tns-note @-3 {{sending task-isolated 'self' to main actor-isolated instance method 'update()' risks causing data races between main actor-isolated and task-isolated uses}}

    _ = await x
    // expected-warning@-1 {{non-sendable type 'NonSendable' passed in implicitly asynchronous call to main actor-isolated property 'x' cannot cross actor boundary}}

    _ = await self.x
      // expected-warning@-1 {{non-sendable type 'NonSendable' passed in implicitly asynchronous call to main actor-isolated property 'x' cannot cross actor boundary}}
  }

  @MainActor
  var x: Int { 0 }

  @CustomActor
  var y: Int { 0 }

  var z: Int { 0 }
}

// This is not an error since t.update and t.x are both main actor isolated. We
// still get the returning main actor-isolated property 'x' error though.
@available(SwiftStdlib 5.1, *)
func testNonSendableBaseArg() async {
  let t = NonSendable()
  await t.update()
  // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into main actor-isolated context may introduce data races}}

  _ = await t.x
  // expected-warning @-1 {{non-sendable type 'NonSendable' passed in implicitly asynchronous call to main actor-isolated property 'x' cannot cross actor boundary}}
}

// We get the region isolation error here since t.y is custom actor isolated.
@available(SwiftStdlib 5.1, *)
func testNonSendableBaseArg2() async {
  let t = NonSendable()
  await t.update()
  // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into main actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 't' risks causing data races}}
  // TODO: Improve the diagnostic so that we say custom actor isolated instead since t.y
  // is custom actor isolated.
  // expected-tns-note @-5 {{sending 't' to main actor-isolated instance method 'update()' risks causing data races between main actor-isolated and local nonisolated uses}}

  _ = await t.y
  // expected-warning @-1 {{non-sendable type 'NonSendable' passed in implicitly asynchronous call to global actor 'CustomActor'-isolated property 'y' cannot cross actor boundary}}
  // expected-tns-note @-2 {{access can happen concurrently}}
}

// We get the region isolation error here since t.z is not isolated.
@available(SwiftStdlib 5.1, *)
func testNonSendableBaseArg3() async {
  let t = NonSendable()
  await t.update()
  // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into main actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 't' risks causing data races}}
  // expected-tns-note @-3 {{sending 't' to main actor-isolated instance method 'update()' risks causing data races between main actor-isolated and local nonisolated uses}}

  _ = t.z
  // expected-tns-note @-1 {{access can happen concurrently}}
}

@available(SwiftStdlib 5.1, *)
@Sendable
func globalSendable(_ ns: NonSendable) async {}

@available(SwiftStdlib 5.1, *)
@MainActor
func callNonisolatedAsyncClosure(
  ns: NonSendable,
  g: (NonSendable) async -> Void
) async {
  await g(ns)
  // expected-targeted-and-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' outside of main actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'ns' risks causing data races}}
  // expected-tns-note @-3 {{sending main actor-isolated 'ns' to nonisolated callee risks causing data races between nonisolated and main actor-isolated uses}}

  let f: (NonSendable) async -> () = globalSendable // okay
  await f(ns)
  // expected-targeted-and-complete-warning@-1 {{passing argument of non-sendable type 'NonSendable' outside of main actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'ns' risks causing data races}}
  // expected-tns-note @-3 {{sending main actor-isolated 'ns' to nonisolated callee risks causing data races between nonisolated and main actor-isolated uses}}
}

@available(SwiftStdlib 5.1, *)
func testLocalCaptures() {
  let ns = NonSendable()

  @Sendable
  func a2() -> NonSendable {
    return ns
    // expected-complete-and-tns-warning @-1 {{capture of 'ns' with non-sendable type 'NonSendable' in a `@Sendable` local function}}
  }
}

func testPointersAreNotSendable() {
  func testSendable<T: Sendable>(_: T) {}

  func testUnsafePointer(ptr: UnsafePointer<Int>,
                         mutablePtr: UnsafeMutablePointer<String>) {
    testSendable(ptr) // expected-warning {{conformance of 'UnsafePointer<Pointee>' to 'Sendable' is unavailable}}
    testSendable(mutablePtr) // expected-warning {{conformance of 'UnsafeMutablePointer<Pointee>' to 'Sendable' is unavailable}}
  }

  func testRawPointer(ptr: UnsafeRawPointer,
                            mutablePtr: UnsafeMutableRawPointer) {
    testSendable(ptr) // expected-warning {{conformance of 'UnsafeRawPointer' to 'Sendable' is unavailable}}
    testSendable(mutablePtr) // expected-warning {{conformance of 'UnsafeMutableRawPointer' to 'Sendable' is unavailable}}
  }

  func testOpaqueAndCPointers(opaquePtr: OpaquePointer, cPtr: CVaListPointer, autoReleasePtr: AutoreleasingUnsafeMutablePointer<Int>) {
    testSendable(opaquePtr) // expected-warning {{conformance of 'OpaquePointer' to 'Sendable' is unavailable}}
    testSendable(cPtr) // expected-warning {{conformance of 'CVaListPointer' to 'Sendable' is unavailable}}
    testSendable(autoReleasePtr) // expected-warning {{conformance of 'AutoreleasingUnsafeMutablePointer<Pointee>' to 'Sendable' is unavailable}}
  }

  func testBufferPointers(buffer: UnsafeBufferPointer<Int>, mutableBuffer: UnsafeMutableBufferPointer<Int>,
                          rawBuffer: UnsafeRawBufferPointer, rawMutableBuffer: UnsafeMutableRawBufferPointer) {
    testSendable(buffer) // expected-warning {{conformance of 'UnsafeBufferPointer<Element>' to 'Sendable' is unavailable}}
    testSendable(mutableBuffer) // expected-warning {{conformance of 'UnsafeMutableBufferPointer<Element>' to 'Sendable' is unavailable}}
    testSendable(buffer.makeIterator()) // expected-warning {{conformance of 'UnsafeBufferPointer<Element>.Iterator' to 'Sendable' is unavailable}}
    testSendable(rawBuffer) // expected-warning {{conformance of 'UnsafeRawBufferPointer' to 'Sendable' is unavailable}}
    testSendable(rawBuffer.makeIterator()) // expected-warning {{conformance of 'UnsafeRawBufferPointer.Iterator' to 'Sendable' is unavailable}}
    testSendable(rawMutableBuffer) // expected-warning {{conformance of 'UnsafeMutableRawBufferPointer' to 'Sendable' is unavailable}}
    testSendable(rawMutableBuffer.makeIterator()) // expected-warning {{conformance of 'UnsafeRawBufferPointer.Iterator' to 'Sendable' is unavailable}}
  }
}

@available(*, unavailable)
extension SynthesizedConformances.NotSendable: Sendable {}

enum SynthesizedConformances {
  struct NotSendable: Equatable {}

  // expected-warning@+2 2{{main actor-isolated property 'x' can not be referenced from a non-isolated context}}
  // expected-note@+1 2{{in static method '==' for derived conformance to 'Equatable'}}
  @MainActor struct Isolated: Equatable {
    let x: NotSendable // expected-note 2{{property declared here}}
  }
}

@available(SwiftStdlib 5.1, *)
final class UseNonisolatedUnsafe: Sendable {
  nonisolated(unsafe) var x1: NonSendable = .init()
  nonisolated(unsafe) let x2: NonSendable = .init()
  nonisolated(unsafe) var x3: Int = 0

  func captureInTask() {
    nonisolated(unsafe) var x = NonSendable()
    Task {
      print(x)
      x = NonSendable()
    }
  }
}

@available(SwiftStdlib 5.1, *)
@preconcurrency
func preconcurrencyContext(_: @escaping @Sendable () -> Void) {}

@available(SwiftStdlib 5.1, *)
@MainActor
struct DowngradeForPreconcurrency {
  func capture(completion: @escaping @MainActor () -> Void) {
    preconcurrencyContext {
      Task {
        completion()
        // expected-warning@-1 {{capture of 'completion' with non-sendable type '@MainActor () -> Void' in a `@Sendable` closure}}
        // expected-warning@-2 {{capture of 'completion' with non-sendable type '@MainActor () -> Void' in an isolated closure}}
        // expected-note@-3 2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
        // expected-warning@-4 {{expression is 'async' but is not marked with 'await'; this is an error in the Swift 6 language mode}}
        // expected-note@-5 {{calls to parameter 'completion' from outside of its actor context are implicitly asynchronous}}
      }
    }
  }

  var x: NonSendable
  func createStream() -> AsyncStream<NonSendable> {
    AsyncStream<NonSendable> {
      self.x
      // expected-warning@-1 {{expression is 'async' but is not marked with 'await'; this is an error in the Swift 6 language mode}}
      // expected-note@-2 {{property access is 'async'}}
      // expected-warning@-3 {{non-sendable type 'NonSendable' in implicitly asynchronous access to main actor-isolated property 'x' cannot cross actor boundary; this is an error in the Swift 6 language mode}}
    }
  }
}

@available(SwiftStdlib 5.1, *)
@MainActor protocol InferMainActor {}

@available(SwiftStdlib 5.1, *)
struct ImplicitSendableViaMain: InferMainActor {}

@available(SwiftStdlib 5.1, *)
extension ImplicitSendableViaMain {
  nonisolated func capture() {
    Task { @MainActor in
      _ = self
    }
  }
}

@available(SwiftStdlib 5.1, *)
struct TestImplicitSendable: Sendable {
  var x: ImplicitSendableViaMain
}

struct UnavailableSendable {}

@available(*, unavailable)
extension UnavailableSendable: Sendable {}
// expected-note@-1 {{conformance of 'UnavailableSendable' to 'Sendable' has been explicitly marked unavailable here}}

@available(SwiftStdlib 5.1, *)
func checkOpaqueType() -> some Sendable {
  UnavailableSendable()
  // expected-warning@-1 {{conformance of 'UnavailableSendable' to 'Sendable' is unavailable; this is an error in the Swift 6 language mode}}
}

// rdar://129024926

@available(SwiftStdlib 5.1, *)
@MainActor class MainActorSuper<T: Sendable> {}

@available(SwiftStdlib 5.1, *)
class MainActorSub: MainActorSuper<MainActorSub.Nested> {
  struct Nested {}  // no cycle
}

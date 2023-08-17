// RUN: %target-typecheck-verify-swift  -disable-availability-checking -warn-concurrency
// REQUIRES: concurrency

@available(SwiftStdlib 5.1, *)
actor A {
  func f() { } // expected-note 5{{calls to instance method 'f()' from outside of its actor context are implicitly asynchronous}}
}

@available(SwiftStdlib 5.1, *)
extension Actor {
  func g() { }
}

@MainActor func mainActorFn() {}

@available(SwiftStdlib 5.1, *)
func testA<T: Actor>(
  a: isolated A,  // expected-note{{previous 'isolated' parameter 'a'}}
  b: isolated T,  // expected-warning{{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
  c: isolated Int // expected-error {{'isolated' parameter has non-actor type 'Int'}}
) {
  a.f()
  a.g()
  b.g()
}

actor Other {
  func inc() {}
  func pass(_ f: @Sendable  (Self) async -> Void) {}
}
actor Counter {
  func inc() {}
  func pass(_ f: @Sendable  (Self) async -> Void) {}
}

@available(SwiftStdlib 5.1, *)
// expected-note@+2 {{previous 'isolated' parameter 'a'}}
// expected-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
func testDoubleIsolatedParams(a: isolated Counter, b: isolated Other) async {
  a.inc()
  b.inc()
}

func taaaa() async {
  await Counter().pass { isoC in
    await Other().pass { isoO in
      await testDoubleIsolatedParams(a: isoC, b: isoO)
    }
  }
}

@available(SwiftStdlib 5.1, *)
typealias Fn = (isolated A) -> Void

@available(SwiftStdlib 5.1, *)
func globalFunc(_ a: A) { }

@available(SwiftStdlib 5.1, *)
func globalFuncIsolated(_: isolated A) { // expected-note{{calls to global function 'globalFuncIsolated' from outside of its actor context are implicitly asynchronous}}
  let _: Int = globalFuncIsolated // expected-error{{cannot convert value of type '(isolated A) -> ()' to specified type 'Int'}}
  let _: (A) -> Void = globalFuncIsolated // expected-error{{cannot convert value of type '(isolated A) -> ()' to specified type '(A) -> Void'}}
  let _: Fn = globalFunc // okay
}

@available(SwiftStdlib 5.1, *)
// expected-warning@+1 {{global function with 'isolated' parameter cannot have a global actor; this is an error in Swift 6}}{{1-12=}}
@MainActor func testIsolatedParamCalls(a: isolated A, b: A) {
  globalFunc(a)
  globalFunc(b)

  globalFuncIsolated(a)
  globalFuncIsolated(b) // expected-error{{call to actor-isolated global function 'globalFuncIsolated' in a synchronous actor-isolated context}}
}

@available(SwiftStdlib 5.1, *)
func testIsolatedParamCallsAsync(a: isolated A, b: A) async {
  globalFunc(a)
  globalFunc(b)

  globalFuncIsolated(a)
  globalFuncIsolated(b) // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{calls to global function 'globalFuncIsolated' from outside of its actor context are implicitly asynchronous}}
  await globalFuncIsolated(b)
}

@available(SwiftStdlib 5.1, *)
func testIsolatedParamCaptures(a: isolated A) async {
  let _ = { @MainActor in
    a.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous main actor-isolated context}}
  }

  let _: @MainActor () -> () = {
    a.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous main actor-isolated context}}
  }

  let _ = {
    a.f()
  }

  let _ = { @Sendable in
    a.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous nonisolated context}}
  }

}

actor MyActor {
  func hello() {} // expected-note {{calls to instance method 'hello()' from outside of its actor context are implicitly asynchronous}}
}

typealias MyFn = (isolated: Int) -> Void // expected-error {{function types cannot have argument labels; use '_' before 'isolated'}}
typealias MyFnFixed = (_: isolated MyActor) -> Void

func standalone(_: isolated MyActor) {}

func check() {
  let _: MyFnFixed = standalone
  let _: MyFnFixed = { (_: isolated MyActor) in () }
}


@available(SwiftStdlib 5.1, *)
protocol P {
  func f(isolated: MyActor) async
  func g(isolated x: MyActor) async
  func h(isolated MyActor: isolated MyActor)
  func i(isolated: isolated MyActor)
  func j(isolated: Int) -> Int
  func k(isolated y: Int) -> Int
  func l(isolated _: Int) -> Int
  func m(thing: isolated MyActor)
}

@available(SwiftStdlib 5.1, *)
struct S: P {
  func f(isolated: MyActor) async { await isolated.hello() }
  func g(isolated x: MyActor) async { await x.hello() }
  func h(isolated MyActor: isolated MyActor) { i(isolated: MyActor) }
  func i(isolated: isolated MyActor) { isolated.hello() }
  func j(isolated: Int) -> Int { return isolated }
  func k(isolated y: Int) -> Int { return j(isolated: y) }
  func l(isolated _: Int) -> Int { return k(isolated: 0) }
  func m(thing: MyActor) { thing.hello() } // expected-error {{call to actor-isolated instance method 'hello()' in a synchronous nonisolated context}}
}

func checkConformer(_ s: S, _ p: any P, _ ma: MyActor) async {
  s.m(thing: ma)
  await p.m(thing: ma)
  // expected-warning@-1 {{passing argument of non-sendable type 'any P' into actor-isolated context may introduce data races}}
}


// Redeclaration checking
actor TestActor {
  func test() { // expected-note{{'test()' previously declared here}}
  }
  nonisolated func test() { // expected-error{{invalid redeclaration of 'test()'}}
  }
}

func redecl(_: TestActor) { } // expected-note{{'redecl' previously declared here}}
func redecl(_: isolated TestActor) { } // expected-error{{invalid redeclaration of 'redecl'}}

func tuplify<Ts>(_ fn: (Ts) -> Void) {} // expected-note {{in call to function 'tuplify'}}

@available(SwiftStdlib 5.1, *)
func testTuplingIsolated(
                         _ a: isolated A, // expected-note {{previous 'isolated' parameter 'a'}}
                         _ b: isolated A  // expected-warning {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
                        ) {
  tuplify(testTuplingIsolated)
  // expected-error@-1 {{generic parameter 'Ts' could not be inferred}}
  // expected-error@-2 {{cannot convert value of type '(isolated A, isolated A) -> ()' to expected argument type '(Ts) -> Void'}}
}

// Inference of "isolated" on closure parameters.
@available(SwiftStdlib 5.1, *)
func testIsolatedClosureInference(one: A, two: A) async {
  let _: (isolated A) -> Void = {
    $0.f()
  }

  let _: (isolated A) -> Void = {
    $0.f()
  }

  let _: (isolated A) -> Void = { a2 in
    a2.f()
  }

  let _: (isolated A) -> Void = { (a2: isolated A) in
    a2.f()
  }

  let f: (isolated A, isolated A) -> Void = // expected-warning {{function type cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
      // expected-note@+2 {{previous 'isolated' parameter 'a1'}}
      // expected-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
      { (a1, a2) in
        a1.f()
        a2.f()
      }

  await f(one, two)

  let g: (isolated A, isolated A) -> Void = // expected-warning {{function type cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
      // expected-note@+2 {{previous 'isolated' parameter '$0'}}
      // expected-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
      {
        $0.f()
        $1.f()
      }

  await g(one, two)
}

struct CheckIsolatedFunctionTypes {
  // expected-warning@+2 {{function type cannot have global actor and 'isolated' parameter; this is an error in Swift 6}}
  // expected-warning@+1 {{function type cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
  func a(_ f: @MainActor @Sendable (isolated A, isolated A) -> ()) {}

  // expected-note@+2 {{calls to parameter 'callback' from outside of its actor context are implicitly asynchronous}}
  // expected-warning@+1 {{function type cannot have global actor and 'isolated' parameter; this is an error in Swift 6}}
  @MainActor func update<R>(_ callback: @Sendable @escaping @MainActor (isolated A) -> R) -> R {
    callback(A()) // expected-error {{call to actor-isolated parameter 'callback' in a synchronous main actor-isolated context}}
  }
}

@available(SwiftStdlib 5.1, *)
func checkIsolatedAndGlobalClosures(_ a: A) {
  let _: @MainActor (isolated A) -> Void // expected-warning {{function type cannot have global actor and 'isolated' parameter; this is an error in Swift 6}}
      = {
    $0.f()
    mainActorFn()
  }

  let _: @MainActor (isolated A) -> Void // expected-warning {{function type cannot have global actor and 'isolated' parameter; this is an error in Swift 6}}
      = { @MainActor in // expected-warning {{closure with 'isolated' parameter '$0' cannot have a global actor; this is an error in Swift 6}}{{11-22=}}
    $0.f()
    mainActorFn()
  }

  let _ = { @MainActor (a: isolated A, // expected-warning {{closure with 'isolated' parameter 'a' cannot have a global actor; this is an error in Swift 6}}{{13-24=}}
                                       // expected-note@-1 {{previous 'isolated' parameter 'a'}}
                        b: isolated A, // expected-warning {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
                        c: isolated A) async in
    a.f()
    mainActorFn()
  }
}

// "isolated" existential parameters.
protocol P2: Actor {
  func m()
}

@available(SwiftStdlib 5.1, *)
func testExistentialIsolated(a: isolated P2, b: P2) async {
  a.m()
  await b.m()
  b.m() // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{calls to instance method 'm()' from outside of its actor context are implicitly asynchronous}}
}

// "isolated" parameters of closures make the closure itself isolated.
extension TestActor {
  func isolatedMethod() { }
  // expected-note@-1{{calls to instance method 'isolatedMethod()' from outside of its actor context are implicitly asynchronous}}

  // expected-warning@+1 {{instance method with 'isolated' parameter cannot be 'nonisolated'; this is an error in Swift 6}}{{3-15=}}
  nonisolated func isolatedToParameter(_ other: isolated TestActor) {
    isolatedMethod()
    // expected-error@-1{{call to actor-isolated instance method 'isolatedMethod()' in a synchronous actor-isolated context}}

    other.isolatedMethod()
  }
}

@available(SwiftStdlib 5.1, *)
func isolatedClosures() {
  let _: (isolated TestActor) -> Void = { (ta: isolated TestActor) in
    ta.isolatedMethod() // okay, isolated to ta

    _ = {
      ta.isolatedMethod() // okay, isolated to ta
    }
  }
}

// expected-warning@+2 {{global function with 'isolated' parameter cannot be 'nonisolated'; this is an error in Swift 6}}{{12-24=}}
// expected-warning@+1 {{global function with 'isolated' parameter cannot have a global actor; this is an error in Swift 6}}{{1-12=}}
@MainActor nonisolated func allOfEm(_ a: isolated A) {
  a.f()
}

@MainActor class MAClass {

  // expected-note@+2 {{previous 'isolated' parameter 'a'}}
  // expected-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
  init(_ a: isolated A, _ b: isolated A) {
    // FIXME: wrong isolation. should be isolated to `a` only!
    a.f()
    b.f()
  }

  // expected-note@+3 {{previous 'isolated' parameter 'a'}}
  // expected-warning@+2 {{cannot have more than one 'isolated' parameter; this is an error in Swift 6}}
  // expected-warning@+1 {{subscript with 'isolated' parameter cannot be 'nonisolated'; this is an error in Swift 6}}{{3-15=}}
  nonisolated subscript(_ a: isolated A, _ b: isolated A) -> Int {
    // FIXME: wrong isolation. should be isolated to `a`.
    a.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous actor-isolated context}}
    b.f() // expected-error {{call to actor-isolated instance method 'f()' in a synchronous actor-isolated context}}
    return 0
  }

  // expected-warning@+1 {{instance method with 'isolated' parameter cannot be 'nonisolated'; this is an error in Swift 6}}{{3-15=}}
  nonisolated func millionDollars(_ a: isolated A) {
    a.f()
  }
}

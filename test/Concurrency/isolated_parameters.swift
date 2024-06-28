// RUN: %target-swift-frontend  -disable-availability-checking -strict-concurrency=complete %s -emit-sil -o /dev/null -verify -DALLOW_TYPECHECKER_ERRORS -verify-additional-prefix typechecker-

// RUN: %target-swift-frontend  -disable-availability-checking -strict-concurrency=complete %s -emit-sil -o /dev/null -verify -verify-additional-prefix tns-

// REQUIRES: asserts
// REQUIRES: concurrency
// REQUIRES: swift_swift_parser

@available(SwiftStdlib 5.1, *)
actor A {
  func f() { } // expected-typechecker-note 5{{calls to instance method 'f()' from outside of its actor context are implicitly asynchronous}}
}

@available(SwiftStdlib 5.1, *)
extension Actor {
  func g() { }
}

@MainActor func mainActorFn() {} // expected-typechecker-note {{calls to global function 'mainActorFn()' from outside of its actor context are implicitly asynchronous}}

#if ALLOW_TYPECHECKER_ERRORS

@available(SwiftStdlib 5.1, *)
func testA<T: Actor>(
  a: isolated A,  // expected-typechecker-note {{previous 'isolated' parameter 'a'}}
  b: isolated T,  // expected-typechecker-warning {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
  c: isolated Int // expected-typechecker-error {{'isolated' parameter type 'Int' does not conform to 'Actor' or 'DistributedActor'}}
) {
  a.f()
  a.g()
  b.g()
}

#endif

actor Other {
  func inc() {}
  func pass(_ f: @Sendable  (Self) async -> Void) {}
}
actor Counter {
  func inc() {}
  func pass(_ f: @Sendable  (Self) async -> Void) {}
}

#if ALLOW_TYPECHECKER_ERRORS

// This is illegal at the SIL level so even though we could ignore this
// note/warning, we will get a crash otherwise.
@available(SwiftStdlib 5.1, *)
// expected-typechecker-note@+2 {{previous 'isolated' parameter 'a'}}
// expected-typechecker-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
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

#endif

@available(SwiftStdlib 5.1, *)
typealias Fn = (isolated A) -> Void

@available(SwiftStdlib 5.1, *)
func globalFunc(_ a: A) { }

#if ALLOW_TYPECHECKER_ERRORS

@available(SwiftStdlib 5.1, *)
func globalFuncIsolated(_: isolated A) { // expected-typechecker-note{{calls to global function 'globalFuncIsolated' from outside of its actor context are implicitly asynchronous}}
  let _: Int = globalFuncIsolated // expected-typechecker-error{{cannot convert value of type '(isolated A) -> ()' to specified type 'Int'}}
  let _: (A) -> Void = globalFuncIsolated // expected-typechecker-error{{cannot convert value of type '(isolated A) -> ()' to specified type '(A) -> Void'}}
  let _: Fn = globalFunc // okay
}

@available(SwiftStdlib 5.1, *)
// expected-typechecker-warning@+1 {{global function with 'isolated' parameter cannot have a global actor; this is an error in the Swift 6 language mode}}{{1-12=}}
@MainActor func testIsolatedParamCalls(a: isolated A, b: A) {
  globalFunc(a)
  globalFunc(b)

  globalFuncIsolated(a)
  globalFuncIsolated(b) // expected-typechecker-error{{call to actor-isolated global function 'globalFuncIsolated' in a synchronous actor-isolated context}}
}

#endif

@available(SwiftStdlib 5.1, *)
func testIsolatedParamCallsAsync(a: isolated A, b: A) async {
  globalFunc(a)
  globalFunc(b)

  #if ALLOW_TYPECHECKER_ERRORS
  globalFuncIsolated(a)
  globalFuncIsolated(b) // expected-typechecker-error{{expression is 'async' but is not marked with 'await'}}
  // expected-typechecker-note@-1{{calls to global function 'globalFuncIsolated' from outside of its actor context are implicitly asynchronous}}
  await globalFuncIsolated(b)
  #endif
}

@available(SwiftStdlib 5.1, *)
func testIsolatedParamCaptures(a: isolated A) async {
#if ALLOW_TYPECHECKER_ERRORS
  let _ = { @MainActor in
    a.f() // expected-typechecker-error {{call to actor-isolated instance method 'f()' in a synchronous main actor-isolated context}}
  }

  let _: @MainActor () -> () = {
    a.f() // expected-typechecker-error {{call to actor-isolated instance method 'f()' in a synchronous main actor-isolated context}}
  }
#endif

  let _ = {
    a.f()
  }

#if ALLOW_TYPECHECKER_ERRORS
  let _ = { @Sendable in
    a.f() // expected-typechecker-error {{call to actor-isolated instance method 'f()' in a synchronous nonisolated context}}
  }
#endif
}

actor MyActor {
  func hello() {} // expected-typechecker-note {{calls to instance method 'hello()' from outside of its actor context are implicitly asynchronous}}
}

// Compiler >= 5.3 is eneded to suppress the parser error
#if compiler(>=5.3) && ALLOW_TYPECHECKER_ERRORS
typealias MyFn = (isolated: Int) -> Void // expected-typechecker-error {{function types cannot have argument labels; use '_' before 'isolated'}}
#endif
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
  #if ALLOW_TYPECHECKER_ERRORS
  func m(thing: MyActor) { thing.hello() } // expected-typechecker-error {{call to actor-isolated instance method 'hello()' in a synchronous nonisolated context}}
  #else
  func m(thing: MyActor) { }
  #endif
}

func checkConformer(_ s: S, _ p: any P, _ ma: MyActor) async {
  s.m(thing: ma)
  await p.m(thing: ma)
  // expected-complete-warning@-1 {{passing argument of non-sendable type 'any P' into actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'p' risks causing data races}}
  // expected-tns-note @-3 {{sending task-isolated 'p' to actor-isolated instance method 'm(thing:)' risks causing data races between actor-isolated and task-isolated uses}}
}

// Redeclaration checking
actor TestActor {
  func test() { // expected-typechecker-note{{'test()' previously declared here}}
  }
#if ALLOW_TYPECHECKER_ERRORS
  nonisolated func test() { // expected-typechecker-error{{invalid redeclaration of 'test()'}}
  }
#endif
}

#if ALLOW_TYPECHECKER_ERRORS

func redecl(_: TestActor) { } // expected-typechecker-note{{'redecl' previously declared here}}
func redecl(_: isolated TestActor) { } // expected-typechecker-error{{invalid redeclaration of 'redecl'}}

#endif

func tuplify<Ts>(_ fn: (Ts) -> Void) {} // expected-typechecker-note {{in call to function 'tuplify'}}

#if ALLOW_TYPECHECKER_ERRORS

@available(SwiftStdlib 5.1, *)
func testTuplingIsolated(
                         _ a: isolated A, // expected-typechecker-note {{previous 'isolated' parameter 'a'}}
                         _ b: isolated A  // expected-typechecker-warning {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
                        ) {
  tuplify(testTuplingIsolated)
  // expected-typechecker-error@-1 {{generic parameter 'Ts' could not be inferred}}
  // expected-typechecker-error@-2 {{cannot convert value of type '(isolated A, isolated A) -> ()' to expected argument type '(Ts) -> Void'}}
}

#endif

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

#if ALLOW_TYPECHECKER_ERRORS
  let f: (isolated A, isolated A) -> Void = // expected-typechecker-warning {{function type cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
      // expected-typechecker-note@+2 {{previous 'isolated' parameter 'a1'}}
      // expected-typechecker-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
      { (a1, a2) in
        a1.f()
        a2.f()
      }

  await f(one, two)

  let g: (isolated A, isolated A) -> Void = // expected-typechecker-warning {{function type cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
      // expected-typechecker-note@+2 {{previous 'isolated' parameter '$0'}}
      // expected-typechecker-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
      {
        $0.f()
        $1.f()
      }

  await g(one, two)
  #endif
}

#if ALLOW_TYPECHECKER_ERRORS

struct CheckIsolatedFunctionTypes {
  // expected-typechecker-warning@+2 {{function type cannot have global actor and 'isolated' parameter; this is an error in the Swift 6 language mode}}
  // expected-typechecker-warning@+1 {{function type cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
  func a(_ f: @MainActor @Sendable (isolated A, isolated A) -> ()) {}

  // expected-typechecker-note@+2 {{calls to parameter 'callback' from outside of its actor context are implicitly asynchronous}}
  // expected-typechecker-warning@+1 {{function type cannot have global actor and 'isolated' parameter; this is an error in the Swift 6 language mode}}
  @MainActor func update<R>(_ callback: @Sendable @escaping @MainActor (isolated A) -> R) -> R {
    callback(A()) // expected-typechecker-error {{call to actor-isolated parameter 'callback' in a synchronous main actor-isolated context}}
  }
}

#endif

@available(SwiftStdlib 5.1, *)
func checkIsolatedAndGlobalClosures(_ a: A) {
  let _: @MainActor (isolated A) -> Void // expected-warning {{function type cannot have global actor and 'isolated' parameter; this is an error in the Swift 6 language mode}}
      = {
    $0.f()
    #if ALLOW_TYPECHECKER_ERRORS
    mainActorFn() // expected-typechecker-error {{call to main actor-isolated global function 'mainActorFn()' in a synchronous actor-isolated context}}
    #endif
  }

  let _: @MainActor (isolated A) -> Void // expected-warning {{function type cannot have global actor and 'isolated' parameter; this is an error in the Swift 6 language mode}}
      = { @MainActor in // expected-warning {{closure with 'isolated' parameter '$0' cannot have a global actor; this is an error in the Swift 6 language mode}}{{11-22=}}
    $0.f()
    mainActorFn()
  }

#if ALLOW_TYPECHECKER_ERRORS
  let _ = { @MainActor (a: isolated A, // expected-typechecker-warning {{closure with 'isolated' parameter 'a' cannot have a global actor; this is an error in the Swift 6 language mode}}{{13-24=}}
                                       // expected-typechecker-note@-1 {{previous 'isolated' parameter 'a'}}
                        b: isolated A, // expected-typechecker-warning {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
                        c: isolated A) async in
    a.f()
    mainActorFn()
  }
#endif
}

// "isolated" existential parameters.
protocol P2: Actor {
  func m()
}

@available(SwiftStdlib 5.1, *)
func testExistentialIsolated(a: isolated P2, b: P2) async {
  a.m()
  await b.m()
  #if ALLOW_TYPECHECKER_ERRORS
  b.m() // expected-typechecker-error{{expression is 'async' but is not marked with 'await'}}
  // expected-typechecker-note@-1{{calls to instance method 'm()' from outside of its actor context are implicitly asynchronous}}
  #endif
}

// "isolated" parameters of closures make the closure itself isolated.
extension TestActor {
  func isolatedMethod() { }
  // expected-typechecker-note@-1{{calls to instance method 'isolatedMethod()' from outside of its actor context are implicitly asynchronous}}

  // expected-warning@+1 {{instance method with 'isolated' parameter cannot be 'nonisolated'; this is an error in the Swift 6 language mode}}{{3-15=}}
  nonisolated func isolatedToParameter(_ other: isolated TestActor) {
    #if ALLOW_TYPECHECKER_ERRORS
    isolatedMethod()
    // expected-typechecker-error@-1{{call to actor-isolated instance method 'isolatedMethod()' in a synchronous actor-isolated context}}
    #endif

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

// expected-warning@+2 {{global function with 'isolated' parameter cannot be 'nonisolated'; this is an error in the Swift 6 language mode}}{{12-24=}}
// expected-warning@+1 {{global function with 'isolated' parameter cannot have a global actor; this is an error in the Swift 6 language mode}}{{1-12=}}
@MainActor nonisolated func allOfEm(_ a: isolated A) {
  a.f()
}

#if ALLOW_TYPECHECKER_ERRORS

@MainActor class MAClass {

  // expected-typechecker-note@+2 {{previous 'isolated' parameter 'a'}}
  // expected-typechecker-warning@+1 {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
  init(_ a: isolated A, _ b: isolated A) {
    // FIXME: wrong isolation. should be isolated to `a` only!
    a.f()
    b.f()
  }

  // expected-typechecker-note@+3 {{previous 'isolated' parameter 'a'}}
  // expected-typechecker-warning@+2 {{cannot have more than one 'isolated' parameter; this is an error in the Swift 6 language mode}}
  // expected-typechecker-warning@+1 {{subscript with 'isolated' parameter cannot be 'nonisolated'; this is an error in the Swift 6 language mode}}{{3-15=}}
  nonisolated subscript(_ a: isolated A, _ b: isolated A) -> Int {
    // FIXME: wrong isolation. should be isolated to `a`.
    #if ALLOW_TYPECHECKER_ERRORS
    a.f() // expected-typechecker-error {{call to actor-isolated instance method 'f()' in a synchronous actor-isolated context}}
    b.f() // expected-typechecker-error {{call to actor-isolated instance method 'f()' in a synchronous actor-isolated context}}
    #endif
    return 0
  }

  // expected-typechecker-warning@+1 {{instance method with 'isolated' parameter cannot be 'nonisolated'; this is an error in the Swift 6 language mode}}{{3-15=}}
  nonisolated func millionDollars(_ a: isolated A) {
    a.f()
  }
}

#endif

// Test case for https://github.com/apple/swift/issues/62568
func execute<ActorType: Actor>(
  on isolatedActor: isolated ActorType,
  task: @escaping @Sendable (isolated ActorType) -> Void)
{
  // Compiler correctly allows this task to execute synchronously.
  task(isolatedActor)
  // Start a task that inherits the current execution context (i.e. that of the isolatedActor)
  Task {
    // 'await' is not not necessary because 'task' is synchronous.
    task(isolatedActor)
  }
}

actor ProtectsDictionary {
  var dictionary: [String: String] = ["A": "B"]
}

func getValues(
  forKeys keys: [String],
  from actor: isolated ProtectsDictionary
) -> [String?] {
  // A non-escaping, synchronous closure cannot cross isolation
  // boundaries; it should be isolated to 'actor'.
  keys.map { key in
    actor.dictionary[key]
  }
}

#if ALLOW_TYPECHECKER_ERRORS

func isolated_generic_bad_1<T>(_ t: isolated T) {}
// expected-typechecker-error@-1 {{'isolated' parameter type 'T' does not conform to 'Actor' or 'DistributedActor'}}
func isolated_generic_bad_2<T: Equatable>(_ t: isolated T) {}
// expected-typechecker-error@-1 {{'isolated' parameter type 'T' does not conform to 'Actor' or 'DistributedActor'}}
func isolated_generic_bad_3<T: AnyActor>(_ t: isolated T) {}
// expected-typechecker-error@-1 {{'isolated' parameter type 'T' does not conform to 'Actor' or 'DistributedActor'}}
// expected-typechecker-warning@-2 {{'AnyActor' is deprecated: Use 'any Actor' with 'DistributedActor.asLocalActor' instead}}

func isolated_generic_bad_4<T>(_ t: isolated Array<T>) {}
// expected-typechecker-error@-1 {{'isolated' parameter type 'Array<T>' does not conform to 'Actor' or 'DistributedActor'}}

#endif

func isolated_generic_ok_1<T: Actor>(_ t: isolated T) {}


class NotSendable {} // expected-complete-note 5 {{class 'NotSendable' does not conform to the 'Sendable' protocol}}

func optionalIsolated(_ ns: NotSendable, to actor: isolated (any Actor)?) async {}
func optionalIsolatedSync(_ ns: NotSendable, to actor: isolated (any Actor)?) {}

nonisolated func callFromNonisolated(ns: NotSendable) async {
  await optionalIsolated(ns, to: nil)

  optionalIsolatedSync(ns, to: nil)

  let myActor = A()

  await optionalIsolated(ns, to: myActor)
  // expected-complete-warning@-1 {{passing argument of non-sendable type 'NotSendable' into actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{pattern that the region based isolation checker does not understand how to check. Please file a bug}}

#if ALLOW_TYPECHECKER_ERRORS
  optionalIsolatedSync(ns, to: myActor)
  // expected-typechecker-error@-1 {{expression is 'async' but is not marked with 'await'}}
  // expected-typechecker-note@-2 {{calls to global function 'optionalIsolatedSync(_:to:)' from outside of its actor context are implicitly asynchronous}}
  // expected-complete-warning@-3 {{passing argument of non-sendable type 'NotSendable' into actor-isolated context may introduce data races}}
  #endif
}

@MainActor func callFromMainActor(ns: NotSendable) async {
  await optionalIsolated(ns, to: nil)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NotSendable' outside of main actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'ns' risks causing data races}}
  // expected-tns-note @-3 {{sending main actor-isolated 'ns' to nonisolated global function 'optionalIsolated(_:to:)' risks causing data races between nonisolated and main actor-isolated uses}}

  optionalIsolatedSync(ns, to: nil) // expected-tns-warning {{pattern that the region based isolation checker does not understand how to check. Please file a bug}}

  let myActor = A()

  await optionalIsolated(ns, to: myActor)
  // expected-complete-warning@-1 {{passing argument of non-sendable type 'NotSendable' into actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{pattern that the region based isolation checker does not understand how to check. Please file a bug}}

#if ALLOW_TYPECHECKER_ERRORS
  optionalIsolatedSync(ns, to: myActor)
  // expected-typechecker-error@-1 {{expression is 'async' but is not marked with 'await'}}
  // expected-typechecker-note@-2 {{calls to global function 'optionalIsolatedSync(_:to:)' from outside of its actor context are implicitly asynchronous}}
  // expected-complete-warning@-3 {{passing argument of non-sendable type 'NotSendable' into actor-isolated context may introduce data races}}
#endif
}

// TODO: Consider making an actor's Self behave like in a struct, removing this special casing.
//       We could consider changing this, so that self is always Self because we don't allow inheritance of actors.
//       See: https://github.com/apple/swift/issues/70954 and rdar://121091417
actor A2 {
  nonisolated func f1() async {
#if ALLOW_TYPECHECKER_ERRORS
    await { (self: isolated Self) in }(self)
    // expected-typechecker-error@-1 {{cannot convert value of type 'A2' to expected argument type 'Self'}}
    await { (self: isolated Self?) in }(self)
    // expected-typechecker-error@-1 {{cannot convert value of type 'A2' to expected argument type 'Self?'}}
#endif
  }
  nonisolated func f2() async -> Self {
    await { (self: isolated Self) in }(self)
    await { (self: isolated Self?) in }(self)
    return self
  }
}

func testNonSendableCaptures(ns: NotSendable, a: isolated MyActor) {
  Task {
    _ = a
    _ = ns
  }

  // FIXME: The `a` in the capture list and `isolated a` are the same,
  // but the actor isolation checker doesn't know that.
  Task { [a] in // expected-tns-warning {{'a'-isolated value of type '() async -> ()' passed as a strongly transferred parameter}}
    _ = a
    _ = ns
  }
}


@globalActor actor MyGlobal {
  static let shared = MyGlobal()
}

func sync(isolatedTo actor: isolated (any Actor)?) {}

func pass(value: NotSendable, isolation: isolated (any Actor)?) async -> NotSendable {
  value
}

func preciseIsolated(a: isolated MyActor) async {
  sync(isolatedTo: a)
  sync(isolatedTo: nil) // okay from anywhere
  sync(isolatedTo: #isolation)

  Task { @MainActor in
    sync(isolatedTo: MainActor.shared)
    sync(isolatedTo: nil) // okay from anywhere
    sync(isolatedTo: #isolation)
  }

  Task { @MyGlobal in
    sync(isolatedTo: MyGlobal.shared)
    sync(isolatedTo: nil) // okay from anywhere
    sync(isolatedTo: #isolation)
  }

  Task.detached {
    sync(isolatedTo: nil) // okay from anywhere
    sync(isolatedTo: #isolation)
  }
}

@MainActor func fromMain(ns: NotSendable) async -> NotSendable {
  await pass(value: ns, isolation: MainActor.shared)
}

nonisolated func fromNonisolated(ns: NotSendable) async -> NotSendable {
  await pass(value: ns, isolation: nil)
}

#if ALLOW_TYPECHECKER_ERRORS

func invalidIsolatedClosureParam<A: AnyActor> ( // expected-typechecker-warning {{'AnyActor' is deprecated: Use 'any Actor' with 'DistributedActor.asLocalActor' instead}}
  _: (isolated A) async throws -> Void // expected-typechecker-error {{'isolated' parameter type 'A' does not conform to 'Actor' or 'DistributedActor'}}
) {}

#endif

public func useDefaultIsolation(
  _ isolation: isolated (any Actor)? = #isolation
) {}

public func useDefaultIsolationWithoutIsolatedParam(
  _ isolation: (any Actor)? = #isolation
) {}

@MainActor func callUseDefaultIsolation() async {
  useDefaultIsolation()
  useDefaultIsolationWithoutIsolatedParam()
}

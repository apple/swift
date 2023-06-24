// RUN: %target-typecheck-verify-swift  -disable-availability-checking -warn-concurrency -parse-as-library
// REQUIRES: concurrency

class NotConcurrent { } // expected-note 27{{class 'NotConcurrent' does not conform to the 'Sendable' protocol}}

// ----------------------------------------------------------------------
// Sendable restriction on actor operations
// ----------------------------------------------------------------------

actor A1 {
  let localLet: NotConcurrent = NotConcurrent()
  func synchronous() -> NotConcurrent? { nil }
  func asynchronous(_: NotConcurrent?) async { }
}

// Actor initializers and Sendable
actor A2 {
  var localVar: NotConcurrent

  init(value: NotConcurrent) {
    self.localVar = value
  }

  init(forwardSync value: NotConcurrent) {
    self.init(value: value)
  }

  init(delegatingSync value: NotConcurrent) {
    self.init(forwardSync: value)
  }

  init(valueAsync value: NotConcurrent) async {
    self.localVar = value
  }

  init(forwardAsync value: NotConcurrent) async {
    await self.init(valueAsync: value)
  }

  nonisolated init(nonisoAsync value: NotConcurrent, _ c: Int) async {
    if c == 0 {
      await self.init(valueAsync: value)
    } else {
      self.init(value: value)
    }
  }

  init(delegatingAsync value: NotConcurrent, _ c: Int) async {
    if c == 0 {
      await self.init(valueAsync: value)
    } else if c == 1 {
      self.init(value: value)
    } else if c == 2 {
      await self.init(forwardAsync: value)
    } else {
      self.init(delegatingSync: value)
    }
  }
}

func testActorCreation(value: NotConcurrent) async {
  _ = A2(value: value) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to nonisolated initializer 'init(value:)' cannot cross actor boundary}}

  _ = await A2(valueAsync: value) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to actor-isolated initializer 'init(valueAsync:)' cannot cross actor boundary}}

  _ = A2(delegatingSync: value) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to nonisolated initializer 'init(delegatingSync:)' cannot cross actor boundary}}

  _ = await A2(delegatingAsync: value, 9) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to actor-isolated initializer 'init(delegatingAsync:_:)' cannot cross actor boundary}}

  _ = await A2(nonisoAsync: value, 3) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to nonisolated initializer 'init(nonisoAsync:_:)' cannot cross actor boundary}}
}

extension A1 {
  func testIsolation(other: A1) async {
    // All within the same actor domain, so the Sendable restriction
    // does not apply.
    _ = localLet
    _ = synchronous()
    _ = await asynchronous(nil)
    _ = self.localLet
    _ = self.synchronous()
    _ = await self.asynchronous(nil)

    // Across to a different actor, so Sendable restriction is enforced.
    _ = other.localLet // expected-warning{{non-sendable type 'NotConcurrent' in asynchronous access to actor-isolated property 'localLet' cannot cross actor boundary}}
    _ = await other.synchronous() // expected-warning{{non-sendable type 'NotConcurrent?' returned by implicitly asynchronous call to actor-isolated instance method 'synchronous()' cannot cross actor boundary}}
    _ = await other.asynchronous(nil) // expected-warning{{non-sendable type 'NotConcurrent?' passed in call to actor-isolated instance method 'asynchronous' cannot cross actor boundary}}
  }
}

// ----------------------------------------------------------------------
// Sendable restriction on global actor operations
// ----------------------------------------------------------------------
actor TestActor {}

@globalActor
struct SomeGlobalActor {
  static var shared: TestActor { TestActor() }
}

@SomeGlobalActor
let globalValue: NotConcurrent? = nil

@SomeGlobalActor
func globalSync(_: NotConcurrent?) {
}

@SomeGlobalActor
func globalAsync(_: NotConcurrent?) async {
  await globalAsync(globalValue) // both okay because we're in the actor
  globalSync(nil)
}

func globalTest() async {
  let a = globalValue // expected-warning{{non-sendable type 'NotConcurrent?' in asynchronous access to global actor 'SomeGlobalActor'-isolated let 'globalValue' cannot cross actor boundary}}
  await globalAsync(a) // expected-warning{{non-sendable type 'NotConcurrent?' passed in implicitly asynchronous call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
  await globalSync(a)  // expected-warning{{non-sendable type 'NotConcurrent?' passed in call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
}

struct HasSubscript {
  @SomeGlobalActor
  subscript (i: Int) -> NotConcurrent? { nil }
}

class ClassWithGlobalActorInits { // expected-note 2{{class 'ClassWithGlobalActorInits' does not conform to the 'Sendable' protocol}}
  @SomeGlobalActor
  init(_: NotConcurrent) { }

  @SomeGlobalActor
  init() { }
}


@MainActor
func globalTestMain(nc: NotConcurrent) async {
  let a = globalValue // expected-warning{{non-sendable type 'NotConcurrent?' in asynchronous access to global actor 'SomeGlobalActor'-isolated let 'globalValue' cannot cross actor boundary}}
  await globalAsync(a) // expected-warning{{non-sendable type 'NotConcurrent?' passed in implicitly asynchronous call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
  await globalSync(a)  // expected-warning{{non-sendable type 'NotConcurrent?' passed in call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
  _ = await ClassWithGlobalActorInits(nc) // expected-warning{{non-sendable type 'NotConcurrent' passed in call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
  // expected-warning@-1{{non-sendable type 'ClassWithGlobalActorInits' returned by call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
  _ = await ClassWithGlobalActorInits() // expected-warning{{non-sendable type 'ClassWithGlobalActorInits' returned by call to global actor 'SomeGlobalActor'-isolated function cannot cross actor boundary}}
}

@SomeGlobalActor
func someGlobalTest(nc: NotConcurrent) {
  let hs = HasSubscript()
  let _ = hs[0] // okay
  _ = ClassWithGlobalActorInits(nc)
}

// ----------------------------------------------------------------------
// Sendable restriction on captures.
// ----------------------------------------------------------------------
func acceptNonConcurrent(_: () -> Void) { }
func acceptConcurrent(_: @Sendable () -> Void) { }

func testConcurrency() {
  let x = NotConcurrent()
  var y = NotConcurrent()
  y = NotConcurrent()
  acceptNonConcurrent {
    print(x) // okay
    print(y) // okay
  }
  acceptConcurrent {
    print(x) // expected-warning{{capture of 'x' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    print(y) // expected-warning{{capture of 'y' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    // expected-error@-1{{reference to captured var 'y' in concurrently-executing code}}
  }
}

@preconcurrency func acceptUnsafeSendable(_ fn: @Sendable () -> Void) { }

func testUnsafeSendableNothing() {
  var x = 5
  acceptUnsafeSendable {
    x = 17 // expected-warning{{mutation of captured var 'x' in concurrently-executing code}}
  }
  print(x)
}

func testUnsafeSendableInAsync() async {
  var x = 5
  acceptUnsafeSendable {
    x = 17 // expected-warning{{mutation of captured var 'x' in concurrently-executing code}}
  }
  print(x)
}

// ----------------------------------------------------------------------
// Sendable restriction on key paths.
// ----------------------------------------------------------------------
class NC: Hashable { // expected-note 2{{class 'NC' does not conform to the 'Sendable' protocol}}
  func hash(into: inout Hasher) { }
  static func==(_: NC, _: NC) -> Bool { true }
}

class HasNC {
  var dict: [NC: Int] = [:]
}

func testKeyPaths(dict: [NC: Int], nc: NC) {
  _ = \HasNC.dict[nc] // expected-warning{{cannot form key path that captures non-sendable type 'NC'}}
}

// ----------------------------------------------------------------------
// Sendable restriction on nonisolated declarations.
// ----------------------------------------------------------------------
actor ANI {
  nonisolated let nc = NC()
  nonisolated func f() -> NC? { nil }
}

func testANI(ani: ANI) async {
  _ = ani.nc // expected-warning{{non-sendable type 'NC' in asynchronous access to nonisolated property 'nc' cannot cross actor boundary}}
}

// ----------------------------------------------------------------------
// Sendable restriction on conformances.
// ----------------------------------------------------------------------
protocol AsyncProto {
  func asyncMethod(_: NotConcurrent) async
}

extension A1: AsyncProto {
  func asyncMethod(_: NotConcurrent) async { } // expected-warning{{non-sendable type 'NotConcurrent' in parameter of the protocol requirement satisfied by actor-isolated instance method 'asyncMethod' cannot cross actor boundary}}
}

protocol MainActorProto {
  func asyncMainMethod(_: NotConcurrent) async
}

class SomeClass: MainActorProto {
  @SomeGlobalActor
  func asyncMainMethod(_: NotConcurrent) async { } // expected-warning{{non-sendable type 'NotConcurrent' in parameter of the protocol requirement satisfied by global actor 'SomeGlobalActor'-isolated instance method 'asyncMainMethod' cannot cross actor boundary}}
}

// ----------------------------------------------------------------------
// Sendable restriction on concurrent functions.
// ----------------------------------------------------------------------

@Sendable func concurrentFunc() -> NotConcurrent? { nil }

// ----------------------------------------------------------------------
// No Sendable restriction on @Sendable function types.
// ----------------------------------------------------------------------
typealias CF = @Sendable () -> NotConcurrent?
typealias BadGenericCF<T> = @Sendable () -> T?
typealias GoodGenericCF<T: Sendable> = @Sendable () -> T? // okay

var concurrentFuncVar: (@Sendable (NotConcurrent) -> Void)? = nil

// ----------------------------------------------------------------------
// Sendable restriction on @Sendable closures.
// ----------------------------------------------------------------------
func acceptConcurrentUnary<T>(_: @Sendable (T) -> T) { }

func concurrentClosures<T>(_: T) { // expected-note{{consider making generic parameter 'T' conform to the 'Sendable' protocol}} {{26-26=: Sendable}}
  acceptConcurrentUnary { (x: T) in
    _ = x // ok
    acceptConcurrentUnary { _ in x } // expected-warning{{capture of 'x' with non-sendable type 'T' in a `@Sendable` closure}}
  }
}

// ----------------------------------------------------------------------
// Sendable checking
// ----------------------------------------------------------------------
struct S1: Sendable {
  var nc: NotConcurrent // expected-warning{{stored property 'nc' of 'Sendable'-conforming struct 'S1' has non-sendable type 'NotConcurrent'}}
}

struct S2<T>: Sendable { // expected-note{{consider making generic parameter 'T' conform to the 'Sendable' protocol}} {{12-12=: Sendable}}
  var nc: T // expected-warning{{stored property 'nc' of 'Sendable'-conforming generic struct 'S2' has non-sendable type 'T'}}
}

struct S3<T> {
  var c: T
  var array: [T]
}

extension S3: Sendable where T: Sendable { }

enum E1: Sendable {
  case payload(NotConcurrent) // expected-warning{{associated value 'payload' of 'Sendable'-conforming enum 'E1' has non-sendable type 'NotConcurrent'}}
}

enum E2<T> {
  case payload(T)
}

extension E2: Sendable where T: Sendable { }

final class C1: Sendable {
  let nc: NotConcurrent? = nil // expected-warning{{stored property 'nc' of 'Sendable'-conforming class 'C1' has non-sendable type 'NotConcurrent?'}}
  var x: Int = 0 // expected-warning{{stored property 'x' of 'Sendable'-conforming class 'C1' is mutable}}
  let i: Int = 0
}

final class C2: Sendable {
  let x: Int = 0
}

class C3 { }

class C4: C3, @unchecked Sendable {
  var y: Int = 0 // okay
}

class C5: @unchecked Sendable {
  var x: Int = 0 // okay
}

class C6: C5 {
  var y: Int = 0 // still okay, it's unsafe
}

final class C7<T>: Sendable { }

class C9: Sendable { } // expected-warning{{non-final class 'C9' cannot conform to 'Sendable'; use '@unchecked Sendable'}}

extension NotConcurrent {
  func f() { }

  func test() {
    Task {
      f() // expected-warning{{capture of 'self' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    }

    Task {
      self.f()  // expected-warning{{capture of 'self' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    }

    Task { [self] in
      f()  // expected-warning{{capture of 'self' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    }

    Task { [self] in
      self.f()  // expected-warning{{capture of 'self' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
    }
  }
}

// ----------------------------------------------------------------------
// @unchecked Sendable disabling checking
// ----------------------------------------------------------------------
struct S11: @unchecked Sendable {
  var nc: NotConcurrent // okay
}

struct S12<T>: @unchecked Sendable {
  var nc: T // okay
}

enum E11<T>: @unchecked Sendable {
  case payload(NotConcurrent) // okay
  case other(T) // okay
}

class C11 { }

class C12: @unchecked C11 { } // expected-error{{'unchecked' attribute cannot apply to non-protocol type 'C11'}}

protocol P { }

protocol Q: @unchecked Sendable { } // expected-error{{'unchecked' attribute only applies in inheritance clauses}}

typealias TypeAlias1 = @unchecked P // expected-error{{'unchecked' attribute only applies in inheritance clauses}}


// ----------------------------------------------------------------------
// UnsafeSendable historical name
// ----------------------------------------------------------------------
enum E12<T>: UnsafeSendable { // expected-warning{{'UnsafeSendable' is deprecated: Use @unchecked Sendable instead}}
  case payload(NotConcurrent) // okay
  case other(T) // okay
}

// ----------------------------------------------------------------------
// @Sendable inference through optionals
// ----------------------------------------------------------------------
func testSendableOptionalInference(nc: NotConcurrent) {
  var fn: (@Sendable () -> Void)? = nil
  fn = {
    print(nc) // expected-warning{{capture of 'nc' with non-sendable type 'NotConcurrent' in a `@Sendable` closure}}
  }
  _ = fn
}

// RUN: %target-typecheck-verify-swift -enable-upcoming-feature InferSendableFromCaptures -disable-availability-checking -strict-concurrency=complete
// RUN: %target-swift-emit-silgen %s -verify -enable-upcoming-feature InferSendableFromCaptures -disable-availability-checking -module-name sendable_methods -strict-concurrency=complete | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: asserts

func outer() {
    @Sendable func sendable() {}

    func nonSendable() {}

    let _ : @Sendable () -> Void = sendable
    let _ : @Sendable () -> Void = nonSendable // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}
}


final class C : Sendable {
  func f() {}
}

struct S : Sendable {
  func f(){}
}

enum E : Sendable {
  case a, b, c
  
  func f() {}
}

protocol P : Sendable {
  init()
}

final class InferredSendableC: P {
  func f() { }
}

struct InferredSendableS: P {
  func f() { }
}

enum InferredSendableE: P {
  case a, b
  case c(Int)
  
  func f() { }
}

final class GenericC<T>: P {
  func f() { }
}

struct GenericS<T> : P {
  init(_: T) { }

  init() { }

  func f() { }

  func g() async { }
}

enum GenericE<T> {
  case a
  case b(T)
}

extension GenericE: Sendable where T: Sendable { }

class NonSendable {
  func f() {}
}

func g<T>(_ f: @escaping @Sendable (T) -> (@Sendable () -> Void)) where T: P {
  Task {
    let instance = T()
    f(instance)()
  }
}
// Unapplied Func Parameters
g(GenericS<NonSendable>.f)  // ok because unapplied references don't capture state
g(GenericC<NonSendable>.f)
g(InferredSendableS.f)
g(InferredSendableC.f)
g(InferredSendableE.f)
g(GenericS<Int>.f)
g(GenericC<Int>.f)

struct GenericQ<T> {

  func f() { }
}

func g2<T>(_ f: @Sendable (T) -> (@Sendable () -> Void)) { }
g2(GenericQ<NonSendable>.f) // ok because unapplied references don't capture state


// Conditional Conformances
//extension GenericS : Sendable where T : Sendable { }
//extension GenericC : Sendable where T : Sendable { }

extension E {
  init(){
    self = .a
  }
}

extension InferredSendableE {
  init(){
    self = .a
  }
}

// Partial Apply Parameters
func h(_ f: (@Sendable () -> Void)) {  }
h(GenericQ<NonSendable>().f) // ok
h(GenericS(NonSendable()).f) // ok
h(GenericS<Int>().f)
h(GenericS(1).f)
h(NonSendable().f) // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}

func executeAsTask (_ f: @escaping  @Sendable () -> Void) {
  Task {
    f()
  }
}
executeAsTask(S().f)
executeAsTask(C().f)
executeAsTask(E().f)
executeAsTask(NonSendable().f) // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}

do {
  let f = S.f
  let _ : @Sendable () -> () = f(S())
}

// Declarations
let us:  @Sendable (GenericS<Int>) -> (@Sendable () -> Void) = GenericS<Int>.f
let uc:  @Sendable (GenericC<Int>) -> (@Sendable () -> Void) = GenericC<Int>.f

let unappliedStruct:  @Sendable (S) -> (@Sendable () -> Void) = S.f
let unappliedClass:  @Sendable (C) -> (@Sendable () -> Void) = C.f
let unappliedEnum:  @Sendable (E) -> (@Sendable () -> Void) = E.f

var partialStruct : @Sendable () -> Void = S().f
var partialClass : @Sendable () -> Void = C().f
var partialEnum : @Sendable () -> Void = E().f

// Reassign
partialClass = NonSendable().f // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}
partialStruct = NonSendable().f // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}
partialEnum = NonSendable().f // expected-warning{{converting non-sendable function value to '@Sendable () -> Void' may introduce data races}}

// Static Functions 
struct World {
  static func greet () { print("hello") }
}

let helloworld:  @Sendable () -> Void = World.greet

class NonSendableC { // expected-note{{class 'NonSendableC' does not conform to the 'Sendable' protocol}}
    var x: Int = 0

    @Sendable func inc() { // expected-warning {{instance method of non-Sendable type 'NonSendableC' cannot be marked as '@Sendable'}}
        x += 1
    }
}

func doWork() -> Int {
  Int.random(in: 1..<42)
}

// unapplied global func call
let work: @Sendable () -> Int = doWork
Task<Int, Never>.detached(priority: nil, operation: doWork)
Task<Int, Never>.detached(priority: nil, operation: work)

// generic argument for `T` should be inferred as `@escaping @Sendable`
func generic2<T>(_ f: T) { }

// CHECK-LABEL: sil hidden [ossa] @$s16sendable_methods12testGeneric2yyF : $@convention(thin) () -> ()
// CHECK: [[F:%.*]] = alloc_stack $@Sendable @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <GenericS<Int>, @Sendable () -> ()>
// CHECK: [[GENERIC_2_FN:%.*]] = function_ref @$s16sendable_methods8generic2yyxlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> ()
// CHECK-NEXT: {{.*}} = apply [[GENERIC_2_FN]]<@Sendable (GenericS<Int>) -> @Sendable () -> ()>([[F]]) : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> ()
func testGeneric2() {
  generic2(GenericS<Int>.f)
}

actor TestActor {}

@globalActor
struct SomeGlobalActor {
  static var shared: TestActor { TestActor() }
}

@SomeGlobalActor
// CHECK-LABEL: sil hidden [ossa] @$s16sendable_methods8generic3yyxYalF : $@convention(thin) @async <T> (@in_guaranteed T) -> ()
//
// CHECK: [[F2:%.*]] = alloc_stack $@Sendable @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <GenericS<String>, @Sendable () -> ()>
// CHECK: [[GENERIC_3_FN:%.*]] = function_ref @$s16sendable_methods8generic3yyxYalF : $@convention(thin) @async <τ_0_0> (@in_guaranteed τ_0_0) -> ()
// CHECK-NEXT: {{.*}} = apply [[GENERIC_3_FN]]<@Sendable (GenericS<String>) -> @Sendable () -> ()>([[F2]]) : $@convention(thin) @async <τ_0_0> (@in_guaranteed τ_0_0) -> ()
//
// CHECK: [[F3:%.*]] = alloc_stack $@Sendable @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <GenericS<NonSendable>, @Sendable () -> ()>
// CHECK: [[GENERIC_3_FN:%.*]] = function_ref @$s16sendable_methods8generic3yyxYalF : $@convention(thin) @async <τ_0_0> (@in_guaranteed τ_0_0) -> ()
// CHECK-NEXT: {{.*}} = apply [[GENERIC_3_FN]]<@Sendable (GenericS<NonSendable>) -> @Sendable () -> ()>([[F3]]) : $@convention(thin) @async <τ_0_0> (@in_guaranteed τ_0_0) -> ()
func generic3<T>(_ x: T) async {

  await generic3(GenericS<String>.f)

  await generic3(GenericS<NonSendable>.f)
}

// Make sure that static members are handled properly
do {
  struct X<T> {
    init(_: T) {
    }

    static func test(_: T) {}
  }

  class Test<T> {
    init(_: T) {
      _ = X(self) // Ok
      _ = X.init(self) // Ok
      _ = Optional.some(self) // Ok

      let _: @Sendable (Int) -> X<Int> = X.init // Ok
      let _: @Sendable (Test<Int>) -> Void = X.test // Ok
      let _ = X.test(self) // Ok
    }
  }
}

func test_initializer_ref() {
  func test<T>(_: @Sendable (T, T) -> Array<T>) {
  }

  let initRef: @Sendable (Int, Int) -> Array<Int> = Array<Int>.init // Ok

  test(initRef) // Ok
  test(Array<Int>.init) // Ok
}

// rdar://119593407 - incorrect errors when partially applied member is accessed with InferSendableFromCaptures
do {
  @MainActor struct ErrorHandler {
    static func log(_ error: Error) {}
  }

  @MainActor final class Manager {
    static var shared: Manager!

    func test(_: @escaping @MainActor (Error) -> Void) {
    }
  }

  @MainActor class Test {
    func schedule() {
      Task {
        Manager.shared.test(ErrorHandler.log) // Ok (access is wrapped in an autoclosure)
      }
    }
  }
}

// rdar://125932231 - incorrect `error: type of expression is ambiguous without a type annotation`
do {
  class C {}

  func test(c: C) -> (any Sendable)? {
    true ? nil : c // Ok
  }
}

func acceptSendableFunc<T, U>(_: @Sendable (T) -> U) { }

acceptSendableFunc(InferredSendableE.c)
acceptSendableFunc(GenericE<Int>.b)
acceptSendableFunc(GenericE<NonSendable>.b)

// Make sure pattern matching isn't affected by @Sendable on cases.
func testPatternMatch(ge: [GenericE<Int>]) {
  if case .b(let a) = ge.first {
    _ = a
  }
}

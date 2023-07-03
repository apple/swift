// RUN: %target-typecheck-verify-swift  -disable-availability-checking
// REQUIRES: concurrency

// provides coverage for rdar://71548470

actor TestActor {}

@globalActor
struct SomeGlobalActor {
  static var shared: TestActor { TestActor() }
}

// expected-note@+1 6 {{calls to global function 'syncGlobActorFn()' from outside of its actor context are implicitly asynchronous}}
@SomeGlobalActor func syncGlobActorFn() { }
@SomeGlobalActor func asyncGlobalActFn() async { }

actor Alex {
  @SomeGlobalActor let const_memb = 20
  @SomeGlobalActor var mut_memb = 30
  @SomeGlobalActor func method() {}

  // expected-note@+1 2 {{mutation of this subscript is only permitted within the actor}}
  @SomeGlobalActor subscript(index : Int) -> Int {
    get {
      return index * 2
    }
    set {}
  }
}

func referenceGlobalActor() async {
  let a = Alex()
  _ = a.method
  _ = a.const_memb
  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{7-7=await }}
  _ = a.mut_memb // expected-note{{property access is 'async'}}

  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{7-7=await }}
  _ = a[1]  // expected-note{{subscript access is 'async'}}
  a[0] = 1  // expected-error{{global actor 'SomeGlobalActor'-isolated subscript 'subscript(_:)' can not be mutated from a non-isolated context}}

  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{7-7=await }}
  _ = 32 + a[1] // expected-note@:12{{subscript access is 'async'}}
}


// expected-note@+1 {{add '@SomeGlobalActor' to make global function 'referenceGlobalActor2()' part of global actor 'SomeGlobalActor'}} {{1-1=@SomeGlobalActor }}
func referenceGlobalActor2() {
  let x = syncGlobActorFn // expected-note{{calls to let 'x' from outside of its actor context are implicitly asynchronous}}
  x() // expected-error{{call to global actor 'SomeGlobalActor'-isolated let 'x' in a synchronous nonisolated context}}
}


// expected-note@+1 {{add 'async' to function 'referenceAsyncGlobalActor()' to make it asynchronous}} {{33-33= async}}
func referenceAsyncGlobalActor() {
  let y = asyncGlobalActFn
  y() // expected-error{{'async' call in a function that does not support concurrency}}
}


// expected-note@+1 {{add '@SomeGlobalActor' to make global function 'callGlobalActor()' part of global actor 'SomeGlobalActor'}} {{1-1=@SomeGlobalActor }}
func callGlobalActor() {
  syncGlobActorFn() // expected-error {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}
}

func fromClosure() {
  { () -> Void in
    let x = syncGlobActorFn // expected-note{{calls to let 'x' from outside of its actor context are implicitly asynchronous}}
    x() // expected-error{{call to global actor 'SomeGlobalActor'-isolated let 'x' in a synchronous nonisolated context}}
  }()

  // expected-error@+1 {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}
  let _ = { syncGlobActorFn() }()
}

class Taylor {
  init() {
    syncGlobActorFn() // expected-error {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}

    _ = syncGlobActorFn
  }

  deinit {
    syncGlobActorFn() // expected-error {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}

    _ = syncGlobActorFn
  }

  // expected-note@+1 {{add '@SomeGlobalActor' to make instance method 'method1()' part of global actor 'SomeGlobalActor'}} {{3-3=@SomeGlobalActor }}
  func method1() {
    syncGlobActorFn() // expected-error {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}

    _ = syncGlobActorFn
  }

  // expected-note@+1 {{add '@SomeGlobalActor' to make instance method 'cannotBeHandler()' part of global actor 'SomeGlobalActor'}} {{3-3=@SomeGlobalActor }}
  func cannotBeHandler() -> Int {
    syncGlobActorFn() // expected-error {{call to global actor 'SomeGlobalActor'-isolated global function 'syncGlobActorFn()' in a synchronous nonisolated context}}

    _ = syncGlobActorFn
    return 0
  }
}


func fromAsync() async {
  let x = syncGlobActorFn
  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{3-3=await }}
  x() // expected-note{{calls to let 'x' from outside of its actor context are implicitly asynchronous}}


  let y = asyncGlobalActFn
  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{3-3=await }}
  y() // expected-note{{call is 'async'}}

  let a = Alex()
  let fn = a.method
  fn() // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{calls to let 'fn' from outside of its actor context are implicitly asynchronous}}
  _ = a.const_memb
  _ = a.mut_memb  // expected-error{{expression is 'async' but is not marked with 'await'}}
  // expected-note@-1{{property access is 'async'}}

  // expected-error@+1{{expression is 'async' but is not marked with 'await'}}{{7-7=await }}
  _ = a[1]  // expected-note{{subscript access is 'async'}}
  _ = await a[1]
  a[0] = 1  // expected-error{{global actor 'SomeGlobalActor'-isolated subscript 'subscript(_:)' can not be mutated from a non-isolated context}}
}

// expected-note@+1{{mutation of this var is only permitted within the actor}}
@SomeGlobalActor var value: Int = 42

func topLevelSyncFunction(_ number: inout Int) { }
// expected-error@+1{{global actor 'SomeGlobalActor'-isolated var 'value' can not be used 'inout' from a non-isolated context}}
topLevelSyncFunction(&value)

// Strict checking based on inferred Sendable/async/etc.
@preconcurrency @SomeGlobalActor class Super { }

class Sub: Super {
  func f() { }

  func g() {
    Task.detached {
      await self.f() // okay: requires await because f is on @SomeGlobalActor
    }
  }

  func g2() {
    Task.detached {
      self.f() // expected-error{{expression is 'async' but is not marked with 'await'}}
      // expected-note@-1{{calls to instance method 'f()' from outside of its actor context are implicitly asynchronous}}
    }
  }
}

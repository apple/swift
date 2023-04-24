// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency

// MARK: - Fixtures

@globalActor final actor FirstActor {
  static let shared = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared = SecondActor()
}

@FirstActor
func isolatedFunc() {}

class AsyncBase {
    deinit async {  // expected-note 2{{async deinit was introduced to class hierarchy here}}
        await Task.yield()
    }
}

class ImplicitDerived: AsyncBase {}

#if !SILGEN
class SyncDerived: AsyncBase {
    deinit {} // expected-error {{deinit must be 'async' because parent class has 'async' deinit}}
}

class IndirectlySyncDerived: ImplicitDerived {
    deinit {} // expected-error {{deinit must be 'async' because parent class has 'async' deinit}}
}
#endif

@FirstActor
class GoodDerived: ImplicitDerived {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'GoodDerived' has different actor isolation from nonisolated superclass 'ImplicitDerived'; this is an error in Swift 6}}

    @SecondActor
    var foo: Int = 42
    
    nonisolated func bar() async {
#if !SILGEN
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{property access is 'async'}}
        print(foo)
#endif
        // ok
        print(await foo)
    }
    
    deinit async {
#if !SILGEN
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{property access is 'async'}}
        print(foo)
#endif
        // ok
        print(await foo)
    }
}

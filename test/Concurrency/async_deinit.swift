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
    deinit async {} // expected-note 2{{async deinit was introduced to class hierarchy here}}
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

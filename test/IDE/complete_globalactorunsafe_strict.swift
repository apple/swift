// RUN: %batch-code-completion -warn-concurrency
// REQUIRES: concurrency

// SAFE_NOTREC: Begin completions, 2 items
// SAFE_NOTREC-DAG: Keyword[self]/CurrNominal:          self[#SafelyIsolatedCls#];
// SAFE_NOTREC-DAG: Decl[InstanceMethod]/CurrNominal: method()[' async'][#Void#];

// UNSAFE_NOTREC: Begin completions, 2 items
// UNSAFE_NOTREC-DAG: Keyword[self]/CurrNominal:          self[#UnsafelyIsolatedCls#];
// UNSAFE_NOTREC-DAG: Decl[InstanceMethod]/CurrNominal: method()[' async'][#Void#];

// SAFE_OK: Begin completions, 2 items
// SAFE_OK-DAG: Keyword[self]/CurrNominal:          self[#SafelyIsolatedCls#];
// SAFE_OK-DAG: Decl[InstanceMethod]/CurrNominal:   method()[' async'][#Void#];

// UNSAFE_OK: Begin completions, 2 items
// UNSAFE_OK-DAG: Keyword[self]/CurrNominal:          self[#UnsafelyIsolatedCls#];
// UNSAFE_OK-DAG: Decl[InstanceMethod]/CurrNominal:   method()[' async'][#Void#];

// UNSAFE_OK_SYNC: Begin completions, 2 items
// UNSAFE_OK_SYNC-DAG: Keyword[self]/CurrNominal:          self[#UnsafelyIsolatedCls#];
// UNSAFE_OK_SYNC-DAG: Decl[InstanceMethod]/CurrNominal:   method()[#Void#];

@globalActor
actor MyGlobalActor {
  static let shared = MyGlobalActor()
}

@MyGlobalActor(unsafe)
class UnsafelyIsolatedCls {
  func method()
}

@MyGlobalActor
class SafelyIsolatedCls {
  func method()
}

class TestNormal {
  func testSync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) {
    s.#^IN_METHOD_SYNC_SAFE?check=SAFE_NOTREC^#
    u.#^IN_METHOD_SYNC_UNSAFE?check=UNSAFE_NOTREC^#
  }
  func testAsync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) async {
    s.#^IN_METHOD_ASYNC_SAFE?check=SAFE_OK^#
    u.#^IN_METHOD_ASYNC_UNSAFE?check=UNSAFE_OK^#
  }
}

func testSync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) {
  s.#^IN_FUNC_SYNC_SAFE?check=SAFE_NOTREC^#
  u.#^IN_FUNC_SYNC_UNSAFE?check=UNSAFE_NOTREC^#
}
func testAsync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) async {
  s.#^IN_FUNC_ASYNC_SAFE?check=SAFE_OK^#
  u.#^IN_FUNC_ASYNC_UNSAFE?check=UNSAFE_OK^#
}

func testClosure(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) {
    func receiverSync(fn: () -> Void) {}
    func receiverAsync(fn: () async -> Void) {}

    receiverSync {
      dummy;
      s.#^IN_CLOSURE_SYNC_SAFE?check=SAFE_NOTREC^#
      u.#^IN_CLOSURE_SYNC_UNSAFE?check=UNSAFE_NOTREC^#
    }

    receiverAsync {
      dummy;
      s.#^IN_CLOSURE_ASYNC_SAFE?check=SAFE_OK^#
      u.#^IN_CLOSURE_ASYNC_UNSAFE?check=UNSAFE_OK^#
    }
}

actor TestActor {
  func test(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) {
    s.#^IN_ACTOR_SYNC_SAFE?check=SAFE_NOTREC^#
    u.#^IN_ACTOR_SYNC_UNSAFE?check=UNSAFE_NOTREC^#
  }

  func testAsync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) async {
    s.#^IN_ACTOR_ASYNC_SAFE?check=SAFE_OK^#
    u.#^IN_ACTOR_ASYNC_UNSAFE?check=UNSAFE_OK^#
  }
}

@MainActor
class TestMainActorIsolated {
  func test(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) {
    s.#^IN_GLOBALACTOR_SYNC_SAFE?check=SAFE_NOTREC^#
    u.#^IN_GLOBALACTOR_SYNC_UNSAFE?check=UNSAFE_NOTREC^#
  }

  func testAsync(s: SafelyIsolatedCls, u: UnsafelyIsolatedCls) async {
    s.#^IN_GLOBALACTOR_ASYNC_SAFE?check=SAFE_OK^#
    u.#^IN_GLOBALACTOR_ASYNC_UNSAFE?check=UNSAFE_OK^#
  }
}

// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always
// TODO: move to target-run-simple-leaks-swift once CI is using at least Xcode 14.3

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// FIXME: enable discarding taskgroup on windows; rdar://104762037
// UNSUPPORTED: OS=windows-msvc

import _Concurrency

actor SimpleCountDownLatch {
  let from: Int
  var count: Int

  var continuation: CheckedContinuation<Void, Never>?

  init(from: Int) {
    self.from = from
    self.count = from
  }

  func hit() {
    defer { count -= 1 }
    if count == 0 {
      fatalError("Counted down more times than expected! (From: \(from))")
    } else if count == 1 {
      continuation?.resume()
    }
  }

  func wait() async {
    guard self.count > 0 else {
      return // we're done
    }

    return await withCheckedContinuation { cc in
      self.continuation = cc
    }
  }
}

final class PrintDeinit {
  let id: String
  init(id: String) {
    self.id = id
  }

  deinit {
    print("deinit, id: \(id)")
  }
}

struct Boom: Error {
  let printDeinit: PrintDeinit

  init(id: String) {
    self.printDeinit = PrintDeinit(id: id)
  }
}

final class BoomClass: Error {
  let id: String

  init(id: String) {
    self.id = id
  }

  deinit {
    print("deinit, id: \(id)")
  }
}

final class SomeClass: @unchecked Sendable {
  let id: String
  init(id: String) {
    self.id = id
  }

  deinit {
    print("deinit, id: \(id)")
  }
}

// NOTE: Not as StdlibUnittest/TestSuite since these types of tests are unreasonably slow to load/debug.

func testTwo() async {
  let latch = SimpleCountDownLatch(from: 2)

  _ = try? await withThrowingDiscardingTaskGroup() { group in
    group.addTask {
      await latch.hit()
      throw Boom(id: "race-boom")
    }
    group.addTask {
      await latch.hit()
      SomeClass(id: "race-boom-class") // will be discarded
    }

    return 12
  }

  // since values may deinit in any order, we just assert their count basically
  // CHECK-DAG: deinit, id: race-boom
  // CHECK-DAG: deinit, id: race-boom
  await latch.wait()
  try? await Task.sleep(for: .milliseconds(300))

  print("done") // CHECK: done
}

func manyOk() async {
  let latch = SimpleCountDownLatch(from: 6)

  _ = try? await withThrowingDiscardingTaskGroup() { group in
    for i in 0..<6 {
      group.addTask {
        await latch.hit()
        _ = SomeClass(id: "many-ok") // will be discarded
      }
    }

    return 12
  }
  // since values may deinit in any order, we just assert their count basically
  // CHECK-DAG: deinit, id: many-ok
  // CHECK-DAG: deinit, id: many-ok
  // CHECK-DAG: deinit, id: many-ok
  // CHECK-DAG: deinit, id: many-ok
  // CHECK-DAG: deinit, id: many-ok
  // CHECK-DAG: deinit, id: many-ok

  await latch.wait()
  try? await Task.sleep(for: .milliseconds(300))

  print("done") // CHECK: done
}

func manyThrows() async {
  let latch = SimpleCountDownLatch(from: 6)

  do {
    let value: Void = try await withThrowingDiscardingTaskGroup() { group in
      for i in 0..<6 {
        group.addTask {
          await latch.hit()
          throw BoomClass(id: "many-error") // will be rethrown
        }
      }

      // since values may deinit in any order, we just assert their count basically
      // CHECK-DAG: deinit, id: many-error
      // CHECK-DAG: deinit, id: many-error
      // CHECK-DAG: deinit, id: many-error
      // CHECK-DAG: deinit, id: many-error
      // CHECK-DAG: deinit, id: many-error
      // CHECK-DAG: deinit, id: many-error

      12 // must be ignored
    }
    preconditionFailure("Should throw")
  } catch {
    precondition("\(error)" == "main.BoomClass", "error was: \(error)")
  }

  await latch.wait()
  try? await Task.sleep(for: .milliseconds(300))

  print("done") // CHECK: done
}

func manyValuesThrows() async {
  let latch = SimpleCountDownLatch(from: 6)

  // many errors, many values
  _ = try? await withThrowingDiscardingTaskGroup() { group in
    group.addTask {
      await latch.hit()
      _ = SomeClass(id: "mixed-ok") // will be discarded
    }
    group.addTask {
      await latch.hit()
      _ = SomeClass(id: "mixed-ok") // will be discarded
    }
    group.addTask {
      await latch.hit()
      _ = SomeClass(id: "mixed-ok") // will be discarded
    }
    group.addTask {
      await latch.hit()
      throw Boom(id: "mixed-error")
    }
    group.addTask {
      await latch.hit()
      throw Boom(id: "mixed-error")
    }
    group.addTask {
      await latch.hit()
      throw Boom(id: "mixed-error")
    }

    return 12
  }

  // since values may deinit in any order, we just assert their count basically
  // three ok's
  // CHECK-DAG: deinit, id: mixed
  // CHECK-DAG: deinit, id: mixed
  // CHECK-DAG: deinit, id: mixed
  // three errors
  // CHECK-DAG: deinit, id: mixed
  // CHECK-DAG: deinit, id: mixed
  // CHECK-DAG: deinit, id: mixed

  await latch.wait()
  try? await Task.sleep(for: .milliseconds(300))

  print("done") // CHECK: done
}

@main struct Main {
  static func main() async {
    await testTwo()
    await manyOk()
    await manyThrows()
    await manyValuesThrows()
  }
}

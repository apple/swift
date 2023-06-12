// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always
// TODO: move to target-run-simple-leaks-swift once CI is using at least Xcode 14.3

// rdar://109998145 - Temporarily disable this test
// REQUIRES: rdar109998145

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// FIXME: enable discarding taskgroup on windows; rdar://104762037
// UNSUPPORTED: OS=windows-msvc

import _Concurrency

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

@main struct Main {
  static func main() async {
    _ = try? await withThrowingDiscardingTaskGroup() { group in
      group.addTask {
        throw Boom(id: "race-boom-class")
      }
      group.addTask {
        SomeClass(id: "race-boom-class") // will be discarded
      }
      // since values may deinit in any order, we just assert their count basically
      // CHECK-DAG: deinit, id: race-boom-class
      // CHECK-DAG: deinit, id: race-boom-class

      return 12
    }

    // many ok
    _ = try? await withThrowingDiscardingTaskGroup() { group in
      for i in 0..<6 {
        group.addTask {
          SomeClass(id: "many-ok") // will be discarded
        }
        // since values may deinit in any order, we just assert their count basically
        // CHECK-DAG: deinit, id: many-ok
        // CHECK-DAG: deinit, id: many-ok
        // CHECK-DAG: deinit, id: many-ok
        // CHECK-DAG: deinit, id: many-ok
        // CHECK-DAG: deinit, id: many-ok
        // CHECK-DAG: deinit, id: many-ok
      }

      return 12
    }

    // many throws
    do {
      let value = try await withThrowingDiscardingTaskGroup() { group in
        for i in 0..<6 {
          group.addTask {
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

    // many errors, many values
    _ = try? await withThrowingDiscardingTaskGroup() { group in
      group.addTask {
        SomeClass(id: "mixed-ok") // will be discarded
      }
      group.addTask {
        SomeClass(id: "mixed-ok") // will be discarded
      }
      group.addTask {
        SomeClass(id: "mixed-ok") // will be discarded
      }
      group.addTask {
        throw Boom(id: "mixed-error")
      }
      group.addTask {
        throw Boom(id: "mixed-error")
      }
      group.addTask {
        throw Boom(id: "mixed-error")
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

      return 12
    }
  }
}

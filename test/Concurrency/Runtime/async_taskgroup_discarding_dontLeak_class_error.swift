// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// FIXME: enable discarding taskgroup on windows; rdar://104762037
// UNSUPPORTED: OS=windows-msvc

import _Concurrency

final class ClassBoom: Error {
  let id: String

  init(file: String = #fileID, line: UInt = #line) {
    self.id = "\(file):\(line)"
    print("INIT OF ClassBoom from \(id)")
  }

  deinit {
    print("DEINIT OF ClassBoom from \(id)")
  }
}

@main struct Main {
  static func main() async {

    // many errors
    _ = try? await withThrowingDiscardingTaskGroup() { group in
      group.addTask { throw ClassBoom() }
      group.addTask { throw ClassBoom() }
      group.addTask { throw ClassBoom() }
      group.addTask { throw ClassBoom() }
      group.addTask { 12 }
      return 12

      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
    }
  }
}

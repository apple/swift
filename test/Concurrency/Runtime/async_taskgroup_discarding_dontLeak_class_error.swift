// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always

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

final class ClassBoom: Error {
  let id: String
  let latch: SimpleCountDownLatch

  init(latch: SimpleCountDownLatch, file: String = #fileID, line: UInt = #line) {
    self.latch = latch
    self.id = "\(file):\(line)"
    print("INIT OF ClassBoom from \(id)")
  }

  deinit {
    print("DEINIT OF ClassBoom from \(id)")
    Task { [latch] in await latch.hit() }
  }
}

@main struct Main {
  static func main() async {
    let latch = SimpleCountDownLatch(from: 4)

    // many errors
    _ = try? await withThrowingDiscardingTaskGroup() { group in
      group.addTask { throw ClassBoom(latch: latch) }
      group.addTask { throw ClassBoom(latch: latch) }
      group.addTask { throw ClassBoom(latch: latch) }
      group.addTask { throw ClassBoom(latch: latch) }
      group.addTask {
        12 // ignore this on purpose
      }
      return 42

      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
      // CHECK: DEINIT OF ClassBoom
    }

    await latch.wait()
    print("done") // CHECK: done
  }
}
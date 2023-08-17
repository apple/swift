// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s
// TODO: move to target-run-simple-leaks-swift once CI is using at least Xcode 14.3

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// FIXME: enable discarding taskgroup on windows; rdar://104762037
// UNSUPPORTED: OS=windows-msvc

import _Concurrency

final class PayloadFirst {}
final class PayloadSecond {}

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
    print("hit @ \(count)")
    if count == 0 {
      fatalError("Counted down more times than expected! (From: \(from))")
    } else if count == 1 {
      print("hit resume")
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

final class ErrorFirst: Error {
  let first: PayloadFirst
  let id: String
  let latch: SimpleCountDownLatch

  init(latch: SimpleCountDownLatch, file: String = #fileID, line: UInt = #line) {
    self.latch = latch
    self.id = "\(file):\(line)"
    first = .init()
    print("init \(self) id:\(id)")
  }
  deinit {
    print("deinit \(self) id:\(id)")
    Task { [latch] in await latch.hit() }
  }
}

// Should not really matter that different types, but want to make really sure
final class ErrorSecond: Error {
  let first: PayloadFirst
  let id: String
  let latch: SimpleCountDownLatch

  init(latch: SimpleCountDownLatch, file: String = #fileID, line: UInt = #line) {
    self.latch = latch
    self.id = "\(file):\(line)"
    first = .init()
    print("init \(self) id:\(id)")
  }
  deinit {
    print("deinit \(self) id:\(id)")
    Task { [latch] in await latch.hit() }
  }
}


func shouldStartWith(_ lhs: Any, _ rhs: Any) {
  let l = "\(lhs)"
  let r = "\(rhs)"
  precondition(l.prefix("\(r)".count) == r, "'\(l)' did not start with '\(r)'")
}

// NOTE: Not as StdlibUnittest/TestSuite since these types of tests are unreasonably slow to load/debug.

@discardableResult
func one() -> Int {
  1
}

@main struct Main {
  static func main() async {
    let latch = SimpleCountDownLatch(from: 6)
    do {

      let got = try await withThrowingDiscardingTaskGroup() { group in
        group.addTask { one() }
        group.addTask { throw ErrorFirst(latch: latch) }
        group.addTask { throw ErrorFirst(latch: latch) }
        group.addTask { throw ErrorFirst(latch: latch) }

        group.addTask { throw ErrorSecond(latch: latch) }
        group.addTask { throw ErrorSecond(latch: latch) }
        group.addTask { throw ErrorSecond(latch: latch) }

        return 12
      }
      fatalError("expected error to be re-thrown, got: \(got)")
    } catch {
       shouldStartWith(error, "main.Error")
    }
    // CHECK: deinit main.Error
    // CHECK: deinit main.Error
    // CHECK: deinit main.Error

    // CHECK: deinit main.Error
    // CHECK: deinit main.Error
    // CHECK: deinit main.Error
    await latch.wait()
    print("done") // CHECK: done
  }
}

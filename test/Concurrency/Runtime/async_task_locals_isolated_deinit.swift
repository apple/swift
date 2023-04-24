// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-stdlib %import-libdispatch)

// REQUIRES: libdispatch
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Swift
import _Concurrency
import Dispatch
import StdlibUnittest

@_silgen_name("swift_task_isCurrentExecutor")
func isCurrentExecutor(_ executor: Builtin.Executor) -> Bool

func isCurrentExecutor(_ executor: UnownedSerialExecutor) -> Bool {
    isCurrentExecutor(unsafeBitCast(executor, to: Builtin.Executor.self))
}

@available(SwiftStdlib 5.1, *)
struct TL {
  @TaskLocal
  static var number: Int = 0
}

actor ActorWithIsolatedDeinit {
  let expectedNumber: Int
  let group: DispatchGroup
  
  init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
  }
  
  deinit {
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}

@globalActor actor AnotherActor: GlobalActor {
  static let shared = AnotherActor()
  
  func performTesting(_ work: @Sendable () -> Void) {
    work()
  }
}

class ClassWithIsolatedDeinit {
  let expectedNumber: Int
  let group: DispatchGroup
  
  init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
  }
  
  @AnotherActor
  deinit {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}


let tests = TestSuite("Isolated Deinit")

if #available(SwiftStdlib 5.1, *) {
  tests.test("fast path") {
    let group = DispatchGroup()
    group.enter()
    Task {
      await TL.$number.withValue(42) {
        await AnotherActor.shared.performTesting {
          _ = ClassWithIsolatedDeinit(expectedNumber: 42, group: group)
        }
      }
    }
    group.wait()
  }
  
  tests.test("slow path") {
    let group = DispatchGroup()
    group.enter()
    group.enter()
    Task {
      TL.$number.withValue(37) {
        _ = ActorWithIsolatedDeinit(expectedNumber: 37, group: group)
      }
      TL.$number.withValue(99) {
        _ = ClassWithIsolatedDeinit(expectedNumber: 99, group: group)
      }
    }
    group.wait()
  }
}

runAllTests()


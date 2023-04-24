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

actor ActorCopy {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
    self.probe = Probe(expectedNumber: expectedNumber, group: group)
    self.probe.probeExpectedExecutor = self.unownedExecutor
  }
  
  isolated deinit {
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}

actor ActorReset {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
    self.probe = Probe(expectedNumber: expectedNumber, group: group)
    self.probe.probeExpectedExecutor = self.unownedExecutor
  }
  
  @resetTaskLocals
  isolated deinit {
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}

actor ActorAPI {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  init(resetNumber: Int, setNumber: Int, group: DispatchGroup) {
    self.expectedNumber = resetNumber
    self.group = group
    self.probe = Probe(expectedNumber: setNumber, group: group)
    self.probe.probeExpectedExecutor = self.unownedExecutor
  }
  
  isolated deinit {
    withResetTaskLocalValues {
      expectTrue(isCurrentExecutor(self.unownedExecutor))
      expectEqual(expectedNumber, TL.number)
      group.leave()
    }
  }
}

@globalActor actor AnotherActor: GlobalActor {
  static let shared = AnotherActor()
  
  func performTesting(_ work: @Sendable () -> Void) {
    work()
  }
}

class Probe {
  var probeExpectedExecutor: UnownedSerialExecutor
  let probeExpectedNumber: Int
  let probeGroup: DispatchGroup
  
  init(expectedNumber: Int, group: DispatchGroup) {
    self.probeExpectedExecutor = AnotherActor.shared.unownedExecutor
    self.probeExpectedNumber = expectedNumber
    self.probeGroup = group
    group.enter()
  }
  
  deinit {
    expectTrue(isCurrentExecutor(probeExpectedExecutor))
    expectEqual(probeExpectedNumber, TL.number)
    probeGroup.leave()
  }
}

class ClassCopy: Probe {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  override init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
    self.probe = Probe(expectedNumber: expectedNumber, group: group)
    super.init(expectedNumber: expectedNumber, group: group)
  }
  
  @AnotherActor
  deinit {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}

class ClassReset: Probe {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  override init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
    self.probe = Probe(expectedNumber: expectedNumber, group: group)
    super.init(expectedNumber: expectedNumber, group: group)
  }
  
  @AnotherActor
  @resetTaskLocals
  deinit {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    group.leave()
  }
}

class ClassAPI: Probe {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe
  
  init(resetNumber: Int, setNumber: Int, group: DispatchGroup) {
    self.expectedNumber = resetNumber
    self.group = group
    self.probe = Probe(expectedNumber: setNumber, group: group)
    super.init(expectedNumber: setNumber, group: group)
  }
  
  @AnotherActor
  deinit {
    withResetTaskLocalValues {
      expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
      expectEqual(expectedNumber, TL.number)
      group.leave()
    }
  }
}


let tests = TestSuite("Isolated Deinit")

if #available(SwiftStdlib 5.1, *) {
  tests.test("fast path") {
    let group = DispatchGroup()
    group.enter()
    group.enter()
    group.enter()
    Task {
      await TL.$number.withValue(42) {
        await AnotherActor.shared.performTesting {
          _ = ClassCopy(expectedNumber: 42, group: group)
          _ = ClassReset(expectedNumber: 0, group: group)
          _ = ClassAPI(resetNumber: 0, setNumber: 42, group: group)
        }
      }
    }
    group.wait()
  }
  
  tests.test("slow path") {
    let group = DispatchGroup()
    group.enter()
    group.enter()
    group.enter()
    group.enter()
    group.enter()
    group.enter()
    Task {
      TL.$number.withValue(37) {
        _ = ActorCopy(expectedNumber: 37, group: group)
        _ = ActorReset(expectedNumber: 0, group: group)
        _ = ActorAPI(resetNumber: 0, setNumber: 37, group: group)
      }
      TL.$number.withValue(99) {
        _ = ClassCopy(expectedNumber: 99, group: group)
        _ = ClassReset(expectedNumber: 0, group: group)
        _ = ClassAPI(resetNumber: 0, setNumber: 99, group: group)
      }
    }
    group.wait()
  }
}

runAllTests()


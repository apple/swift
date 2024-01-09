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
private func isCurrentExecutor(_ executor: Builtin.Executor) -> Bool

private func isCurrentExecutor(_ executor: UnownedSerialExecutor) -> Bool {
    isCurrentExecutor(unsafeBitCast(executor, to: Builtin.Executor.self))
}

extension DispatchGroup {
    func enter(_ count: Int) {
        for _ in 0..<count {
            self.enter()
        }
    }
}

@available(SwiftStdlib 5.1, *)
struct TL {
  @TaskLocal
  static var number: Int = 0
}

func checkTaskLocalStack() {
  TL.$number.withValue(-999) {
    expectEqual(-999, TL.number)
  }
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
    checkTaskLocalStack()
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
    checkTaskLocalStack()
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
      checkTaskLocalStack()
      group.leave()
    }
  }
}

actor ActorCopyAsync {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe

  init(expectedNumber: Int, group: DispatchGroup) {
    self.expectedNumber = expectedNumber
    self.group = group
    self.probe = Probe(expectedNumber: expectedNumber, group: group)
    self.probe.probeExpectedExecutor = self.unownedExecutor
  }

  isolated deinit async {
    await Task.yield()
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    checkTaskLocalStack()
    group.leave()
  }
}

actor ActorResetAsync {
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
  isolated deinit async {
    await Task.yield()
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    checkTaskLocalStack()
    group.leave()
  }
}

actor ActorAPIAsync {
  let expectedNumber: Int
  let group: DispatchGroup
  let probe: Probe

  init(resetNumber: Int, setNumber: Int, group: DispatchGroup) {
    self.expectedNumber = resetNumber
    self.group = group
    self.probe = Probe(expectedNumber: setNumber, group: group)
    self.probe.probeExpectedExecutor = self.unownedExecutor
  }

  isolated deinit async {
    await Task.yield()
    withResetTaskLocalValues {
      expectTrue(isCurrentExecutor(self.unownedExecutor))
      expectEqual(expectedNumber, TL.number)
      checkTaskLocalStack()
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
    checkTaskLocalStack()
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
    checkTaskLocalStack()
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
    checkTaskLocalStack()
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
      checkTaskLocalStack()
      group.leave()
    }
  }
}

class ClassCopyAsync: Probe {
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
  deinit async {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    await Task.yield()
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    checkTaskLocalStack()
    group.leave()
  }
}

class ClassResetAsync: Probe {
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
  deinit async {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    await Task.yield()
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    expectEqual(expectedNumber, TL.number)
    checkTaskLocalStack()
    group.leave()
  }
}

class ClassAPIAsync: Probe {
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
  deinit async {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    await Task.yield()
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    await withResetTaskLocalValues {
      expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
      expectEqual(expectedNumber, TL.number)
      await Task.yield()
      expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
      expectEqual(expectedNumber, TL.number)
      checkTaskLocalStack()
      group.leave()
    }
  }
}


let tests = TestSuite("Isolated Deinit")

if #available(SwiftStdlib 5.1, *) {
  tests.test("sync fast path") {
    let group = DispatchGroup()
    group.enter(3)
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
  
  tests.test("sync slow path") {
    let group = DispatchGroup()
    group.enter(6)
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
  
  tests.test("async same actor") {
    let group = DispatchGroup()
    group.enter(3)
    Task {
      await TL.$number.withValue(42) {
        await AnotherActor.shared.performTesting {
          _ = ClassCopyAsync(expectedNumber: 42, group: group)
          _ = ClassResetAsync(expectedNumber: 0, group: group)
          _ = ClassAPIAsync(resetNumber: 0, setNumber: 42, group: group)
        }
      }
    }
    group.wait()
  }
  
  tests.test("async different actor") {
    let group = DispatchGroup()
    group.enter(6)
    Task {
      TL.$number.withValue(37) {
        _ = ActorCopyAsync(expectedNumber: 37, group: group)
        _ = ActorResetAsync(expectedNumber: 0, group: group)
        _ = ActorAPIAsync(resetNumber: 0, setNumber: 37, group: group)
      }
      TL.$number.withValue(99) {
        _ = ClassCopyAsync(expectedNumber: 99, group: group)
        _ = ClassResetAsync(expectedNumber: 0, group: group)
        _ = ClassAPIAsync(resetNumber: 0, setNumber: 99, group: group)
      }
    }
    group.wait()
  }
  
  tests.test("no TLs") {
    let group = DispatchGroup()
    group.enter(12)
    Task {
      _ = ActorCopy(expectedNumber: 0, group: group)
      _ = ActorReset(expectedNumber: 0, group: group)
      _ = ActorAPI(resetNumber: 0, setNumber: 0, group: group)
      _ = ClassCopy(expectedNumber: 0, group: group)
      _ = ClassReset(expectedNumber: 0, group: group)
      _ = ClassAPI(resetNumber: 0, setNumber: 0, group: group)
      _ = ActorCopyAsync(expectedNumber: 0, group: group)
      _ = ActorResetAsync(expectedNumber: 0, group: group)
      _ = ActorAPIAsync(resetNumber: 0, setNumber: 0, group: group)
      _ = ClassCopyAsync(expectedNumber: 0, group: group)
      _ = ClassResetAsync(expectedNumber: 0, group: group)
      _ = ClassAPIAsync(resetNumber: 0, setNumber: 0, group: group)
    }
    group.wait()
  }
}

runAllTests()


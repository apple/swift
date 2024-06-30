// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking %import-libdispatch %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime

// REQUIRES: libdispatch

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding

import StdlibUnittest

struct MyUnexpectedExecutorError: Sendable, Error {}

final class NaiveQueueExecutor: SerialExecutor {
  init() {}

  public func enqueue(_ job: consuming ExecutorJob) {
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }

  func checkIsolated() throws {
    // Just always throw, we're testing if the throw propagates correctly
    throw MyUnexpectedExecutorError()
  }
}

actor DefaultActor {
  nonisolated func nonisolatedCheck() async throws {
    try self.checkIsolated()
  }

  func checkIt() async throws {
    try self.checkIsolated()
  }
}

actor QueueExecutorActor {
  let executor: NaiveQueueExecutor

  init(executor: NaiveQueueExecutor) {
    self.executor = executor
  }

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    self.executor.asUnownedSerialExecutor()
  }

  nonisolated func checkIt() async throws {
    print("Before preconditionIsolated")
    try self.checkIsolated()
    print("After preconditionIsolated")
  }
}

func getError(_ op: () async throws -> ()) async -> Error {
  do {
    try await op()
    fatalError("Expected error!")
  } catch {
    return error
  }
}

func assertExpectedError<Err: Error>(_ error: any Error, type: Err.Type) {
  precondition(error is Err,
    "Expected \(_Concurrency.UnexpectedSerialExecutorError.self) but got \(error)")
  print("Got expected: \(error)")
}

// ==== ------------------------------------------------------------------------

let tests = TestSuite("ThrowingCheckIsolated")

let defaultActor = DefaultActor()

tests.test("DefaultActor.checkIsolated() - should throw if off the actor") {
  let error = await getError { try defaultActor.checkIsolated() }

  assertExpectedError(error, type: _Concurrency.UnexpectedSerialExecutorError.self)
}

tests.test("DefaultActor.checkIsolated() - should not throw on the actor") {
  try! await defaultActor.checkIt()
}

tests.test("DefaultActor.checkIsolated() - nonisolated func should throw, it is not on the actor") {
  let error = await getError { try await defaultActor.nonisolatedCheck() }
  assertExpectedError(error, type: _Concurrency.UnexpectedSerialExecutorError.self)
}

await runAllTestsAsync()

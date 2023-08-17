// RUN: %target-typecheck-verify-swift -disable-availability-checking
// REQUIRES: concurrency

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding


extension Executor {
  func enqueue(_ job: UnownedJob) { // expected-warning{{'Executor.enqueue(UnownedJob)' is deprecated as a protocol requirement; conform type 'NoneExecutor' to 'Executor' by implementing 'func enqueue(ExecutorJob)' instead}}
    fatalError()
  }
}

final class NoneExecutor: SerialExecutor {

  // the enqueue from the extension is properly picked up,
  // even though we do warn about deprecation on it in the extension.

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

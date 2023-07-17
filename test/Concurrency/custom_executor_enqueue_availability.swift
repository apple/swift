// RUN: %target-typecheck-verify-swift -enable-experimental-move-only
// REQUIRES: concurrency
// REQUIRES: OS=macosx

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

/// Such a type may be encountered since Swift 5.5 (5.1 backdeployed) if someone implemented the
/// not documented, but public Executor types back then already. Allow these to be implemented
/// without warnings.
@available(SwiftStdlib 5.1, *)
final class OldExecutorOldStdlib: SerialExecutor {
  func enqueue(_ job: UnownedJob) {}

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

/// We warn on the ExecutorJob witness if the type has a broader
/// availability, since in this case the UnownedJob version needs to exist.
@available(SwiftStdlib 5.1, *)
final class BothExecutorOldStdlib: SerialExecutor {
  func enqueue(_ job: UnownedJob) {}

  @available(SwiftStdlib 5.9, *)
  func enqueue(_ job: __owned ExecutorJob) {} // expected-warning{{'Executor.enqueue(ExecutorJob)' will never be used, due to the presence of 'enqueue(UnownedJob)'}}

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

/// Meanwhile, we warn on the UnownedJob overload if the availability is new enough
/// that it can be dropped.
@available(SwiftStdlib 5.9, *)
final class BothExecutorNewStdlib: SerialExecutor {
  func enqueue(_ job: UnownedJob) {} // expected-warning{{'Executor.enqueue(UnownedJob)' is deprecated as a protocol requirement; conform type 'BothExecutorNewStdlib' to 'Executor' by implementing 'func enqueue(ExecutorJob)' instead}}

  func enqueue(_ job: __owned ExecutorJob) {}

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

@available(SwiftStdlib 5.9, *)
final class TripleExecutor: SerialExecutor {
  func enqueue(_ job: UnownedJob) {} // expected-warning{{'Executor.enqueue(UnownedJob)' is deprecated as a protocol requirement; conform type 'TripleExecutor' to 'Executor' by implementing 'func enqueue(ExecutorJob)' instead}}

  // expected-warning@+2{{'Job' is deprecated: renamed to 'ExecutorJob'}}
  // expected-note@+1{{use 'ExecutorJob' instead}}
  func enqueue(_ job: __owned Job) {} // expected-warning{{'Executor.enqueue(Job)' is deprecated as a protocol requirement; conform type 'TripleExecutor' to 'Executor' by implementing 'func enqueue(ExecutorJob)' instead}}

  func enqueue(_ job: consuming ExecutorJob) {}

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

/// Implementing the new signature on 5.9 platforms is good, no warnings
@available(SwiftStdlib 5.9, *)
final class NewExecutorNewStdlib: SerialExecutor {
  func enqueue(_ job: __owned ExecutorJob) {}

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

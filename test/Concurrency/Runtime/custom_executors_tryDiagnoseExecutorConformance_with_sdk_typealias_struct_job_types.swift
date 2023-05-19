// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-concurrency-typealias-struct-job) -typecheck -parse-as-library %s -verify

// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

import _Concurrency

final class FakeExecutor1: SerialExecutor {
  func enqueue(_ job: __owned ExecutorJob) {}
}

final class FakeExecutor2: SerialExecutor {
  func enqueue(_ job: __owned Job) {}
}

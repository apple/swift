// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-concurrency-owned-job-param) -emit-sil -parse-as-library %s -verify

// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

import _Concurrency

final class FakeExecutor: SerialExecutor {
  // implements the __owned requirement in "old" SDK:
  //    func enqueue(_ job: __owned Job)
  func enqueue(_ job: consuming ExecutorJob) {}
}
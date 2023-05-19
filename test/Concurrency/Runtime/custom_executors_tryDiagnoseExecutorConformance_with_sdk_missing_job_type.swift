// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-concurrency-without-job) -emit-sil -parse-as-library %s -verify

// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

import _Concurrency

// NOTE: This test simulates what happens when we have an SDK with some missing concurrency types.
// Specifically, we're missing the ExecutorJob and Job declarations.
// This simulates a pre-Swift-5.9 SDK being used with a Swift 5.9+ compiler,
// which would have failed previously due to missing handling of missing types.
//
// This is a regression test for that situation

final class FakeExecutor: SerialExecutor {
  func enqueue(_ job: UnownedJob) {}
}
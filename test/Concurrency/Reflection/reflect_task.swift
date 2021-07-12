// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -enable-experimental-concurrency -parse-stdlib -parse-as-library -lswiftSwiftReflectionTest %s -o %t/reflect_task
// RUN: %target-codesign %t/reflect_task

// RUN: %target-run %target-swift-reflection-test %t/reflect_task | %FileCheck %s --dump-input=fail

// REQUIRES: reflection_test_support
// REQUIRES: executable_test
// REQUIRES: concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Swift
import _Concurrency

import SwiftReflectionTest

@_silgen_name("swift_task_getCurrent")
func _getCurrentAsyncTask() -> UInt

func add(_ a: UInt, _ b: UInt) async -> UInt {
  if b == 0 {
    reflect(asyncTask: _getCurrentAsyncTask())
    // CHECK: Reflecting an async task.
    // CHECK: Async task {{0x[0-9a-fA-F]*}}

    // The actual number of chunks we'll get depends on internal implementation
    // details that we don't want this test to depend on. We'll just make sure
    // we get at least two, and ignore the details.
    // CHECK:   Allocation block {{0x[0-9a-fA-F]*}}
    // CHECK:     Chunk at {{0x[0-9a-fA-F]*}} length {{[0-9]*}} kind {{[0-9]*}}
    // CHECK:   Allocation block {{0x[0-9a-fA-F]*}}
    // CHECK:     Chunk at {{0x[0-9a-fA-F]*}} length {{[0-9]*}} kind {{[0-9]*}}
    return a
  } else {
    return await add(a, b - 1) + 1
  }
}

@main struct Main {
  static func main() async {
    let n = await add(100, 100)
    reflect(any: n)

    doneReflecting()
  }
}

// CHECK: Done.

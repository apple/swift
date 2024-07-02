// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) 2>&1 | %FileCheck %s --dump-input=always
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: swift_task_debug_log

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

#if os(Linux)
import Glibc
#elseif os(Windows)
import MSVCRT
#elseif canImport(Android)
import Android
#else
import Darwin
#endif

func test_withUnsafeCurrentTask() async {
  // The task we're running in ("main")
  // CHECK: creating task [[MAIN_TASK:0x.*]] with parent 0x0

  // CHECK: creating task [[TASK:0x.*]] with parent 0x0
  let t = Task.detached {
    withUnsafeCurrentTask { task in
      fputs("OK: \(task!)", stderr)
    }
    fputs("DONE", stderr)
  }

  // CHECK: OK: UnsafeCurrentTask(_task: (Opaque Value))
  // CHECK: DONE
  // CHECK: destroy task [[TASK]]
  await t.value
}

@main struct Main {
  static func main() async {
    await test_withUnsafeCurrentTask()
  }
}

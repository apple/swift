// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency) | %FileCheck %s --dump-input=always
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64

import Dispatch

// ==== ------------------------------------------------------------------------
// MARK: "Infrastructure" for the tests

extension DispatchQueue {
  func async<R>(operation: @escaping () async -> R) -> Task.Handle<R> {
    let handle = Task.runDetached(operation: operation)

    // Run the task
    _ = { self.async { handle.run() } }() // force invoking the non-async version

    return handle
  }
}

@available(*, deprecated, message: "This is a temporary hack")
@discardableResult
func launch<R>(operation: @escaping () async -> R) -> Task.Handle<R> {
  let handle = Task.runDetached(operation: operation)

  // Run the task
  DispatchQueue.global(priority: .default).async { handle.run() }

  return handle
}

// ==== ------------------------------------------------------------------------
// MARK: Tests

func test_skipCallingNext() {
  let numbers = [1, 1]

  let taskHandle = launch { () async -> Int in
    return await try! Task.withGroup(resultType: Int.self) { (group) async -> Int in
      for n in numbers {
        print("group.add { \(n) }")
        await group.add { () async -> Int in
          sleep(1)
          print("  inside group.add { \(n) } (canceled: \(await Task.isCancelled()))")
          return n
        }
      }

      // return immediately; the group should wait on the tasks anyway
      print("return immediately 0 (canceled: \(await Task.isCancelled()))")
      return 0
    }
  }

  // CHECK: main task

  launch { () async -> () in
    let result = await try! taskHandle.get()
    // CHECK: group.add { 1 }
    // CHECK: group.add { 1 }
    // CHECK: return immediately 0 (canceled: false)

    // CHECK: inside group.add { 1 } (canceled: false)
    // CHECK: inside group.add { 1 } (canceled: false)

    // CHECK: result: 0
    print("result: \(result)")
    assert(result == 0)
    exit(0)
  }

  print("main task")
}

test_skipCallingNext()

dispatchMain()

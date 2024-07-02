// RUN: %target-run-simple-swift( -plugin-path %swift-plugin-dir -Xfrontend -disable-availability-checking -parse-as-library %import-libdispatch) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

@available(SwiftStdlib 5.1, *)
enum TL {
  @TaskLocal
  static var number = 0
}

@available(SwiftStdlib 5.1, *)
@discardableResult
func printTaskLocal<V>(

    _ key: TaskLocal<V>,
    _ expected: V? = nil,
    file: String = #file, line: UInt = #line
) -> V? {
  let value = key.get()
  print("\(key) (\(value)) at \(file):\(line)")
  if let expected = expected {
    assert("\(expected)" == "\(value)",
        "Expected [\(expected)] but found: \(value), at \(file):\(line)")
  }
  return expected
}

// ==== ------------------------------------------------------------------------

@available(SwiftStdlib 5.1, *)
func groups() async {
  // no value
  _ = await withTaskGroup(of: Int.self) { group in
    printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)
  }

  // no value in parent, value in child
  let x1: Int = await withTaskGroup(of: Int.self) { group in
    group.spawn {
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)
      // inside the child task, set a value
      _ = TL.$number.withValue(1) {
        printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (1)
      }
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)
      return TL.$number.get() // 0
    }

    return await group.next()!
  }
  assert(x1 == 0)

  // value in parent and in groups
  await TL.$number.withValue(2) {
    printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)

    let x2: Int = await withTaskGroup(of: Int.self) { group in
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)
      group.spawn {
        printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)

        async let childInsideGroupChild = printTaskLocal(TL.$number)
        _ = await childInsideGroupChild // CHECK: TaskLocal<Int>(defaultValue: 0) (2)

        return TL.$number.get()
      }
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)

      return await group.next()!
    }

    assert(x2 == 2)
  }
}

@available(SwiftStdlib 5.1, *)
func taskInsideGroup() async {
  Task {
    print("outside") // CHECK: outside
    _ = await withTaskGroup(of: Int.self) { group -> Int in
      print("in group") // CHECK: in group
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)

      for _ in 0..<5 {
        Task {
          printTaskLocal(TL.$number)
          print("some task")
        }
      }

      return 0
    }
  }

  // CHECK: some task
  // CHECK: some task
  // CHECK: some task
  // CHECK: some task

  await Task.sleep(5 * 1_000_000_000)

//  await t.value
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    await groups()
    await taskInsideGroup()
  }
}

// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

@available(SwiftStdlib 5.1, *)
func test_taskGroup_is_asyncSequence() async {
  print(#function)

  let sum = await withTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...10 {
      group.spawn {
        print("add \(n)")
        return n
      }
    }

    var sum = 0
    for await r in group { // here
      print("next: \(r)")
      sum += r
    }

    return sum
  }

  print("result: \(sum)")
}

@available(SwiftStdlib 5.1, *)
func test_throwingTaskGroup_is_asyncSequence() async throws {
  print(#function)

  let sum = try await withThrowingTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...10 {
      group.spawn {
        print("add \(n)")
        return n
      }
    }

    var sum = 0
    for try await r in group { // here
      print("next: \(r)")
      sum += r
    }

    return sum
  }

  print("result: \(sum)")
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    await test_taskGroup_is_asyncSequence()
    // CHECK: test_taskGroup_is_asyncSequence()
    // CHECK: result: 55

    try! await test_throwingTaskGroup_is_asyncSequence()
    // CHECK: test_throwingTaskGroup_is_asyncSequence()
    // CHECK: result: 55
  }
}

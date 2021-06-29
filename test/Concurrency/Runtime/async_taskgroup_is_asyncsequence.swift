// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// UNSUPPORTED: linux
// XFAIL: windows
// XFAIL: openbsd

@available(SwiftStdlib 5.5, *)
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

@available(SwiftStdlib 5.5, *)
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

@available(SwiftStdlib 5.5, *)
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

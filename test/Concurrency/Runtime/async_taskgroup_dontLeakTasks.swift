// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s
// TODO: move to target-run-simple-leaks-swift once CI is using at least Xcode 14.3

// Task group addTask is not supported in freestanding mode
// UNSUPPORTED: freestanding

// REQUIRES: executable_test
// REQUIRES: concurrency

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

final class Something {
  let int: Int
  init(int: Int) {
    self.int = int
  }

  deinit {
    print("deinit, Something, int: \(self.int)")
  }
}

func test_taskGroup_next() async {
  let tasks = 5
  _ = await withTaskGroup(of: Something.self, returning: Int.self) { group in
    for n in 0..<tasks {
      group.addTask {
        Something(int: n)
      }
    }

    var sum = 0
    for await value in group {
      sum += value.int
    }


    // CHECK-DAG: deinit, Something, int: 0
    // CHECK-DAG: deinit, Something, int: 1
    // CHECK-DAG: deinit, Something, int: 2
    // CHECK-DAG: deinit, Something, int: 3
    // CHECK-DAG: deinit, Something, int: 4

    return sum
  }
}

@main struct Main {
  static func main() async {
    await test_taskGroup_next()
  }
}

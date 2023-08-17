// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: OS=linux-gnu

struct Boom: Error {}

func boom() async throws -> Int {
  throw Boom()
}

@available(SwiftStdlib 5.1, *)
func test_taskGroup_next() async {
  let sum = await withThrowingTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...10 {
      group.addTask {
        return n.isMultiple(of: 3) ? try await boom() : n
      }
    }

    var sum = 0
    var catches = 0
    for _ in 1...5 {
      do {
        while let r = try await group.next() {
          sum += r
          print("add \(r) -> sum: \(sum)")
        }
      } catch {
        catches += 1
        print("catch: \(catches)")
      }
    }

    // CHECK: catches with group.next(): 3
    print("catches with group.next(): \(catches)")

    return sum
  }

  // CHECK: result with group.next(): 37
  print("result with group.next(): \(sum)")
}

@available(SwiftStdlib 5.1, *)
func test_taskGroup_for_in() async {
  let sum = await withThrowingTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...10 {
      group.addTask {
        return n.isMultiple(of: 3) ? try await boom() : n
      }
    }

    var sum = 0
    var catches = 0
    for _ in 1...5 {
      do {
        for try await r in group {
          sum += r
        }
      } catch {
        catches += 1
      }
    }

    // CHECK: catches with for-in: 3
    print("catches with for-in: \(catches)")

    return sum
  }

  // CHECK: result with for-in: 37
  print("result with for-in: \(sum)")
}

@available(SwiftStdlib 5.1, *)
func test_taskGroup_asyncIterator() async {
  let sum = await withThrowingTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...10 {
      group.addTask {
        return n.isMultiple(of: 3) ? try await boom() : n
      }
    }

    var sum = 0
    var catches = 0
    for _ in 1...5 {
      var iterator = group.makeAsyncIterator()
      do {
        while let r = try await iterator.next() {
          sum += r
        }
        if try! await iterator.next() != nil {
          fatalError("Element returned from iterator after nil")
        }
      } catch {
        catches += 1
        if try! await iterator.next() != nil {
          fatalError("Element returned from iterator after throw")
        }
      }
    }

    // CHECK: catches with for-in: 3
    print("catches with for-in: \(catches)")

    return sum
  }

  // CHECK: result with async iterator: 37
  print("result with async iterator: \(sum)")
}

@available(SwiftStdlib 5.1, *)
func test_taskGroup_contains() async {
  let sum = await withTaskGroup(of: Int.self, returning: Int.self) { group in
    for n in 1...4 {
      group.addTask {
        return n
      }
    }

    let three = await group.contains(3)
    print("three = \(three)") // CHECK: three = true

    for n in 5...7 {
      group.addTask {
        return n
      }
    }

    let six = await group.contains(6)
    print("six = \(six)") // CHECK: six = true
                                                                    
                                                                    
    let sixAgain = await group.contains(6)
    print("six again = \(sixAgain)") // CHECK: six again = false

    return 0
  }

  // CHECK: result with async iterator: 0
  print("result with async iterator: \(sum)")
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    await test_taskGroup_next()
    await test_taskGroup_for_in()
    await test_taskGroup_asyncIterator()
    await test_taskGroup_contains()
  }
}

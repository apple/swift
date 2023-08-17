// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// REQUIRES: concurrency_runtime

import Dispatch

func completeSlowly(n: Int) async -> Int {
  await Task.sleep(UInt64((n * 1_000_000_000) + 1_000_000_000))
  print("  complete group.spawn { \(n) }")
  return n
}

/// Tasks complete AFTER they are next() polled.
func test_sum_nextOnPending() async {
  let numbers = [1, 2, 3]
  let expected = 6 // FIXME: numbers.reduce(0, +) this hangs?

  let sum = try! await withTaskGroup(of: Int.self) { (group) async -> Int in
    for n in numbers {
      group.spawn {
        let res = await completeSlowly(n: n)
        return res
      }
    }

    var sum = 0
    print("before group.next(), sum: \(sum)")
    while let n = try! await group.next() {
      assert(numbers.contains(n), "Unexpected value: \(n)! Expected any of \(numbers)")
      print("next: \(n)")
      sum += n
      print("before group.next(), sum: \(sum)")
    }

    print("task group returning: \(sum)")
    return sum
  }

  // The completions are set apart by n seconds, so we expect them to arrive
  // in the order as the numbers (and delays) would suggest:
  //
  // CHECK: complete group.spawn { 1 }
  // CHECK: next: 1
  // CHECK: complete group.spawn { 2 }
  // CHECK: next: 2
  // CHECK: complete group.spawn { 3 }
  // CHECK: next: 3

  // CHECK: task group returning: 6

  // CHECK: result: 6
  print("result: \(sum)")
  assert(sum == expected, "Expected: \(expected), got: \(sum)")
}

@main struct Main {
  static func main() async {
    await test_sum_nextOnPending()
  }
}

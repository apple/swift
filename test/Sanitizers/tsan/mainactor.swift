// RUN: %target-run-simple-swift(-parse-as-library -Xfrontend -enable-experimental-concurrency %import-libdispatch -sanitize=thread) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: tsan_runtime
// UNSUPPORTED: use_os_stdlib

// XFAIL: freebsd

import Dispatch

/// @returns true iff the expected answer is actually the case, i.e., correct.
/// If the current queue does not match expectations, this function may return
/// false or just crash the program with non-zero exit code, depending on SDK.
func checkIfMainQueue(expectedAnswer expected: Bool) -> Bool {
  if #available(macOS 10.12, iOS 10, tvOS 10, watchOS 3, *) {
    dispatchPrecondition(condition: expected ? .onQueue(DispatchQueue.main) 
                                             : .notOnQueue(DispatchQueue.main))
  }
  return true
}

actor A {
  func onCorrectQueue(_ count : Int) -> Int {
    if checkIfMainQueue(expectedAnswer: false) {
      print("on actor instance's queue")
      return count + 1
    }
    print("ERROR: not on actor instance's queue")
    return -10
  }
}

@MainActor func checkAnotherFn(_ count : Int) -> Int {
  if checkIfMainQueue(expectedAnswer: true) {
    print("on main queue again!")
    return count + 1
  } else {
    print("ERROR: left the main queue?")
    return -10
  }
}

@MainActor func enterMainActor(_ initialCount : Int) async -> Int {
  if checkIfMainQueue(expectedAnswer: true) {
    print("hello from main actor!")
  } else {
    print("ERROR: not on correct queue!")
  }

  // try calling a function on another actor.
  let count = await A().onCorrectQueue(initialCount)

  guard checkIfMainQueue(expectedAnswer: true) else {
    print("ERROR: did not switch back to main actor!")
    return -10
  }

  return checkAnotherFn(count) + 1
}

@Sendable func someFunc() async -> Int {
  // NOTE: the "return" counter is just to make sure we're properly returning values.
  // the expected number should be equal to the number of "plus-one" expressions.
  // since there are no loops or duplicate function calls
  return await enterMainActor(0) + 1
}


// CHECK: starting
// CHECK-NOT: ERROR
// CHECK: hello from main actor!
// CHECK-NOT: ERROR
// CHECK: on actor instance's queue
// CHECK-NOT: ERROR
// CHECK: on main queue again!
// CHECK-NOT: ERROR
// CHECK: finished with return counter = 4

@main struct RunIt {
  static func main() async {
    print("starting")
    let result = await someFunc()
    print("finished with return counter = \(result)")
  }
}

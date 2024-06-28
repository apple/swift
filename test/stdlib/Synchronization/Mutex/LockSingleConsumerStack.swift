// RUN: %target-run-simple-swift(%import-libdispatch)
// REQUIRES: executable_test
// REQUIRES: libdispatch
// REQUIRES: synchronization

import Synchronization
import StdlibUnittest
import Dispatch

let suite = TestSuite("LockSingleConsumerStackTests")

@available(SwiftStdlib 6.0, *)
class LockSingleConsumerStack<Element> {
  struct Node {
    let value: Element
    var next: UnsafeMutablePointer<Node>?
  }
  typealias NodePtr = UnsafeMutablePointer<Node>

  private let _last = Mutex<NodePtr?>(nil)
  private let _consumerCount = Mutex<Int>(0)
  private var foo = 0

  deinit {
    // Discard remaining nodes
    while let _ = pop() {}
  }

  // Push the given element to the top of the stack.
  // It is okay to concurrently call this in an arbitrary number of threads.
  func push(_ value: Element) {
    let new = NodePtr.allocate(capacity: 1)
    new.initialize(to: Node(value: value, next: nil))

    _last.withLock {
      new.pointee.next = $0
      $0 = new
    }
  }

  // Pop and return the topmost element from the stack.
  // This method does not support multiple overlapping concurrent calls.
  func pop() -> Element? {
    precondition(
      _consumerCount.withLock {
        let old = $0
        $0 += 1
        return old == 0
      },
      "Multiple consumers detected")

    defer {
      _consumerCount.withLock {
        $0 -= 1
      }
    }

    return _last.withLock { (c: inout NodePtr?) -> Element? in
      guard let current = c else {
        return nil
      }

      c = current.pointee.next

      let result = current.move()
      current.deallocate()
      return result.value
    }
  }
}

if #available(SwiftStdlib 6.0, *) {

suite.test("basics") {
  let stack = LockSingleConsumerStack<Int>()
  expectNil(stack.pop())
  stack.push(0)
  expectEqual(0, stack.pop())

  stack.push(1)
  stack.push(2)
  stack.push(3)
  stack.push(4)
  expectEqual(4, stack.pop())
  expectEqual(3, stack.pop())
  expectEqual(2, stack.pop())
  expectEqual(1, stack.pop())
  expectNil(stack.pop())
}

suite.test("concurrentPushes") {
  let stack = LockSingleConsumerStack<(thread: Int, value: Int)>()

  let numThreads = 100
  let numValues = 10_000
  DispatchQueue.concurrentPerform(iterations: numThreads) { thread in
    for value in 1 ... numValues {
      stack.push((thread: thread, value: value))
    }
  }

  var expected: [Int] = Array(repeating: numValues, count: numThreads)
  while let (thread, value) = stack.pop() {
    expectEqual(expected[thread], value)
    expected[thread] -= 1
  }
  expectEqual(Array(repeating: 0, count: numThreads), expected)
}

suite.test("concurrentPushesAndPops") {
  let stack = LockSingleConsumerStack<(thread: Int, value: Int)>()

  let numThreads = 100
  let numValues = 10_000

  var perThreadSums: [Int] = Array(repeating: 0, count: numThreads)
  let consumerQueue = DispatchQueue(label: "org.swift.background")
  consumerQueue.async {
    var count = 0
    while count < numThreads * numValues {
      // Note: busy wait
      if let (thread, value) = stack.pop() {
        perThreadSums[thread] += value
        count += 1
      }
    }
  }

  DispatchQueue.concurrentPerform(iterations: numThreads + 1) { thread in
    if thread < numThreads {
      // Producers
      for value in 0 ..< numValues {
        stack.push((thread: thread, value: value))
      }
    }
  }

  consumerQueue.sync {
    expectEqual(Array(repeating: numValues * (numValues - 1) / 2, count: numThreads), perThreadSums)
  }
}

} // if #available(SwiftStdlib 6.0)

runAllTests()

// RUN: %target-run-simple-swift(%import-libdispatch)
// REQUIRES: executable_test
// REQUIRES: libdispatch
// REQUIRES: synchronization

import Synchronization
import StdlibUnittest
import Dispatch

let suite = TestSuite("LockFreeSingleConsumerStackTests")

@available(SwiftStdlib 6.0, *)
class LockFreeSingleConsumerStack<Element> {
  struct Node {
    let value: Element
    var next: UnsafeMutablePointer<Node>?
  }
  typealias NodePtr = UnsafeMutablePointer<Node>

  private let _last = Atomic<NodePtr?>(nil)
  private let _consumerCount = Atomic<Int>(0)
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

    var done = false
    var current = _last.load(ordering: .relaxed)
    while !done {
      new.pointee.next = current
      (done, current) = _last.compareExchange(
        expected: current,
        desired: new,
        ordering: .releasing)
    }
  }

  // Pop and return the topmost element from the stack.
  // This method does not support multiple overlapping concurrent calls.
  func pop() -> Element? {
    precondition(
      _consumerCount.wrappingAdd(1, ordering: .acquiring).oldValue == 0,
      "Multiple consumers detected")
    defer { _consumerCount.wrappingSubtract(1, ordering: .releasing) }
    var done = false
    var current = _last.load(ordering: .acquiring)
    while let c = current {
      (done, current) = _last.compareExchange(
        expected: c,
        desired: c.pointee.next,
        ordering: .acquiring)
      if done {
        let result = c.move()
        c.deallocate()
        return result.value
      }
    }
    return nil
  }
}

if #available(SwiftStdlib 6.0, *) {

suite.test("basics") {
  let stack = LockFreeSingleConsumerStack<Int>()
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
  let stack = LockFreeSingleConsumerStack<(thread: Int, value: Int)>()

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
  let stack = LockFreeSingleConsumerStack<(thread: Int, value: Int)>()

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

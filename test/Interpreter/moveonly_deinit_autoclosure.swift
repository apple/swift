// RUN: %target-run-simple-swift
// REQUIRES: executable_test

internal func _myPrecondition(
  _ body: @autoclosure () -> Bool
) {
  guard body() else {
    fatalError("condition failed")
  }
}

var deinitCounter = 0

struct MyCounter<T>: ~Copyable {
  let expectedCount = 1
  let box: T
  init(_ t: T) {
    self.box = t
  }
  deinit {
    print("hello")
    deinitCounter += 1
    _myPrecondition(deinitCounter == self.expectedCount)
  }
}

func test() {
  _ = MyCounter(4343)
}

// CHECK: hello
test()
// CHECK-NEXT: great success
print("great success")

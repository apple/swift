// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest

var suite = TestSuite("Unmanaged")
defer { runAllTests() }

struct Counted: ~Copyable {
  let value: Int
  static var instances: Int = 0

  init(_ value: Int) {
    self.value = value
    Counted.instances += 1
  }

  deinit {
    Counted.instances -= 1
    expectGE(Counted.instances, 0)
  }
}

suite.test("swap.Int") {
  var a = 1
  var b = 2
  swap(&a, &b)
  expectEqual(a, 2)
  expectEqual(b, 1)
}

suite.test("exchange.Int") {
  var a = 1
  let old = exchange(&a, with: 2)
  expectEqual(old, 1)
  expectEqual(a, 2)
}

suite.test("swap.class") {
  var a = LifetimeTracked(10)
  var b = LifetimeTracked(20)
  swap(&a, &b)
  expectEqual(LifetimeTracked.instances, 2)
  expectEqual(a.value, 20)
  expectEqual(b.value, 10)
}

suite.test("exchange.class") {
  var a = LifetimeTracked(10)
  let old = exchange(&a, with: LifetimeTracked(20))
  expectEqual(LifetimeTracked.instances, 2)
  expectEqual(old.value, 10)
  expectEqual(a.value, 20)
}

suite.test("swap.noncopyable") {
  var a = Counted(10)
  var b = Counted(20)
  expectEqual(Counted.instances, 2)
  swap(&a, &b)
  expectEqual(Counted.instances, 2)
  expectEqual(a.value, 20)
  expectEqual(b.value, 10)
  _ = consume a
  _ = consume b
  expectEqual(Counted.instances, 0)
}

suite.test("exchange.noncopyable") {
  var a = Counted(10)
  let old = exchange(&a, with: Counted(20))
  expectEqual(Counted.instances, 2)
  expectEqual(a.value, 20)
  expectEqual(old.value, 10)
  _ = consume old
  _ = consume a
  expectEqual(Counted.instances, 0)
}

// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: synchronization

import Synchronization
import StdlibUnittest

let suite = TestSuite("AtomicLazyReference")

if #available(SwiftStdlib 6.0, *) {

suite.test("creation") {
  expectEqual(LifetimeTracked.instances, 0)

  let v = AtomicLazyReference<LifetimeTracked>()

  defer {
    expectEqual(LifetimeTracked.instances, 0)
  }

  expectNil(v.load())
}

suite.test("storeIfNil") {
  expectEqual(LifetimeTracked.instances, 0)
  do {
    let v = AtomicLazyReference<LifetimeTracked>()
    expectNil(v.load())

    let ref = LifetimeTracked(42)
    expectTrue(v.storeIfNil(ref) === ref)
    expectTrue(v.load() === ref)

    let ref2 = LifetimeTracked(23)
    expectTrue(v.storeIfNil(ref2) === ref)
    expectTrue(v.load() === ref)

    // v is automatically deinitialized here.
  }
  expectEqual(LifetimeTracked.instances, 0)
}

} // if #available(SwiftStdlib 6.0)

runAllTests()

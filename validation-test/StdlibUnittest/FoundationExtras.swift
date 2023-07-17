// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// REQUIRES: objc_interop

import StdlibUnittest


import Foundation
import StdlibUnittestFoundationExtras

var FoundationExtrasTests = TestSuite("FoundationExtras")

@_silgen_name("objc_autorelease")
func objc_autorelease(_ ref: __owned AnyObject)

FoundationExtrasTests.test("objc_autorelease()") {
  autoreleasepool {
    // Check that objc_autorelease indeed autoreleases.
    objc_autorelease(LifetimeTracked(101))
    expectEqual(1, LifetimeTracked.instances)
  }
}

FoundationExtrasTests.test("autoreleasepoolIfUnoptimizedReturnAutoreleased()/autorelease") {
  autoreleasepool {
    autoreleasepoolIfUnoptimizedReturnAutoreleased {
      objc_autorelease(LifetimeTracked(103))
      expectEqual(1, LifetimeTracked.instances)
    }
  }
}

FoundationExtrasTests.test("autoreleasepoolIfUnoptimizedReturnAutoreleased()/return-autoreleased") {
  autoreleasepool {
    autoreleasepoolIfUnoptimizedReturnAutoreleased {
      let nsa = [ LifetimeTracked(104) ] as NSArray
      expectEqual(1, LifetimeTracked.instances)
      _blackHole(nsa[0])
    }
    expectEqual(0, LifetimeTracked.instances)
  }
}

runAllTests()


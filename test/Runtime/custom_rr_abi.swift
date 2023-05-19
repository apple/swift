// RUN: %target-run-simple-swift(-import-objc-header %S/Inputs/custom_rr_abi_utilities.h)

// REQUIRES: CPU=arm64 || CPU=arm64e

// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

// A class that can provider a retainable pointer and determine whether it's
// been retained or released. This creates a helper object that will be retained
// or released. We don't attempt to clean up the helper so it leaks if not released,
// but this is only used for this one test so that's OK.
class RetainReleaseChecker {
  var pointerValue: UnsafeMutableRawPointer

  private class Helper {}

  private weak var weakRef: Helper?

  private let originalRetainCount: UInt

  init() {
    do {
      // Make a helper object, retain it so it stays alive, and put it into
      // pointerValue and weakRef.
      let helper = Helper()
      pointerValue = Unmanaged.passRetained(helper).toOpaque()
      weakRef = helper
    }
    // Record the original retain count before anything happens. Then we can
    // detect changes without needing to know exactly what the count is supposed
    // to be.
    originalRetainCount = _getRetainCount(weakRef!)
  }

  // If helper was retained, then weakRef will still point to it, and the retain
  // count will have increased.
  var retained: Bool {
    weakRef != nil && _getRetainCount(weakRef!) > originalRetainCount
  }

  // weakRef is the only reference we had to the helper, aside from the retain we put
  // on it to create pointerValue. If helper was released, then it will be destroyed
  // and weakRef will be nil.
  var released: Bool {
    weakRef == nil
  }
}

var CustomRRABITestSuite = TestSuite("CustomRRABI")

CustomRRABITestSuite.test("retain") {
  foreachRRFunction { function, cname, register, isRetain in
    let name = String(cString: cname!)
    let fullname = "\(name)_x\(register)"

    // Create a set of RR checker objects.
    var checkers = (0..<NUM_REGS).map{ _ in RetainReleaseChecker() }

    // Fill out a registers array with the pointers from the RR checkers.
    var regs: [UnsafeMutableRawPointer?] = checkers.map{ $0.pointerValue }

    // Call the RR function.
    function!(&regs)

    // Make sure all the checkers report what they're supposed to. All registers
    // aside from `register` should be untouched, and `register` should have been
    // either retained or released.
    for (i, checker) in checkers.enumerated() {
      if i == register {
        if isRetain != 0 {
          expectTrue(checker.retained, "\(fullname) must retain x\(i)")
        } else {
          expectTrue(checker.released, "\(fullname) must release x\(i)")
        }
      } else {
        expectFalse(checker.retained, "\(fullname) must not retain x\(i)")
        expectFalse(checker.released, "\(fullname) must not retain x\(i)")
      }
    }
  }
}

runAllTests()

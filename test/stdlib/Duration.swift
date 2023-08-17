// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// Int128 operations are not supported on 32bit platforms, 128-bit types are not
// provided by the 32-bit LLVM. See `dividingFullWidth` in IntegerTypes.swift.gyb
// UNSUPPORTED: PTRSIZE=32

import StdlibUnittest

var suite = TestSuite("StringIndexTests")
defer { runAllTests() }

if #available(SwiftStdlib 5.7, *) {
  suite.test("seconds from Double") {
    for _ in 0 ..< 100 {
      let integerValue = Double(Int64.random(in: 0 ... 0x7fff_ffff_ffff_fc00))
      let (sec, attosec) = Duration.seconds(integerValue).components
      expectEqual(sec, Int64(integerValue))
      expectEqual(attosec, 0)
    }
    let quarterSecond = Duration.seconds(0.25)
    expectEqual(quarterSecond.components, (0, 250_000_000_000_000_000))
    // Value that overflows conversion from Double -> Int64, but should be
    // representable as a number of seconds:
    let huge: Double = 1.7e20
    let duration = Duration.seconds(huge)
    // Divide by 1000 to get back to a duration with representable components:
    let smallerDuration = duration / 1000
    expectEqual(smallerDuration.components, (170_000_000_000_000_000, 0))
    // Now check that the components of the original value trap:
    expectCrashLater()
    let _ = duration.components
  }
  
  suite.test("milliseconds from Double") {
    for _ in 0 ..< 100 {
      let integerValue = Double(Int64.random(in: 0 ... 0x7fff_ffff_ffff_fc00))
      let (sec, attosec) = Duration.milliseconds(integerValue).components
      expectEqual(sec, Int64(integerValue) / 1000)
      expectEqual(attosec, Int64(integerValue) % 1000 * 1_000_000_000_000_000)
    }
  }
  
  suite.test("microseconds from Double") {
    for _ in 0 ..< 100 {
      let integerValue = Double(Int64.random(in: 0 ... 0x7fff_ffff_ffff_fc00))
      let (sec, attosec) = Duration.microseconds(integerValue).components
      expectEqual(sec, Int64(integerValue) / 1_000_000)
      expectEqual(attosec, Int64(integerValue) % 1_000_000 * 1_000_000_000_000)
    }
  }
}

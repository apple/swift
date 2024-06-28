// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out -Ounchecked
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(WASI)
  import WASILibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif
import StdlibUnittest


var TGMathTestSuite = TestSuite("tgmath")

let minusOneDouble = Double(-1.0)
let minusOneFloat = Float(-1.0)

@inline(never)
func minusOneDoubleFunction() -> Double{
  return minusOneDouble
}

@inline(never)
func minusOneFloatFunction() -> Float {
  return minusOneFloat
}

TGMathTestSuite.test("sqrt") {
  expectTrue(sqrt(minusOneFloat).isNaN)
  expectTrue(sqrt(minusOneFloatFunction()).isNaN)
  expectTrue(sqrt(minusOneDouble).isNaN)
  expectTrue(sqrt(minusOneDoubleFunction()).isNaN)
}

runAllTests()

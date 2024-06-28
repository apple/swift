// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -cxx-interoperability-mode=default)
//
// REQUIRES: executable_test
// REQUIRES: OS=macosx

import StdlibUnittest
import Closure

var ClosureTestSuite = TestSuite("Closure")

ClosureTestSuite.test("ConvertToBlock") {
  cfunc({NonTrivial in})
}

ClosureTestSuite.test("ConvertToBlockARCWeak") {
  cfuncARCWeak({ARCWeak in})
}

runAllTests()

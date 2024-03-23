// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test
//
// XFAIL: *

import Polymorphism
import StdlibUnittest


var PolymorphismTestSuite = TestSuite("Determining if runtime polymorphism works")


PolymorphismTestSuite.test("Call overridden methods with pointer to base type") {
  // MakeShape() creates a Rectangle and returns the object as a Shape*.
  let shape = MakeShape()

  // DOES NOT WORK: executes the Shape implementation, not the Rectangle
  // version, and thus fails the comparison (0 != 4)
  // Filed as https://github.com/apple/swift/issues/62354.
  expectEqual(shape!.pointee.NumberOfSides(), 4)
}

runAllTests()

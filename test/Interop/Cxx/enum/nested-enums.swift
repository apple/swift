// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)

// REQUIRES: executable_test

import NestedEnums
import StdlibUnittest

var NestedEnumsTestSuite = TestSuite("Nested Enums")

NestedEnumsTestSuite.test("Make and compare") {
  let val: ns.EnumInNS = ns.kA
  expectNotEqual(val, ns.kB)
  let valNested = ns.nestedNS.kNestedA
  expectNotEqual(valNested, ns.nestedNS.kNestedB)
}

runAllTests()

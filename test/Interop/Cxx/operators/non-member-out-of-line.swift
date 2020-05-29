// RUN: %empty-directory(%t)
// RUN: %target-clang -c %S/Inputs/non-member-out-of-line.cpp -I %S/Inputs -o %t/non-member-out-of-line.o
// RUN: %target-build-swift %s -I %S/Inputs -o %t/non-member-out-of-line %t/non-member-out-of-line.o -Xfrontend -enable-cxx-interop
// RUN: %target-codesign %t/non-member-out-of-line
// RUN: %target-run %t/non-member-out-of-line
//
// REQUIRES: executable_test

import NonMemberOutOfLine
import StdlibUnittest

var OperatorsTestSuite = TestSuite("Operators")

OperatorsTestSuite.test("plus") {
  let lhs = IntBox(value: 42)
  let rhs = IntBox(value: 23)

  let result = lhs + rhs

  expectEqual(65, result.value)
}

OperatorsTestSuite.test("minus") {
  let lhs = IntBox(value: 42)
  let rhs = IntBox(value: 23)

  let result = lhs - rhs

  expectEqual(19, result.value)
}

OperatorsTestSuite.test("star") {
  let lhs = IntBox(value: 42)
  let rhs = IntBox(value: 23)

  let result = lhs * rhs

  expectEqual(966, result.value)
}

OperatorsTestSuite.test("slash") {
  let lhs = IntBox(value: 42)
  let rhs = IntBox(value: 23)

  let result = lhs / rhs

  expectEqual(1, result.value)
}

runAllTests()

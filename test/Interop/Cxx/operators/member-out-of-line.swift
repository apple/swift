// RUN: %empty-directory(%t)
// RUN: %target-clangxx -c %S/Inputs/member-out-of-line.cpp -I %S/Inputs -o %t/member-out-of-line.o
// RUN: %target-build-swift %s -I %S/Inputs -o %t/member-out-of-line %t/member-out-of-line.o -Xfrontend -enable-experimental-cxx-interop
// RUN: %target-codesign %t/member-out-of-line
// RUN: %target-run %t/member-out-of-line

// REQUIRES: executable_test

import MemberOutOfLine
import StdlibUnittest

var OperatorsTestSuite = TestSuite("Operators")

OperatorsTestSuite.test("LoadableIntWrapper.plus (out-of-line)") {
  let lhs = LoadableIntWrapper(value: 42)
  let rhs = LoadableIntWrapper(value: 23)

  let result = lhs + rhs

  expectEqual(65, result.value)
}

OperatorsTestSuite.test("LoadableIntWrapper.call (out-of-line)") {
  let wrapper = LoadableIntWrapper(value: 42)

  let resultNoArgs = wrapper()
  let resultOneArg = wrapper(23)
  let resultTwoArgs = wrapper(3, 5)

  expectEqual(42, resultNoArgs)
  expectEqual(65, resultOneArg)
  expectEqual(57, resultTwoArgs)
}

OperatorsTestSuite.test("AddressOnlyIntWrapper.call (out-of-line)") {
  let wrapper = AddressOnlyIntWrapper(42)

  let resultNoArgs = wrapper()
  let resultOneArg = wrapper(23)
  let resultTwoArgs = wrapper(3, 5)

  expectEqual(42, resultNoArgs)
  expectEqual(65, resultOneArg)
  expectEqual(57, resultTwoArgs)
}

OperatorsTestSuite.test("ReadWriteIntArray.subscript (out-of-line)") {
  var arr = ReadWriteIntArray()

  let resultBefore = arr[1]
  expectEqual(2, resultBefore)

  arr[1] = 234

  let resultAfter = arr[1]
  expectEqual(234, resultAfter)
}

OperatorsTestSuite.test("NonTrivialIntArrayByVal.subscript (out-of-line)") {
  var arr = NonTrivialIntArrayByVal(1)

  let result0 = arr[0]
  let result2 = arr[2]
  let result4 = arr[4]

  expectEqual(1, result0)
  expectEqual(3, result2)
  expectEqual(5, result4)

  arr.setValueAtIndex(42, 3)
  let result5 = arr[3]
  expectEqual(42, result5)
}

runAllTests()

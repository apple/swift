// RUN: %empty-directory(%t)
// RUN: %target-clangxx -c %S/Inputs/reference.cpp -I %S/Inputs -o %t/reference.o
// RUN: %target-build-swift %s -I %S/Inputs -o %t/reference %t/reference.o -Xfrontend -enable-experimental-cxx-interop
// RUN: %target-codesign %t/reference
// RUN: %target-run %t/reference
//
// REQUIRES: executable_test

import Reference
import StdlibUnittest

var ReferenceTestSuite = TestSuite("Reference")

ReferenceTestSuite.test("read-lvalue-reference") {
  expectNotEqual(13, getStaticInt())
  setStaticInt(13)
  expectEqual(13, getStaticIntRef().pointee)
  expectEqual(13, getConstStaticIntRef().pointee)
}

ReferenceTestSuite.test("read-rvalue-reference") {
  expectNotEqual(32, getStaticInt())
  setStaticInt(32)
  expectEqual(32, getStaticIntRvalueRef().pointee)
  expectEqual(32, getConstStaticIntRvalueRef().pointee)
}

ReferenceTestSuite.test("write-lvalue-reference") {
  expectNotEqual(14, getStaticInt())
  getStaticIntRef().pointee = 14
  expectEqual(14, getStaticInt())
}

ReferenceTestSuite.test("write-rvalue-reference") {
  expectNotEqual(41, getStaticInt())
  getStaticIntRvalueRef().pointee = 41
  expectEqual(41, getStaticInt())
}

ReferenceTestSuite.test("pass-lvalue-reference") {
  expectNotEqual(21, getStaticInt())
  var val: CInt = 21
  setStaticIntRef(&val)
  expectEqual(21, getStaticInt())
  val = 111
  setStaticIntRefTypealias(&val)
  expectEqual(getStaticInt(), 111)
}

ReferenceTestSuite.test("pass-const-lvalue-reference") {
  expectNotEqual(22, getStaticInt())
  let val: CInt = 22
  setConstStaticIntRef(val)
  expectEqual(22, getStaticInt())
  setConstStaticIntRefTypealias(112)
  expectEqual(getStaticInt(), 112)
}

ReferenceTestSuite.test("func-reference") {
  let cxxF: @convention(c) () -> Int32 = getFuncRef()

  expectNotEqual(15, getStaticInt())
  setStaticInt(15)
  expectEqual(15, cxxF())
}

ReferenceTestSuite.test("func-rvalue-reference") {
  let cxxF: @convention(c) () -> Int32 = getFuncRvalueRef()

  expectNotEqual(61, getStaticInt())
  setStaticInt(61)
  expectEqual(61, cxxF())
}

ReferenceTestSuite.test("pod-struct-const-lvalue-reference") {
  expectNotEqual(getStaticInt(), 78)
  takeConstRef(78)
  expectEqual(getStaticInt(), 78)
}

ReferenceTestSuite.test("reference to template") {
  var val: CInt = 53
  let ref = refToTemplate(&val)
  expectEqual(53, ref.pointee)
  ref.pointee = 42
  expectEqual(42, val)
}

ReferenceTestSuite.test("const reference to template") {
  let val: CInt = 53
  let ref = constRefToTemplate(val)
  expectEqual(53, ref.pointee)
}

runAllTests()

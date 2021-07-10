// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -I %S/Inputs/custom-modules/ -o %t/cxx_exceptions -Xfrontend -enable-cxx-interop
// RUN: %target-codesign %t/cxx_exceptions
// RUN: %target-run %t/cxx_exceptions

import CxxExceptions
import StdlibUnittest

var CxxExceptionsTestSuite = TestSuite("CxxExceptions")

// Uncaught C++ exceptions terminate the program.
CxxExceptionsTestSuite.test("UncaughtException") {
  expectCrashLater(withMessage: "terminate called")
  throwException()
}

// Exceptions can be thrown and caught within C++ code if they don't cross any
// interop boundaries. In addition, ClangImporter correctly codegens throw and
// try-catch in a C++ inline function.
CxxExceptionsTestSuite.test("ExceptionCaughtWithinCpp") {
  expectTrue(callAndCatchExceptions(throwException))
}

// Exceptions cannot be thrown across interop boundaries. If while unwinding the
// stack we reach a Swift stack frame, the program terminates.
// FIXME: This test documents the current behavior, which is wrong. Currently,
// exceptions will propagate through Swift code. This is bad, because Swift
// stack frames will be unwound without doing any potentially necessary
// cleanups.
// I'm documenting the wrong behavior instead of making this an XFAIL because I
// want to show exactly how this fails today.
CxxExceptionsTestSuite.test("DontUnwindAcrossSwiftStackFrame") {
  func callThrowException() {
    throwException()
  }
  
  expectTrue(callAndCatchExceptions(callThrowException))
}

runAllTests()

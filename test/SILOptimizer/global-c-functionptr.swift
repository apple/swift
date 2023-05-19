// Testcase for an IRGen crash with function pointers in static globals and multi-threaded compilation.

// First test: check if the compilation succeeds and the code is correct.

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=Test %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// Second test (bonus): check if the optimization is done: statically initialize a global with a C-function pointer

// RUN: %target-build-swift -O -module-name=Test %s -emit-sil | %FileCheck %s -check-prefix=CHECK-SIL

// REQUIRES: executable_test
// REQUIRES: swift_in_compiler


internal func cFn(_ i: Int) -> Int {
  return i + 1
}

public struct S {
  // CHECK-SIL-LABEL: sil_global @$s4Test1SV6cFnPtryS2iXCvpZ : $@convention(c) (Int) -> Int = {
  // CHECK-SIL:         %initval = function_ref @$s4Test3cFnyS2iFTo : $@convention(c) (Int) -> Int
  static public var cFnPtr: @convention(c) (Int) -> Int = cFn
}

func testit() {
  // CHECK-OUTPUT: 28
  print(S.cFnPtr(27))
}

testit()

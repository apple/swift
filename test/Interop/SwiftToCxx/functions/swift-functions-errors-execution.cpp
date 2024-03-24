// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/swift-functions-errors.swift -typecheck -module-name Functions -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr-or-stdlib -enable-experimental-feature GenerateBindingsForThrowingFunctionsInCXX -emit-clang-header-path %t/functions.h

// RUN: %target-interop-build-clangxx -c %s -I %t -o %t/swift-functions-errors-execution.o -DSWIFT_CXX_INTEROP_EXPERIMENTAL_SWIFT_ERROR
// RUN: %target-interop-build-swift %S/swift-functions-errors.swift -o %t/swift-functions-errors-execution -Xlinker %t/swift-functions-errors-execution.o -module-name Functions -Xfrontend -entry-point-function-name -Xfrontend swiftMain -enable-experimental-feature GenerateBindingsForThrowingFunctionsInCXX

// RUN: %target-codesign %t/swift-functions-errors-execution
// RUN: %target-run %t/swift-functions-errors-execution | %FileCheck %s

// REQUIRES: executable_test
// UNSUPPORTED: OS=windows-msvc

// rdar://102167469
// UNSUPPORTED: CPU=arm64e

// for experimental feature GenerateBindingsForThrowingFunctionsInCXX:
// REQUIRES: asserts

#include <cassert>
#include <cstdio>
#include "functions.h"

int main() {
  static_assert(!noexcept(Functions::emptyThrowFunction()), "noexcept function");
  static_assert(!noexcept(Functions::throwFunction()), "noexcept function");
  static_assert(!noexcept(Functions::throwFunctionWithReturn()), "noexcept function");

  try {
    Functions::emptyThrowFunction();
  } catch (swift::Error& e) {
    printf("Exception\n");
  }
  try {
    Functions::throwFunction();
  } catch (swift::Error& e) {
      auto errorOpt = e.as<Functions::NaiveErrors>();
      assert(errorOpt.isSome());

      auto errorVal = errorOpt.get();
      assert(errorVal == Functions::NaiveErrors::throwError);
      errorVal.getMessage();
  }
  try {
    Functions::throwFunctionWithReturn();
  } catch (swift::Error& e) {
     printf("Exception\n");
  }
  try {
    Functions::throwFunctionWithNeverReturn();
  } catch (swift::Error& e) {
     printf("Exception\n");
  }
  try {
    Functions::testDestroyedError();
  } catch(const swift::Error &e) { }

  return 0;
}

// CHECK: passEmptyThrowFunction
// CHECK-NEXT: passThrowFunction
// CHECK-NEXT: throwError
// CHECK-NEXT: passThrowFunctionWithReturn
// CHECK-NEXT: Exception
// CHECK-NEXT: passThrowFunctionWithNeverReturn
// CHECK-NEXT: Exception
// CHECK-NEXT: Test destroyed

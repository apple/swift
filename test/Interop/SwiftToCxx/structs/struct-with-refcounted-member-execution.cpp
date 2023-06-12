// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/struct-with-refcounted-member.swift -typecheck -module-name Structs -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h

// RUN: %target-interop-build-clangxx -c %s -I %t -o %t/swift-structs-execution.o
// RUN: %target-interop-build-swift %S/struct-with-refcounted-member.swift -o %t/swift-structs-execution -Xlinker %t/swift-structs-execution.o -module-name Structs -Xfrontend -entry-point-function-name -Xfrontend swiftMain

// RUN: %target-codesign %t/swift-structs-execution
// RUN: %target-run %t/swift-structs-execution | %FileCheck %s

// REQUIRES: executable_test

#include <assert.h>
#include "structs.h"

int main() {
  using namespace Structs;

  // Ensure that the value destructor is called.
  {
    StructWithRefcountedMember value = returnNewStructWithRefcountedMember();
  }
  printBreak(1);
// CHECK:      create RefCountedClass
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: breakpoint 1

  {
    StructWithRefcountedMember value = returnNewStructWithRefcountedMember();
    StructWithRefcountedMember copyValue(value);
  }
  printBreak(2);
// CHECK-NEXT: create RefCountedClass
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: breakpoint 2

  {
    StructWithRefcountedMember value = returnNewStructWithRefcountedMember();
    StructWithRefcountedMember value2(returnNewStructWithRefcountedMember());
  }
  printBreak(3);
// CHECK-NEXT: create RefCountedClass
// CHECK-NEXT: create RefCountedClass
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: breakpoint 3

  {
    StructWithRefcountedMember value = returnNewStructWithRefcountedMember();
    StructWithRefcountedMember value2 = returnNewStructWithRefcountedMember();
    value = value2;
    printBreak(4);
  }
  printBreak(5);
// CHECK-NEXT: create RefCountedClass
// CHECK-NEXT: create RefCountedClass
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: breakpoint 4
// CHECK-NEXT: destroy RefCountedClass
// CHECK-NEXT: breakpoint 5
  return 0;
}

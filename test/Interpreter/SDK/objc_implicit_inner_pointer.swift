// RUN: %empty-directory(%t)
// RUN: %target-clang -fobjc-arc %S/Inputs/objc_implicit_inner_pointer/objc_implicit_inner_pointer.m -c -o %t/objc_implicit_inner_pointer.o
// RUN: %target-build-swift %s -import-objc-header %S/Inputs/objc_implicit_inner_pointer/objc_implicit_inner_pointer.h %t/objc_implicit_inner_pointer.o -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main 2>&1 | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation

_ = JSONDecoder()

func doit() {
  do {
    let x = Foo().bar().takeUnretainedValue()
    // CHECK: -[Foo dealloc]
    printNullableRef(x) // CHECK: 1234567891
  }

  do {
    let y = Foo().nullabar()!.takeUnretainedValue()
    // CHECK: -[Foo dealloc]
    printNonnullRef(y) // CHECK: 1234567891
  }
}

doit()

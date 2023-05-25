// Testcase for an IRGen crash with function pointers in static globals and multi-threaded compilation.

// First test: check if the compilation succeeds and the code is correct.

// RUN: %empty-directory(%t) 
// RUN: touch %t/empty.swift
// RUN: %target-swift-frontend -O -parse-as-library -num-threads 2 -emit-module -emit-module-path=%t/Test.swiftmodule -module-name=Test %s %t/empty.swift -c -o %t/test.o -o %t/empty.o
// RUN: %target-build-swift -O -wmo -module-name=Main -I%t %S/Inputs/global-functionptr-main.swift -c -o %t/main.o
// RUN: %target-swiftc_driver %t/main.o %t/test.o %t/empty.o -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// Second test (bonus): check if the optimization is done: statically initialize the array of function pointers.

// RUN: %target-build-swift -O -wmo -parse-as-library -module-name=Test %s -emit-sil | %FileCheck %s -check-prefix=CHECK-SIL

// REQUIRES: executable_test
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler

internal protocol P {
  init()
}

private struct FuncPtr {
  let cl: () -> Void

  init<C: P>(_: C.Type) {
    self.cl = { _ = C.init() }
  }
}

public struct S: P {
  init() {
    print("init S")
  }
}

// CHECK-SIL-LABEL: sil_global private @$s4Test8funcPtrs{{.*}}_WZTv_ : $_ContiguousArrayStorage<FuncPtr> = {
// CHECK-SIL: %{{[0-9]+}} = function_ref @$s4Test7FuncPtr{{.*}}Tg5 : $@convention(thin) () -> ()
// CHECK-SIL: %initval = object $_ContiguousArrayStorage<FuncPtr> ({{%[0-9]+}} : $_ArrayBody, [tail_elems] {{%[0-9]+}} : $FuncPtr, {{%[0-9]+}} : $FuncPtr)
private let funcPtrs = [
  FuncPtr(S.self),
  FuncPtr(S.self)]

public func testit(_ i: Int) {
  // CHECK-OUTPUT: init S
  funcPtrs[i].cl()
}


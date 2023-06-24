// RUN: rm -rf %t
// RUN: split-file %s %t

// RUN: %target-swift-ide-test -print-module -module-to-print=Test -I %t/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop -verify

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

class TrivialABIRecord {
    int x = 0;
public:
    TrivialABIRecord() {}
    ~TrivialABIRecord() {
    }
}
__attribute__((trivial_abi));

// CHECK: @available(*, unavailable, message: "C++ classes with `trivial_abi` Clang attribute are not yet available in Swift")
// CHECK-NEXT: struct TrivialABIRecord {

//--- test.swift

import Test

func test() {
    let _ = TrivialABIRecord() // expected-error{{'TrivialABIRecord' is unavailable: C++ classes with `trivial_abi` Clang attribute are not yet available in Swift}}
}

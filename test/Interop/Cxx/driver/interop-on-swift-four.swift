// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: not %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop -swift-version 4 2>&1 | %FileCheck %s

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

inline long test() { return 42; }

//--- test.swift

import Test

// CHECK: error: Swift language mode must be set to '5' to be able to use C++ interoperability.
public func caller() -> Int {
    test()
}

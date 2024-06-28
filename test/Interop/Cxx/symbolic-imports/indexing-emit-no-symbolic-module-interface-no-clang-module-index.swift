// RUN: %empty-directory(%t)
// RUN: split-file %s %t
//
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-ignore-clang-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck %s
// RUN: not ls %t/store/interfaces

//--- Inputs/module.modulemap
module CxxModule {
    header "headerA.h"
    requires cplusplus
}

//--- Inputs/headerA.h

namespace ns {
    int freeFunction(int x, int y);
}

//--- test.swift

import CxxModule

// CHECK-NOT: emitting symbolic interface at

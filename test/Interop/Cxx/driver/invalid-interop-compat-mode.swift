// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: not %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -cxx-interoperability-mode=swift-5.8 2>&1 | %FileCheck %s

// Note: swift-5.9 is still supported, but will be removed.
// RUN: %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -cxx-interoperability-mode=swift-5.9

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

//--- test.swift

import Test

// CHECK: error: invalid value 'swift-5.8' in '-cxx-interoperability-mode=swift-5.8'
// CHECK: note: valid arguments to '-cxx-interoperability-mode=' are 'off', 'default'

// RUN: %empty-directory(%t/Inputs)
// RUN: %empty-directory(%t/Inputs/objcfail)
// RUN: split-file %s %t/Inputs

//--- objcfail/objcfail.h

#ifdef FAIL
#error some error from Clang module

// We only record the first error emitted, so we ignore this one.
#error another error from Clang module
#endif

void foo(void);

//--- objcfail/module.modulemap

module ObjCFail {
  header "objcfail.h"
  export *
}

//--- Library.swift

import ObjCFail

// First try printing the interface of the Clang module directly.

// RUN: %sourcekitd-test -req=interface-gen -module ObjCFail -- -I %t/Inputs/objcfail -target %target-triple %s | %FileCheck --check-prefix DIRECT-SUCCESS %s
// DIRECT-SUCCESS: public func foo()

// RUN: not %sourcekitd-test -req=interface-gen -module ObjCFail -- -Xcc -DFAIL -I %t/Inputs/objcfail -target %target-triple %s 2>&1 | %FileCheck --check-prefix DIRECT-FAIL %s
// DIRECT-FAIL: Could not load module: ObjCFail (could not build {{Objective-C|C}} module 'ObjCFail', some error from Clang module)

// Now try doing it transitively

// RUN: %target-swift-frontend -emit-module %t/Inputs/Library.swift -I %t/Inputs/objcfail -module-name Library -o %t

// RUN: %sourcekitd-test -req=interface-gen -module Library -- -I %t -target %target-triple %s | %FileCheck --check-prefix TRANSITIVE-SUCCESS %s
// TRANSITIVE-SUCCESS: import ObjCFail

// RUN: not %sourcekitd-test -req=interface-gen -module Library -- -Xcc -DFAIL -I %t -target %target-triple %s 2>&1 | %FileCheck --check-prefix TRANSITIVE-FAIL %s
// TRANSITIVE-FAIL: Could not load module: Library (could not build {{Objective-C|C}} module 'ObjCFail', some error from Clang module)

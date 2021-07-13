// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/Modules/SomeModule.swiftmodule
// RUN: echo 'DUMMY' > %t/Modules/SomeModule.swiftmodule/i386.swiftmodule
// RUN: not %target-swift-frontend -typecheck -I %t/Modules %s 2>&1 | %FileCheck %s

// REQUIRES: CPU=x86_64

// Testing 'canImport()' not emitting error in inactive #if .. #endif blocks.

#if false
#if canImport(SomeModule) // Ok
// CHECK-NOT: :[[@LINE-1]]:
#endif
#endif

#if true
#else
#if canImport(SomeModule) // Ok
// CHECK-NOT: :[[@LINE-1]]:
#endif
#endif

#if canImport(SomeModule)
// CHECK: :[[@LINE-1]]:15: error: could not find module 'SomeModule' for target '{{.*}}'; found: i386
#endif

import SomeModule

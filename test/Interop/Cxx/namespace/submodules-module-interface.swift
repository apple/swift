// RUN: %target-swift-ide-test -print-module -module-to-print=Submodules.SubmoduleA -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s -check-prefix=CHECK-A
// RUN: %target-swift-ide-test -print-module -module-to-print=Submodules.SubmoduleB -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s -check-prefix=CHECK-B

// CHECK-A-NOT: extension
// CHECK-A: extension NS1 {
// CHECK-A:   struct BasicA {
// CHECK-A:   }
// CHECK-A: }
// CHECK-A-NOT: extension

// CHECK-A: extension NS1.NS2 {
// CHECK-A:   struct BasicDeepA {
// CHECK-A:   }
// CHECK-A: }
// CHECK-A-NOT: extension

// CHECK-B-NOT: extension
// CHECK-B: extension NS1 {
// CHECK-B:   struct BasicB {
// CHECK-B:   }
// CHECK-B: }
// CHECK-B-NOT: extension

// CHECK-B: extension NS1.NS2 {
// CHECK-B:   struct BasicDeepB {
// CHECK-B:   }
// CHECK-B: }
// CHECK-B-NOT: extension

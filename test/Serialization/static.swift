// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -o %t %s
// RUN: llvm-bcanalyzer -dump %t/static.swiftmodule | %FileCheck %s -check-prefix CHECK -check-prefix DYNAMIC

// RUN: %target-swift-frontend -static -emit-module -o %t %s
// RUN: llvm-bcanalyzer -dump %t/static.swiftmodule | %FileCheck %s -check-prefix CHECK -check-prefix STATIC

// CHECK: <MODULE_BLOCK {{.*}}>
// CHECK-STATIC: <IS_STATIC abbrevid={{[0-9]+}}/>
// CHECK-DYNAMIC-NOT: <IS_STATIC abbrevid={{[0-9]+}}/>
// CHECK: </MODULE_BLOCK>

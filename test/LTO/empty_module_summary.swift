// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/empty.swiftmodule.summary -module-name empty %s
// RUN: llvm-bcanalyzer -dump %t/empty.swiftmodule.summary | %FileCheck %s -check-prefix BCANALYZER

// BCANALYZER-NOT: UnknownCode

// RUN: %swift-module-summary-test --to-yaml %t/empty.swiftmodule.summary -o - | %FileCheck %s

// CHECK: module_name:     empty

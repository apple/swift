// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/empty.swiftmodulesummary -module-name empty %s
// RUN: llvm-bcanalyzer -dump %t/empty.swiftmodulesummary | %FileCheck %s -check-prefix BCANALYZER

// BCANALYZER-NOT: UnknownCode

// RUN: %swift-module-summary-test --to-yaml %t/empty.swiftmodulesummary -o - | %FileCheck %s

// CHECK: module_name:     empty

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface %s -o %t/deps.json -sdk %t/mysecretsdk.sdk

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

func foo() { print(1) }

// CHECK: "-sdk",
// CHECK-NEXT: mysecretsdk.sdk

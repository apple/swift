// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -scan-dependencies %s -I %S\\Inputs -o - | %validate-json | %FileCheck %s

// We want to explicitly use the Windows path separator
// REQUIRES: OS=windows-msvc

import A

// CHECK:        "modulePath": "escaped.swiftmodule",
// CHECK-NEXT:   "sourceFiles": [
// CHECK-NEXT:      "{{.*}}\\test\\ScanDependencies\\escaped.swift"
// CHECK-NEXT:   ],

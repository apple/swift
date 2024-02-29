// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -Xcc -fobjc-disable-direct-methods-for-testing
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import C

// CHECK: "mainModuleName": "deps"
/// --------Main module
// CHECK-LABEL: "modulePath": "deps.swiftmodule",
// CHECK-NEXT: sourceFiles
// CHECK-NEXT: ObjCStrict.swift
// CHECK-NEXT: ],
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:     "clang": "C"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-DAG:     "swift": "_Concurrency"
// CHECK: ],

// CHECK:     "swift": "A"
// CHECK:        "swift": {
// CHECK-NEXT:          "moduleInterfacePath": "{{.*}}{{/|\\}}Inputs{{/|\\}}Swift{{/|\\}}A.swiftinterface",
// CHECK:          "commandLine": [
// CHECK:            "-fobjc-disable-direct-methods-for-testing"
// CHECK:            "-o",
// CHECK-NEXT:       "{{.*}}{{/|\\}}A-{{.*}}.swiftmodule",

            


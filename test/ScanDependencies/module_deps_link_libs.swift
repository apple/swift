// RUN: %empty-directory(%t)
// REQUIRES: objc_interop

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

import C
import E
import G
import SubE

// CHECK: "mainModuleName": "deps"

// CHECK:       "linkLibraries": [
// CHECK-DAG:           "linkName": "objc",
// CHECK-NEXT:          "isFramework": false,
// CHECK-NEXT:          "shouldForceLoad": false

// CHECK-DAG:           "linkName": "swiftyLibE",
// CHECK-NEXT:          "isFramework": false,
// CHECK-NEXT:          "shouldForceLoad": true

// CHECK-DAG:           "linkName": "nonSwiftyLibC",
// CHECK-NEXT:          "isFramework": true,
// CHECK-NEXT:          "shouldForceLoad": false

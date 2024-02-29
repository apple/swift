// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface %s -o %t/deps.json -emit-dependencies -emit-dependencies-path %t/deps.d -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -F %S/Inputs/Frameworks
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -test-dependency-scan-cache-serialization %s -o %t/deps.json -emit-dependencies -emit-dependencies-path %t/deps.d -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -F %S/Inputs/Frameworks
// RUN: %validate-json %t/deps.json | %FileCheck %s

import ScannerTestKit

// CHECK: "mainModuleName": "deps"
// CHECK: directDependencies
// CHECK-DAG: "swift": "ScannerTestKit"
// CHECK-DAG: "swift": "Swift"
// CHECK-DAG: "swift": "SwiftOnoneSupport"
// CHECK: ],

// CHECK: "isFramework": true

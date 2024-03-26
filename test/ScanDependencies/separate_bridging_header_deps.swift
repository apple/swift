// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 5
// RUN: %validate-json %t/deps.json | %FileCheck %s

import E

// CHECK: "swift": "deps"
// CHECK: "directDependencies": [
// CHECK-NEXT: {
// CHECK-DAG:          "swift": "E"
// CHECK-DAG:          "swift": "Swift"
// CHECK-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-DAG:          "swift": "_Concurrency"
// CHECK-DAG:          "clang": "_SwiftConcurrencyShims"
// CHECK-DAG:          "swift": "_StringProcessing"
// The source of dependency on clang:F is the bridging header, ensure it is captured here
// CHECK-DAG:          "clang": "F"


// CHECK: "bridgingHeader": {
// CHECK-NEXT:             "path": "{{.*}}Bridging.h",
// CHECK-NEXT:            "sourceFiles": [
// CHECK-NEXT:              "{{.*}}Bridging.h",
// CHECK-NEXT:              "{{.*}}BridgingOther.h"
// CHECK-NEXT:            ],
// CHECK-NEXT:            "moduleDependencies": [
// CHECK-NEXT:              "F"

// CHECK:      "swiftOverlayDependencies": [
// CHECK-NEXT:   {
// CHECK-NEXT:      "swift": "F"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

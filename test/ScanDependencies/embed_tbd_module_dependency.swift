// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -swift-version 4 -embed-tbd-for-module E
// RUN: %validate-json %t/deps.json | %FileCheck %s

// CHECK: "mainModuleName": "deps"
// CHECK-NEXT:  "modules": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "swift": "deps"
// CHECK-NEXT:    },
// CHECK:      "directDependencies": [

// Ensure there is a dependency on 'E' even though it is not explicitly imported
// CHECK:          "swift": "E"


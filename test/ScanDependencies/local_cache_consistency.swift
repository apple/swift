// REQUIRES: executable_test
// REQUIRES: objc_interop
// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// Run the scanner once, ensuring CoreFoundation dependencies are as expected
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -swift-version 4
// RUN: %validate-json %t/deps.json | %FileCheck %s

import CoreFoundation

// CHECK: "clang": "CoreFoundation"

// CHECK:       "directDependencies": [
// CHECK:         {
// CHECK:           "clang": "Darwin"
// CHECK:         },
// CHECK:         {
// CHECK:           "clang": "Dispatch"
// CHECK:         }
// CHECK:       ],

// Make sure the transitive dependency on os_object is present
// CHECK:       "clang": "os_object"

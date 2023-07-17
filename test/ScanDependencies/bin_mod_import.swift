// RUN: %empty-directory(%t)

import EWrapper

// Step 1: Put the textual interface for E in the right place
// RUN: cp %S/Inputs/Swift/E.swiftinterface %t/E.swiftinterface
// Step 1: Build a swift interface into a binary module
// RUN: %target-swift-frontend -compile-module-from-interface %S/Inputs/Swift/EWrapper.swiftinterface -o %t/EWrapper.swiftmodule -I %t
// Step 3: scan dependency should give us the binary module and a textual swift dependency from it
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Step 4: Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -test-dependency-scan-cache-serialization %s -o %t/deps.json -I %t
// RUN: %validate-json %t/deps.json | %FileCheck %s

// CHECK: "modulePath": "{{.*}}EWrapper.swiftmodule"
// CHECK-NEXT: "directDependencies": [
// CHECK-NEXT:   {
// CHECK-DAG:     "swift": "E"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -test-dependency-scan-cache-serialization -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import EWrapper
import SubEWrapper

// CHECK:  "directDependencies": [
// CHECK-DAG:   "swift": "EWrapper"
// CHECK-DAG:   "swift": "F"
// CHECK-DAG:   "swift": "SubEWrapper"
// CHECK-DAG:   "swift": "Swift"
// CHECK-DAG:   "swift": "SwiftOnoneSupport"
// CHECK-DAG:   "swift": "_Concurrency"
// CHECK-DAG:   "swift": "_StringProcessing"
// CHECK-DAG:   "swift": "_cross_import_E"
// CHECK-DAG:   "clang": "_SwiftConcurrencyShims"
// CHECK: ],

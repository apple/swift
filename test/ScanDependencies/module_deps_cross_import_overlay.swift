// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -I %S/Inputs/CHeaders/ExtraCModules -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4 -module-name CrossImportTestModule
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -test-dependency-scan-cache-serialization -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -I %S/Inputs/CHeaders/ExtraCModules -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4 -module-name CrossImportTestModule
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

@_exported import CrossImportTestModule
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
// CHECK-DAG:   "clang": "CrossImportTestModule"
// Ensure a transitive dependency via "_cross_import_E" is not a direct dep of main module
// CHECK-NOT:   "clang": "X"
// CHECK: ],

// Ensure a transitive dependency via "_cross_import_E" is recorded in the graph still
// CHECK:   "clang": "X"

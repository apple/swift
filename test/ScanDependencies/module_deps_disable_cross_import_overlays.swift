// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -I %S/Inputs/CHeaders/ExtraCModules -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -module-name CrossImportTestModule -disable-cross-import-overlays

// Check the contents of the JSON output
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
// CHECK-DAG:   "clang": "_SwiftConcurrencyShims"
// CHECK-DAG:   "clang": "CrossImportTestModule"

// Ensure the cross-import overlay is not detected with -disable-cross-import-overlays
// CHECK-NOT:   "swift": "_cross_import_E"


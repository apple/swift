// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// This test ensures that subsequent invocations of the dependency scanner that re-use previous cache state do not re-use cache entries that contain modules found outside of the current scanner invocation's search paths.

// Run the scanner once, emitting the serialized scanner cache, with one set of search paths
// RUN: %target-swift-frontend -scan-dependencies -serialize-dependency-scan-cache -dependency-scan-cache-path %t/cache.moddepcache -module-cache-path %t/clang-module-cache %s -o %t/deps_initial.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// RUN: %validate-json %t/deps_initial.json &>/dev/null
// RUN: %FileCheck -input-file %t/deps_initial.json %s -check-prefix CHECK-INITIAL-SCAN

// Run the scanner again, but now re-using previously-serialized cache and using a different search path for Swift modules
// RUN: %target-swift-frontend -scan-dependencies -load-dependency-scan-cache -dependency-scan-cache-path %t/cache.moddepcache -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/SwiftDifferent -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// RUN: %validate-json %t/deps.json &>/dev/null
// RUN: %FileCheck -input-file %t/deps.json %s -check-prefix CHECK-DIFFERENT

// REQUIRES: executable_test
// REQUIRES: objc_interop

import A

// CHECK-INITIAL-SCAN:           "modulePath": "{{.*}}{{/|\\}}A-{{.*}}.swiftmodule",
// CHECK-INITIAL-SCAN-NEXT:      "sourceFiles": [
// CHECK-INITIAL-SCAN-NEXT:      ],
// CHECK-INITIAL-SCAN-NEXT:      "directDependencies": [
// CHECK-INITIAL-SCAN-NEXT:        {
// CHECK-INITIAL-SCAN-DAG:          "clang": "A"
// CHECK-INITIAL-SCAN-DAG:          "swift": "Swift"
// CHECK-INITIAL-SCAN-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-INITIAL-SCAN:      ],
// CHECK-INITIAL-SCAN-NEXT:      "details": {
// CHECK-INITIAL-SCAN-NEXT:        "swift": {
// CHECK-INITIAL-SCAN-NEXT:          "moduleInterfacePath": "{{.*}}/Swift/A.swiftinterface",

// CHECK-DIFFERENT:           "modulePath": "{{.*}}{{/|\\}}A-{{.*}}.swiftmodule",
// CHECK-DIFFERENT-NEXT:      "sourceFiles": [
// CHECK-DIFFERENT-NEXT:      ],
// CHECK-DIFFERENT-NEXT:      "directDependencies": [
// CHECK-DIFFERENT-NEXT:        {
// CHECK-DIFFERENT-DAG:          "swift": "Swift"
// CHECK-DIFFERENT-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-DIFFERENT:      ],
// CHECK-DIFFERENT-NEXT:      "details": {
// CHECK-DIFFERENT-NEXT:        "swift": {
// CHECK-DIFFERENT-NEXT:          "moduleInterfacePath": "{{.*}}/SwiftDifferent/A.swiftinterface",

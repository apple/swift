// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop


import F

// CHECK: "mainModuleName": "deps"

/// --------Main module
// CHECK-LABEL: "modulePath": "deps.swiftmodule",
// CHECK-NEXT: sourceFiles
// CHECK-NEXT: explicit-swift-dependencies.swift
// CHECK-NEXT: ],
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:     "swift": "F"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-DAG:     "swift": "_Concurrency"
// CHECK-DAG:     "clang": "_SwiftConcurrencyShims"
// CHECK-DAG:     "swift": "_StringProcessing"
// CHECK: ],

/// --------Swift module F
// CHECK: "modulePath": "{{.*}}{{/|\\}}F-{{.*}}.swiftmodule",
// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-DAG:   "clang": "F"
// CHECK-DAG:   "swift": "Swift"
// CHECK-DAG:   "swift": "SwiftOnoneSupport"


// CHECK: "commandLine": [
// CHECK-NEXT:            "-frontend",
// CHECK-NEXT:            "-compile-module-from-interface",
// CHECK-NEXT:            "-target",
// ...
// CHECK:                 "-explicit-interface-module-build",
// CHECK-NEXT:            "-disable-implicit-swift-modules",
// CHECK-NEXT:            "-Xcc",
// CHECK-NEXT:            "-fno-implicit-modules",
// CHECK-NEXT:            "-Xcc",
// CHECK-NEXT:            "-fno-implicit-module-maps",
// CHECK-NEXT:            "-o",
// CHECK-NEXT:            "{{.*}}{{/|\\}}F-{{.*}}.swiftmodule"
// CHECK-DAG:             "-swift-module-file=Swift={{.*}}{{/|\\}}Swift-{{.*}}.swiftmodule"
// CHECK-DAG:             "-swift-module-file=SwiftOnoneSupport={{.*}}{{/|\\}}SwiftOnoneSupport-{{.*}}.swiftmodule"
// CHECK-DAG:             "-fmodule-file=F={{.*}}{{/|\\}}F-{{.*}}.pcm",
// CHECK-DAG:             "-fmodule-file=SwiftShims={{.*}}{{/|\\}}SwiftShims-{{.*}}.pcm",

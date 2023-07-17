// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/inputs

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"SomeExternalModule\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/t/inputs/SomeExternalModule.swiftmodule\"," >> %/t/inputs/map.json
// RUN: echo "\"docPath\": \"%/t/inputs/SomeExternalModule.swiftdoc\"," >> %/t/inputs/map.json
// RUN: echo "\"sourceInfoPath\": \"%/t/inputs/SomeExternalModule.swiftsourceinfo\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -placeholder-dependency-module-map-file %t/inputs/map.json -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Check the make-style dependencies file
// RUN: %FileCheck %s -check-prefix CHECK-MAKE-DEPS < %t/deps.d

// Check that the JSON parses correctly into the canonical Swift data
// structures.

// RUN: mkdir -p %t/PrintGraph
// RUN: cp %S/Inputs/PrintGraph.swift %t/main.swift
// RUN: %target-build-swift %S/Inputs/ModuleDependencyGraph.swift %t/main.swift -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main %t/deps.json

// Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -test-dependency-scan-cache-serialization -module-cache-path %t/clang-module-cache %s -placeholder-dependency-module-map-file %t/inputs/map.json -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop
import SomeExternalModule

// CHECK: "mainModuleName": "deps"

/// --------Main module
// CHECK-LABEL: "modulePath": "deps.swiftmodule",
// CHECK-NEXT: sourceFiles
// CHECK-NEXT: module_deps_external.swift

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-DAG: "swift": "F"
// CHECK-DAG: "swiftPlaceholder": "SomeExternalModule"
// CHECK-DAG: "swift": "Swift"
// CHECK-DAG: "swift": "SwiftOnoneSupport"
// CHECK-DAG: "swift": "_Concurrency"
// CHECK-DAG: "swift": "_StringProcessing"
// CHECK-DAG: "clang": "_SwiftConcurrencyShims"
// CHECK: ],

// CHECK:      "extraPcmArgs": [
// CHECK-NEXT:    "-Xcc",
// CHECK-NEXT:    "-target",
// CHECK-NEXT:    "-Xcc",
// CHECK:         "-fapinotes-swift-version=4"

// CHECK: "bridgingHeader":
// CHECK-NEXT: "path":
// CHECK-SAME: Bridging.h

// CHECK-NEXT: "sourceFiles":
// CHECK-NEXT: Bridging.h
// CHECK-NEXT: BridgingOther.h

// CHECK: "moduleDependencies": [
// CHECK-NEXT: "F"
// CHECK-NEXT: ]

/// --------Swift module Swift
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}Swift-{{.*}}.swiftmodule",

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-NEXT: "clang": "SwiftShims"

/// --------Clang module SwiftShims
// CHECK-LABEL: "modulePath": "{{.*}}/SwiftShims-{{.*}}.pcm",

// Check make-style dependencies
// CHECK-MAKE-DEPS: module_deps_external.swift
// CHECK-MAKE-DEPS-SAME: Bridging.h
// CHECK-MAKE-DEPS-SAME: BridgingOther.h
// CHECK-MAKE-DEPS-SAME: module.modulemap

/// --------Swift external module SomeExternalModule
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}SomeExternalModule.swiftmodule",
// CHECK-NEXT: "details": {
// CHECK-NEXT: "swiftPlaceholder": {
// CHECK-NEXT: "moduleDocPath": "BUILD_DIR/{{.*}}/ScanDependencies/Output/module_deps_external.swift.tmp/inputs/SomeExternalModule.swiftdoc",
// CHECK-NEXT: "moduleSourceInfoPath": "BUILD_DIR/{{.*}}/ScanDependencies/Output/module_deps_external.swift.tmp/inputs/SomeExternalModule.swiftsourceinfo"

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck -check-prefix CHECK_NO_CLANG_TARGET %s

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix CHECK-NO-SEARCH-PATHS

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
// RUN: %target-swift-frontend -scan-dependencies -test-dependency-scan-cache-serialization -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4
// RUN: %validate-json %t/deps.json | %FileCheck -check-prefix CHECK_NO_CLANG_TARGET %s

// Ensure that scanning with `-clang-target` makes sure that Swift modules' respective PCM-dependency-build-argument sets do not contain target triples.
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps_clang_target.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4 -clang-target %target-cpu-apple-macosx10.14
// Check the contents of the JSON output
// RUN: %validate-json %t/deps_clang_target.json | %FileCheck -check-prefix CHECK_CLANG_TARGET %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import C
import E
import G
import SubE

// CHECK: "mainModuleName": "deps"

/// --------Main module
// CHECK-LABEL: "modulePath": "deps.swiftmodule",
// CHECK-NEXT: sourceFiles
// CHECK-NEXT: module_deps.swift
// CHECK-NEXT: ],
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:     "swift": "A"
// CHECK-DAG:     "clang": "C"
// CHECK-DAG:     "swift": "E"
// CHECK-DAG:     "swift": "F"
// CHECK-DAG:     "swift": "G"
// CHECK-DAG:     "swift": "SubE"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-DAG:     "swift": "_Concurrency"
// CHECK-DAG:     "swift": "_cross_import_E"
// CHECK: ],

// CHECK:      "extraPcmArgs": [
// CHECK-NEXT:    "-Xcc",
// CHECK-NEXT:    "-target",
// CHECK-NEXT:    "-Xcc",
// CHECK:         "-fapinotes-swift-version=4"
// CHECK-NOT: "error: cannot open Swift placeholder dependency module map from"
// CHECK: "bridgingHeader":
// CHECK-NEXT: "path":
// CHECK-SAME: Bridging.h

// CHECK-NEXT: "sourceFiles":
// CHECK-NEXT: Bridging.h
// CHECK-NEXT: BridgingOther.h

// CHECK: "moduleDependencies": [
// CHECK-NEXT: "F"
// CHECK-NEXT: ]

/// --------Swift module A
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}A-{{.*}}.swiftmodule",

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-DAG:   "clang": "A"
// CHECK-DAG:   "swift": "Swift"
// CHECK-NEXT: },

/// --------Clang module C
// CHECK-LABEL: "modulePath": "C.pcm",

// CHECK: "sourceFiles": [
// CHECK-DAG: module.modulemap
// CHECK-DAG: C.h

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-NEXT: "clang": "B"

// CHECK: "moduleMapPath"
// CHECK-SAME: module.modulemap

// CHECK: "contextHash"
// CHECK-SAME: "{{.*}}"

// CHECK: "commandLine": [
// CHECK-NEXT: "-frontend"
// CHECK-NEXT: "-only-use-extra-clang-opts"
// CHECK-NOT:  "BUILD_DIR/bin/clang"
// CHECK:      "-Xcc"
// CHECK-NEXT: "-resource-dir"
// CHECK-NEXT: "-Xcc"
// CHECK-NEXT: "BUILD_DIR/lib/swift/clang"
// CHECK:      "-fsystem-module",
// CHECK-NEXT: "-emit-pcm",
// CHECK-NEXT: "-module-name",
// CHECK-NEXT: "C"

// CHECK: "capturedPCMArgs": [
// CHECK-NEXT:   [,
// CHECK-NEXT:     "-Xcc",
// CHECK-NEXT:     "-fapinotes-swift-version=4"
// CHECK-NEXT:   ]
// CHECK-NEXT: ]

/// --------Swift module E
// CHECK: "swift": "E"
// CHECK-LABEL: modulePath": "{{.*}}{{/|\\}}E-{{.*}}.swiftmodule"
// CHECK: "directDependencies"
// CHECK-NEXT: {
// CHECK-NEXT: "swift": "Swift"

// CHECK: "moduleInterfacePath"
// CHECK-SAME: E.swiftinterface

/// --------Swift module F
// CHECK:      "modulePath": "{{.*}}{{/|\\}}F-{{.*}}.swiftmodule",
// CHECK-NEXT: "sourceFiles": [
// CHECK-NEXT: ],
// CHECK-NEXT: "directDependencies": [
// CHECK-NEXT:   {
// CHECK-DAG:     "clang": "F"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-NEXT:   }
// CHECK-NEXT: ],

/// --------Swift module G
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}G-{{.*}}.swiftmodule"
// CHECK: "directDependencies"
// CHECK-NEXT: {
// CHECK-DAG:   "clang": "G"
// CHECK-DAG:   "swift": "Swift"
// CHECK-DAG:   "swift": "SwiftOnoneSupport"
// CHECK: ],
// CHECK-NEXT: "details": {

// CHECK: "contextHash": "{{.*}}",
// CHECK: "commandLine": [
// CHECK: "-compile-module-from-interface"
// CHECK: "-target"
// CHECK: "-module-name"
// CHECK: "G"
// CHECK: "-swift-version"
// CHECK: "5"
// CHECK: ],
// CHECK_NO_CLANG_TARGET: "extraPcmArgs": [
// CHECK_NO_CLANG_TARGET-NEXT:   "-Xcc",
// CHECK_NO_CLANG_TARGET-NEXT:   "-target",
// CHECK_CLANG_TARGET: "extraPcmArgs": [
// CHECK_CLANG_TARGET-NEXT:   "-Xcc",
// CHECK_CLANG_TARGET-NEXT:   "-fapinotes-swift-version={{.*}}"
// CHECK_CLANG_TARGET-NEXT:   ]

/// --------Swift module Swift
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}Swift-{{.*}}.swiftmodule",

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-NEXT: "clang": "SwiftShims"

/// --------Clang module B
// CHECK-LABEL: "modulePath": "B.pcm"
// CHECK: "contextHash": [[B_CONTEXT:"{{.*}}"]],
// CHECK: "-o"
// CHECK-NEXT: "-Xcc"
// CHECK-NEXT: "%t/clang-module-cache/B-[[B_CONTEXT]].pcm",

// CHECK-NEXT: sourceFiles
// CHECK-DAG: module.modulemap
// CHECK-DAG: B.h

// CHECK: directDependencies
// CHECK-NEXT: {
// CHECK-NEXT: "clang": "A"
// CHECK-NEXT: }

/// --------Clang module SwiftShims
// CHECK-LABEL: "modulePath": "SwiftShims.pcm",
// CHECK: "contextHash": [[SHIMS_CONTEXT:"{{.*}}"]],
// CHECK: "-o"
// CHECK-NEXT: "-Xcc"
// CHECK-NEXT: "%t/clang-module-cache/SwiftShims-[[SHIMS_CONTEXT]].pcm",
// CHECK: "-fmodule-file=A=%t/clang-module-cache/A-{{.*}}.pcm"
// CHECK-NO-SEARCH-PATHS-NOT: "-prebuilt-module-cache-path"

// Check make-style dependencies
// CHECK-MAKE-DEPS: module_deps.swift
// CHECK-MAKE-DEPS-SAME: A.swiftinterface
// CHECK-MAKE-DEPS-SAME: G.swiftinterface
// CHECK-MAKE-DEPS-SAME: B.h
// CHECK-MAKE-DEPS-SAME: F.h
// CHECK-MAKE-DEPS-SAME: Bridging.h
// CHECK-MAKE-DEPS-SAME: BridgingOther.h
// CHECK-MAKE-DEPS-SAME: module.modulemap

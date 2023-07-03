// This test ensures that the parent invocation's '-validate-clang-modules-once' flag is inherited when building dependency modules
// RUN: %empty-directory(%t)
// RUN: touch %t/Build.session

// RUN: %target-build-swift -module-name TestModule -module-link-name TestModule %S/Inputs/TestModule.swift -enable-library-evolution -emit-module-interface -o %t/TestModule.swiftmodule -swift-version 5 -Xfrontend -disable-implicit-concurrency-module-import -Xfrontend -disable-implicit-string-processing-module-import

// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I%t -validate-clang-modules-once -clang-build-session-file %t/Build.session -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import
// RUN: %FileCheck %s < %t/deps.json

import TestModule

 // CHECK:      "directDependencies": [
 // CHECK-NEXT:        {
 // CHECK-DAG:          "swift": "TestModule"
 // CHECK-DAG:          "swift": "Swift"
 // CHECK-DAG:          "swift": "SwiftOnoneSupport"

 // CHECK:      "swift": "TestModule"
 // CHECK-NEXT:    },
 // CHECK-NEXT:    {
 // CHECK-NEXT:      "modulePath": "{{.*}}TestModule-{{.*}}.swiftmodule",
 // CHECK-NEXT:      "sourceFiles": [
 // CHECK-NEXT:      ],
 // CHECK-NEXT:      "directDependencies": [
 // CHECK-NEXT:        {
 // CHECK-NEXT:          "swift": "Swift"
 // CHECK-NEXT:        },
 // CHECK-NEXT:        {
 // CHECK-NEXT:          "swift": "SwiftOnoneSupport"
 // CHECK-NEXT:        }
 // CHECK-NEXT:      ],
 // CHECK-NEXT:      "details": {
 // CHECK-NEXT:        "swift": {
 // CHECK-NEXT:          "moduleInterfacePath": 
 // CHECK-NEXT:          "contextHash":
 // CHECK-NEXT:          "compiledModuleCandidates": [
 // CHECK-NEXT:            TestModule.swiftmodule
 // CHECK-NEXT:          ],
 // CHECK-NEXT:          "commandLine": [
 // CHECK-NEXT:            "-frontend",
 // CHECK-NEXT:            "-compile-module-from-interface",
 // CHECK:                 "-validate-clang-modules-once",
 // CHECK-NEXT:            "-clang-build-session-file",
 // CHECK-NEXT:            "{{.*}}Build.session"

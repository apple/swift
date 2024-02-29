// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/DependencyModules)
// RUN: split-file --leading-lines %s %t

// Emit a binary module dependency
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/DependencyModules/BinaryModuleDep.swiftmodule -module-cache-path %t/clang-module-cache %t/BinaryModuleDepSource.swift -module-name BinaryModuleDep -I %S/Inputs/CHeaders -I %S/Inputs/Swift

// Scan the client and ensure both the Client and BinaryModuleDep modules have a Swift overlay dependency on 'F' as imported by 'ClangModuleWithOverlayedDep'
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -I %t/DependencyModules
// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

//--- BinaryModuleDepSource.swift
import ClangModuleWithOverlayedDep

//--- Client.swift
import BinaryModuleDep

// CHECK:         "swift": "deps"
// CHECK:         "directDependencies": [
// CHECK-DAG:          "swift": "Swift"
// CHECK-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-DAG:          "swift": "_Concurrency"
// CHECK-DAG:          "clang": "_SwiftConcurrencyShims"
// CHECK-DAG:          "swift": "_StringProcessing"
// CHECK-DAG:          "clang": "ClangModuleWithOverlayedDep"
// CHECK-DAG:          "swiftPrebuiltExternal": "BinaryModuleDep"
// CHECK-DAG:          "swift": "F"
// CHECK:          ],
// CHECK:          "swiftOverlayDependencies": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "swift": "F"
// CHECK-NEXT:       }
// CHECK-NEXT:     ]

// CHECK:      "swiftPrebuiltExternal": "BinaryModuleDep"
// CHECK:         "directDependencies": [
// CHECK-DAG:          "swift": "Swift"
// CHECK-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-DAG:          "swift": "_Concurrency"
// CHECK-DAG:          "clang": "_SwiftConcurrencyShims"
// CHECK-DAG:          "swift": "_StringProcessing"
// CHECK-DAG:          "clang": "ClangModuleWithOverlayedDep"
// CHECK-DAG:          "swift": "F"
// CHECK:          ],
// CHECK:          "swiftOverlayDependencies": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "swift": "F"
// CHECK-NEXT:       }
// CHECK-NEXT:     ]

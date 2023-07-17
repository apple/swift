// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/Frameworks
// RUN: mkdir -p %t/Frameworks/Foo.framework/
// RUN: mkdir -p %t/Frameworks/Foo.framework/Modules
// RUN: mkdir -p %t/Frameworks/Foo.framework/Modules/Foo.swiftmodule

// Build a dependency into a binary module
// RUN: echo "public func foo() {}" >> %t/foo.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Frameworks/Foo.framework/Modules/Foo.swiftmodule/%target-cpu.swiftmodule -module-cache-path %t.module-cache %t/foo.swift -module-name Foo

// Run the scan
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -F %t/Frameworks/ -sdk %t
// RUN: %validate-json %t/deps.json | %FileCheck %s

import Foo

// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK:      "swiftPrebuiltExternal": "Foo"
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "modulePath": 
// CHECK-NEXT:      "directDependencies": [
// CHECK:      "details": {
// CHECK-NEXT:        "swiftPrebuiltExternal": {
// CHECK-NEXT:          "compiledModulePath": 
// CHECK-NEXT:          "isFramework": true
// CHECK-NEXT:        }

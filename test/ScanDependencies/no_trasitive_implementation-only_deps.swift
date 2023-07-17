// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/Frameworks
// RUN: mkdir -p %t/Frameworks/Foo.framework/
// RUN: mkdir -p %t/Frameworks/Foo.framework/Modules
// RUN: mkdir -p %t/Frameworks/Foo.framework/Modules/Foo.swiftmodule

// Build a dependency into a binary module with an @_implementationOnly dependency on `E`
// RUN: echo "@_implementationOnly import E;public func foo() {}" >> %t/foo.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Frameworks/Foo.framework/Modules/Foo.swiftmodule/%target-cpu.swiftmodule -module-cache-path %t.module-cache %t/foo.swift -module-name Foo -I %S/Inputs/Swift

// Run the scan
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -F %t/Frameworks/ -sdk %t
// RUN: %validate-json %t/deps.json | %FileCheck %s

import Foo

// CHECK-NOT: "swift": "E"

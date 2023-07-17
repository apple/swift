// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/Foo.swiftmodule)
// RUN: echo "internal import A; public func foo() {}" > %t/Foo.swift
// REQUIRES: executable_test
// REQUIRES: objc_interop

@testable import Foo

// Step 1: build swift interface and swift module side by side, make them testable
// RUN: %target-swift-frontend -emit-module %t/Foo.swift -emit-module-path %t/Foo.swiftmodule/%target-swiftmodule-name -module-name Foo -emit-module-interface-path %t/Foo.swiftmodule/%target-swiftinterface-name -I %S/Inputs/CHeaders -I %S/Inputs/Swift -enable-testing -enable-experimental-feature AccessLevelOnImport

// Step 3: scan dependencies
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t -sdk %t -prebuilt-module-cache-path %t/clang-module-cache -I %S/Inputs/CHeaders -I %S/Inputs/Swift
// RUN: %validate-json %t/deps.json | %FileCheck %s

// The dependency of `Foo` on `A` will not be visible if the scanner simply scans the textual interface
// of `Foo`. So we verify that for a `@testable` import, the scanner also opens up the adjacent binary module and
// resolves the required dependencies contained within.
//
// CHECK: "swift": "A"

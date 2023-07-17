// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/Frameworks
// RUN: mkdir -p %t/Frameworks/E.framework/
// RUN: mkdir -p %t/Frameworks/E.framework/Modules
// RUN: mkdir -p %t/Frameworks/E.framework/Modules/E.swiftmodule

// Copy over the interface
// RUN: cp %S/Inputs/Swift/E.swiftinterface %t/Frameworks/E.framework/Modules/E.swiftmodule/%module-target-triple.private.swiftinterface
// RUN: cp %S/Inputs/Swift/E.swiftinterface %t/Frameworks/E.framework/Modules/E.swiftmodule/%module-target-triple.swiftinterface

// Build a dependency into a binary module
// RUN: echo "public func foo() {}" >> %t/foo.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Frameworks/E.framework/Modules/E.swiftmodule/%module-target-triple.swiftmodule -module-cache-path %t.module-cache %t/foo.swift -module-name E

// Run the scan
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -F %t/Frameworks/ -sdk %t
// RUN: %validate-json %t/deps.json | %FileCheck %s

import E

// Ensure the private interface is the canonical one
// CHECK: "moduleInterfacePath": {{.*}}{{/|\\}}E.framework{{/|\\}}Modules{{/|\\}}E.swiftmodule{{/|\\}}{{.*}}.private.swiftinterface
// Ensure the adjacent binary module is a candidate
// CHECK: "compiledModuleCandidates": [
// CHECK-NEXT: {{.*}}{{/|\\}}E.framework{{/|\\}}Modules{{/|\\}}E.swiftmodule{{/|\\}}{{.*}}.swiftmodule

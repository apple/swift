// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/cas

// RUN: %target-swift-frontend -emit-module -module-cache-path %t/clang-module-cache %S/../ScanDependencies/Inputs/Swift/E.swiftinterface -o %t/E.swiftmodule -I %S/../ScanDependencies/Inputs/CHeaders -I %S/../ScanDependencies/Inputs/Swift -swift-version 4
// RUN: %target-swift-frontend -emit-module -module-cache-path %t/clang-module-cache %S/../ScanDependencies/Inputs/Swift/A.swiftinterface -o %t/A.swiftmodule -I %S/../ScanDependencies/Inputs/CHeaders -I %S/../ScanDependencies/Inputs/Swift -swift-version 4
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %t -swift-version 4 -cache-compile-job -cas-path %t/cas
// RUN: %validate-json %t/deps.json | %FileCheck %s -DTEMP=%t

/// Test binary module key: binary module key is the CASID of itself.
// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json swiftPrebuiltExternal:A moduleCacheKey > %t/A.key.casid
// RUN: llvm-cas --cas %t/cas --cat-blob @%t/A.key.casid > %t/Loaded.swiftmodule
// RUN: diff %t/A.swiftmodule %t/Loaded.swiftmodule

import A
import E


/// Main module
// CHECK-LABEL: "swift": "deps"
// CHECK: "directDependencies": [
// CHECK: "swiftPrebuiltExternal": "A"
// CHECK: "swiftPrebuiltExternal": "E"
// CHECK: "details": {
// CHECK: "casFSRootID"

/// E.swiftmodule
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}E.swiftmodule",
// CHECK: "details": {
// CHECK: "swiftPrebuiltExternal": {
// CHECK: "compiledModulePath":
// CHECK: "moduleCacheKey":

/// A.swiftmodule
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}A.swiftmodule",
// CHECK: "details": {
// CHECK: "swiftPrebuiltExternal": {
// CHECK: "compiledModulePath":
// CHECK: "moduleCacheKey":

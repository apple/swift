// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/module-cache)
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -target %target-cpu-apple-macosx10.14
// RUN: %validate-json %t/deps.json | %FileCheck %s

import X

// CHECK: "clang": "X"
// CHECK: "clang": "X"
// CHECK: "commandLine": [
// CHECK-DAG: "-fmodule-format=obj"
// CHECK-DAG: "-dwarf-ext-refs"

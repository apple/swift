// RUN: %empty-directory(%t.module-cache)
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t.module-cache %s -o %t.deps.json -I %S/Inputs/CHeaders

// RUN: %validate-json %t.deps.json | %FileCheck %s
// CHECK: "clang": "X_Private"
import X.Private


// RUN: %empty-directory(%t.module-cache)
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t.module-cache %s -o %t.deps.json -I %S/Inputs/CHeaders

// RUN: %FileCheck %s < %t.deps.json
// CHECK: "clang": "X_Private"
import X.Private


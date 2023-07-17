// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: %target-swift-frontend -scan-dependencies -import-prescan -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import C
import E
import G
import SubE

// CHECK: "imports": [
// CHECK-NEXT:  "C",
// CHECK-NEXT:  "E",
// CHECK-NEXT:  "G",
// CHECK-NEXT:  "SubE",
// CHECK-NEXT:  "Swift",
// CHECK-NEXT:  "SwiftOnoneSupport",
// CHECK-NEXT:  "_Concurrency",
// CHECK-NEXT:  "_SwiftConcurrencyShims",
// CHECK-NEXT:  "_StringProcessing"
// CHECK-NEXT: ]

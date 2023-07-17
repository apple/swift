// This test ensures that the parent invocation's '-Xcc X' flags are inherited when building dependency modules
// RUN: %empty-directory(%t)

// Just running a compile is useful to make sure it succeeds because that means the transitive Clang module dependency
// received the TANGERINE macro
// RUN: %target-swift-frontend -typecheck -strict-implicit-module-context %s -I %S/Inputs/macro-only-module -Xcc -DTANGERINE=1 -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import

// RUN: %target-swift-frontend -scan-dependencies -strict-implicit-module-context %s -o %t/deps.json -I %S/Inputs/macro-only-module -Xcc -DTANGERINE=1 -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import
// RUN: %validate-json %t/deps.json &>/dev/null
// RUN: %FileCheck %s < %t/deps.json

import ImportsMacroSpecificClangModule

// CHECK:      "directDependencies": [
// CHECK-NEXT:        {
// CHECK-DAG:          "swift": "ImportsMacroSpecificClangModule"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-DAG:          "swift": "Swift"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-DAG:          "swift": "SwiftOnoneSupport"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],

//CHECK:      "swift": "ImportsMacroSpecificClangModule"
//CHECK-NEXT:    },
//CHECK-NEXT:    {
//CHECK-NEXT:      "modulePath": "{{.*}}{{/|\\}}ImportsMacroSpecificClangModule-{{.*}}.swiftmodule",
//CHECK-NEXT:      "sourceFiles": [
//CHECK-NEXT:      ],
//CHECK-NEXT:      "directDependencies": [
//CHECK-NEXT:        {
//CHECK-NEXT:          "swift": "SubImportsMacroSpecificClangModule"
//CHECK-NEXT:        },
//CHECK-NEXT:        {
//CHECK-NEXT:          "swift": "SwiftOnoneSupport"

//CHECK:      "swift": "SubImportsMacroSpecificClangModule"
//CHECK-NEXT:    },
//CHECK-NEXT:    {
//CHECK-NEXT:      "modulePath": "{{.*}}{{/|\\}}SubImportsMacroSpecificClangModule-{{.*}}.swiftmodule",
//CHECK-NEXT:      "sourceFiles": [
//CHECK-NEXT:      ],
//CHECK-NEXT:      "directDependencies": [
//CHECK-NEXT:        {
//CHECK-NEXT:          "clang": "OnlyWithMacro"

// CHECK:      "clang": "OnlyWithMacro"
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "modulePath": "{{.*}}{{/|\\}}OnlyWithMacro-{{.*}}.pcm",
// CHECK-NEXT:      "sourceFiles": [
// CHECK-DAG:        "{{.*}}OnlyWithMacro.h"
// CHECK-DAG:        "{{.*}}module.modulemap"
// CHECK-NEXT:      ],
// CHECK-NEXT:      "directDependencies": [
// CHECK-NEXT:      ],
// CHECK-NEXT:      "details": {
// CHECK-NEXT:        "clang": {
// CHECK-NEXT:          "moduleMapPath": "{{.*}}module.modulemap",
// CHECK-NEXT:          "contextHash": "{{.*}}",
// CHECK-NEXT:          "commandLine": [

// CHECK:                  "TANGERINE=1"

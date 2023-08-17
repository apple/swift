// RUN: %empty-directory(%t.module-cache)
// RUN: %empty-directory(%t.scanner-cache)

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t.module-cache -clang-scanner-module-cache-path %t.scanner-cache %s -o %t.deps.json -I %S/Inputs/SwiftDifferent -dump-clang-diagnostics 2>&1 | %FileCheck %s --check-prefix=CHECK-CLANG-COMMAND
// RUN: %validate-json %t.deps.json | %FileCheck %s --check-prefix=CHECK-DEPS

// Ensure we prefer clang scanner module cache path
// CHECK-CLANG-COMMAND: '-fmodules-cache-path={{.*}}.scanner-cache'

// Ensure we the modules' output path is set to the module cache
// CHECK-DEPS: "swift": "A"
// CHECK-DEPS:            "swift": "A"
// CHECK-DEPS:      "modulePath": "{{.*}}separate_clang_scan_cache.swift.tmp.module-cache{{/|\\\\}}A-{{.*}}.swiftmodule"

import A

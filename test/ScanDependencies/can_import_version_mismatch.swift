// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: cp %S/../Serialization/Inputs/too-old/Library.swiftmodule %t/include/Library.swiftmodule

/// No warning when there is a swiftinterface file even the binary module is invalid.
// RUN: %target-swift-frontend -scan-dependencies -module-name Test %t/test.swift -module-cache-path %t/clang-module-cache \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -o %t/deps.json -I %t/include 2>&1 | %FileCheck %s --check-prefix=NOWARN --allow-empty

// NOWARN-NOT: warning:

/// Issue a warning when only found invalid binary module.
// RUN: rm %t/include/Library.swiftinterface
// RUN: %target-swift-frontend -scan-dependencies -module-name Test %t/test.swift -module-cache-path %t/clang-module-cache \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -o %t/deps.json -I %t/include 2>&1 | %FileCheck %s --check-prefix=WARN
// WARN: warning: canImport() evaluated to false due to invalid swiftmodule

// RUN: %FileCheck %s --check-prefix=DEP --input-file=%t/deps.json
// DEP-NOT: Library

//--- test.swift
#if canImport(Library)
import Library
#endif

//--- include/Library.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name Library -O -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -user-module-version 1.0
public func foo() {}

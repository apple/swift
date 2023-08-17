// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath


// RUN: %target-swift-frontend -compile-module-from-interface -module-name ConformanceMacroLib %S/Inputs/ConformanceMacroLib.swiftinterface -o %t/ConformanceMacroLib.swiftmodule

// RUN: %target-swift-frontend -swift-version 5 -typecheck -I%t -verify %s -verify-ignore-unknown  -load-plugin-library %t/%target-library-name(MacroDefinition) -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt

import ConformanceMacroLib

@Equatable
struct S {}

// CHECK-DUMP: extension S: Equatable  {
// CHECK-DUMP: }

@Hashable
struct T {}

// CHECK-DUMP: extension T: Hashable  {
// CHECK-DUMP: }

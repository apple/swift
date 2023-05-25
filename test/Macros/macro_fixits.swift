// REQUIRES: swift_swift_parser

// This test ensures we don't emit fix-its in generated code, such as macro
// expansion buffers.

// RUN: %empty-directory(%t)

// RUN: %host-build-swift -emit-library %S/Inputs/syntax_macro_definitions.swift -o %t/%target-library-name(MacroDefinition) -module-name MacroDefinition -swift-version 5 -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -typecheck %s -load-plugin-library %t/%target-library-name(MacroDefinition) -swift-version 5 -serialize-diagnostics-path %t/diags.dia -fixit-all -emit-fixits-path %t/fixits.json

// RUN: %FileCheck %s --check-prefix FIXITS-JSON < %t/fixits.json

// FIXITS-JSON:      [
// FIXITS-JSON-NEXT: ]

// RUN: c-index-test -read-diagnostics %t/diags.dia 2>&1 | %FileCheck -check-prefix DIAGS %s

// DIAGS: warning: variable 'x' was never mutated; consider changing to 'let' constant
// DIAGS-NEXT: Number FIXITs = 0

@attached(member, names: arbitrary)
public macro addMemberWithFixIt() = #externalMacro(module: "MacroDefinition", type: "AddMemberWithFixIt")

@addMemberWithFixIt
struct S {}

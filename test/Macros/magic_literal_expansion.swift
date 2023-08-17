// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-build-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5 -emit-tbd -emit-tbd-path %t/MacroUser.tbd -I %t
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s


@freestanding(expression)
macro MagicFile() -> String = #externalMacro(module: "MacroDefinition", type: "MagicFileMacro")

@freestanding(expression)
macro MagicLine() -> Int = #externalMacro(module: "MacroDefinition", type: "MagicLineMacro")

// CHECK: magic_literal_expansion.swift
print(#MagicFile)

// CHECK: 21
print(#MagicLine)

@freestanding(expression)
macro Nested() -> Void = #externalMacro(module: "MacroDefinition", type: "NestedMagicLiteralMacro")

// CHECK: magic_literal_expansion.swift
// CHECK: 28
#Nested

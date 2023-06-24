// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/src
// RUN: mkdir -p %t/plugins
// RUN: mkdir -p %t/lib

// RUN: split-file %s %t/src

//#-- Prepare the macro dylib plugin.
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library -o %t/plugins/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroDefinition \
// RUN:   %t/src/MacroDefinition.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

//#-- Prepare the macro library.
// RUN: %target-swift-frontend \
// RUN:   -swift-version 5 \
// RUN:   -emit-module -o %t/lib/MacroLib.swiftmodule \
// RUN:   -module-name MacroLib \
// RUN:   -plugin-path %t/plugins \
// RUN:   %t/src/MacroLib.swift

// RUN: %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -I %t/lib \
// RUN:   -plugin-path %t/plugins \
// RUN:   %t/src/test.swift

//--- MacroDefinition.swift
import SwiftSyntax
import SwiftSyntaxMacros

public struct OneMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    return "1"
  }
}


//--- MacroLib.swift
@freestanding(expression) public macro `public`() -> Int = #externalMacro(module: "MacroDefinition", type: "OneMacro")
@freestanding(expression) public macro `escaped`() -> Int = #externalMacro(module: "MacroDefinition", type: "OneMacro")
@freestanding(expression) public macro normal() -> Int = #externalMacro(module: "MacroDefinition", type: "OneMacro")

//--- test.swift
import MacroLib
@freestanding(expression) public macro `class`() -> Int = #externalMacro(module: "MacroDefinition", type: "OneMacro")

func test() {
  let _: Int = #public()
  let _: Int = #`public`()
  let _: Int = #escaped()
  let _: Int = #`class`()
  let _: Int = #normal()
}

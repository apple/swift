// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t/src)
// RUN: %empty-directory(%t/plugins)
// RUN: %empty-directory(%t/plugins_local)

// RUN: split-file %s %t/src


//#-- Prepare the macro dylib plugin.
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library -o %t/plugins/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroDefinition \
// RUN:   %t/src/MacroDefinition.float.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

//#-- Prepare the macro dylib plugin.
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library -o %t/plugins_local/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroDefinition \
// RUN:   %t/src/MacroDefinition.int.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

//#-- Check '-load-plugin-library' takes precedence over '-plugin-path'.
// RUN: %target-swift-frontend -typecheck -verify -swift-version 5 \
// RUN:   -load-plugin-library %t/plugins_local/%target-library-name(MacroDefinition) \
// RUN:   -plugin-path %t/plugins \
// RUN:   %t/src/test.swift

//#-- Different argument order changes the search order, hence fail.
// RUN: not %target-swift-frontend -typecheck -verify -swift-version 5 \
// RUN:   -plugin-path %t/plugins \
// RUN:   -load-plugin-library %t/plugins_local/%target-library-name(MacroDefinition) \
// RUN:   %t/src/test.swift

//--- test.swift
@freestanding(expression) macro constInt() -> Int = #externalMacro(module: "MacroDefinition", type: "ConstMacro")

func foo() {
  let _: Int = #constInt
  // If 'MacroDefinition.float.swift' (in '-pluing-path') is loaded, type checking this fails because it expands to '4.2' which is a float literal.
}

//--- MacroDefinition.float.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct ConstMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {

    return "4.2"
  }
}

//--- MacroDefinition.int.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct ConstMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {

    return "42"
  }
}

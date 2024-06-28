// REQUIRES: swift_swift_parser
// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/plugins)
// RUN: split-file %s %t

//== Build the plugin library
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library \
// RUN:   -o %t/plugins/%target-library-name(MacroDefinition) \
// RUN:   -module-name=MacroDefinition \
// RUN:   %t/MacroDefinition.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library \
// RUN:   -o %t/plugins/%target-library-name(EvilMacros) \
// RUN:   -module-name=EvilMacros \
// RUN:   %t/EvilMacros.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

// RUN: %cmake-c-compiler \
// RUN:   %c-flags -target %host_triple -isysroot %host_sdk \
// RUN:   -shared -o %t/plugins/libCrashOnLoad.dylib \
// RUN:   %t/CrashOnLoad.c

// RUN: %target-swift-frontend \
// RUN:   -swift-version 5 \
// RUN:   -emit-module -o \
// RUN:   %t/MacroLibrary.swiftmodule \
// RUN:   %t/MacroLibrary.swift \
// RUN:   -module-name MacroLibrary \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server

// RUN: env SWIFT_DUMP_PLUGIN_MESSAGING=1 %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -I %t \
// RUN:   -swift-version 5 \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server \
// RUN:   -module-name MyApp \
// RUN:   %t/app.swift \
// RUN:   2>&1 | tee %t/macro-expansions.txt

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// CHECK: ->(plugin:[[#PID1:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION:]]}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}CrashOnLoad.{{.*}}","moduleName":"CrashOnLoad"}}

// CHECK-NEXT: ->(plugin:[[#PID2:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}EvilMacros{{.*}},"moduleName":"EvilMacros"}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"expandFreestandingMacro":{"discriminator":"{{.*}}","lexicalContext":{{.*}},"macro":{"moduleName":"EvilMacros","name":"evil","typeName":"CrashingMacro"}{{.*}}

// CHECK-NEXT: ->(plugin:[[#PID3:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: <-(plugin:[[#PID3]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: ->(plugin:[[#PID3]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}EvilMacros{{.*}}","moduleName":"EvilMacros"}}
// CHECK-NEXT: <-(plugin:[[#PID3]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID3]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}MacroDefinition{{.*}}","moduleName":"MacroDefinition"}}
// CHECK-NEXT: <-(plugin:[[#PID3]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID3]]) {"expandFreestandingMacro":{"discriminator":"{{.*}}","lexicalContext":{{.*}},"macro":{"moduleName":"MacroDefinition","name":"stringify","typeName":"StringifyMacro"}{{.*}}
// CHECK-NEXT: <-(plugin:[[#PID3]]) {"expandMacroResult":{"diagnostics":[],"expandedSource":"(1, \"1\")"}}

//--- MacroDefinition.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct StringifyMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.arguments.first?.expression else {
      fatalError("boom")
    }

    return "(\(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }
}

//--- EvilMacros.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct CrashingMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let arg: UInt = UInt(macro.argumentList.first!.expression.description)!
    let zero: UInt = 0
    return "\(raw: zero - arg).description"
  }
}

//--- CrashOnLoad.c
#include <stdlib.h>

__attribute__((constructor))
void crashMe(void) {
  abort();
}

//--- MacroLibrary.swift
@freestanding(expression) public macro crash() -> Int = #externalMacro(module: "CrashOnLoad", type: "WhateverMacro")
@freestanding(expression) public macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")
@freestanding(expression) public macro evil(_ value: Int) -> String = #externalMacro(module: "EvilMacros", type: "CrashingMacro")

//--- app.swift
import MacroLibrary 

func test() {
  _ = #crash() // expected-error {{external macro implementation type 'CrashOnLoad.WhateverMacro' could not be found for macro 'crash()'; failed to load library plugin}}
  _ = #evil(1) // expected-error {{failed to receive result from plugin (from macro 'evil')}}
  _ = #stringify(1)
}

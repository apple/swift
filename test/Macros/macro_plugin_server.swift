// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/plugins)
//
//== Build the plugin library
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library \
// RUN:   -o %t/plugins/%target-library-name(MacroDefinition) \
// RUN:   -module-name=MacroDefinition \
// RUN:   %S/Inputs/syntax_macro_definitions.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library \
// RUN:   -o %t/plugins/%target-library-name(EvilMacros) \
// RUN:   -module-name=EvilMacros \
// RUN:   %S/Inputs/evil_macro_definitions.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

// RUN: env SWIFT_DUMP_PLUGIN_MESSAGING=1 %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -swift-version 5 -enable-experimental-feature Macros \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server \
// RUN:   -Rmacro-loading -verify-ignore-unknown \
// RUN:   -module-name MyApp \
// RUN:   %s \
// RUN:   2>&1 | tee %t/macro-expansions.txt

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// RUN: not %target-swift-frontend \
// RUN:   -typecheck \
// RUN:   -swift-version 5 \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server \
// RUN:   -Rmacro-loading \
// RUN:   -module-name MyApp \
// RUN:   %s \
// RUN:   2>&1 | tee %t/macro-loading.txt

// RUN: %FileCheck -check-prefix=CHECK-DIAGS %s < %t/macro-loading.txt

// CHECK-DIAGS: loaded macro implementation module 'MacroDefinition' from compiler plugin server

// CHECK: ->(plugin:[[#PID1:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION:]]}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}MacroDefinition.{{dylib|so|dll}}","moduleName":"MacroDefinition"}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}EvilMacros.{{dylib|so|dll}}","moduleName":"EvilMacros"}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"${{.*}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"MacroDefinition","name":"stringify","typeName":"StringifyMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{{{.+}}},"source":"#stringify(a + b)"}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"expandMacroResult":{"diagnostics":[],"expandedSource":"(a + b, \"a + b\")"}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"${{.*}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"EvilMacros","name":"evil","typeName":"CrashingMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{{{.+}}},"source":"#evil(42)"}}}
// ^ This crashes the plugin server.

// CHECK: ->(plugin:[[#PID2:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}MacroDefinition.{{dylib|so|dll}}","moduleName":"MacroDefinition"}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"loadPluginLibrary":{"libraryPath":"{{.*}}EvilMacros.{{dylib|so|dll}}","moduleName":"EvilMacros"}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"expandFreestandingMacro":{"discriminator":"${{.*}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"MacroDefinition","name":"stringify","typeName":"StringifyMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{{{.+}}},"source":"#stringify(b + a)"}}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"expandMacroResult":{"diagnostics":[],"expandedSource":"(b + a, \"b + a\")"}}

// CHECK-NEXT ->(plugin:[[#PID2]]) {"expandFreestandingMacro":{"discriminator":"${{.*}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"MacroDefinition","name":"missing","typeName":"TypeDoesNotExist"},"macroRole":"expression","syntax":{"kind":"expression","location":{{{*}}},"source":" #missing()"}}}
// CHECK-NEXT <-(plugin:[[#PID2]]) {"expandMacroResult":{"diagnostics":[{"fixIts":[],"highlights":[{{{.*}}}],"message":"macro implementation type 'MacroDefinition.TypeDoesNotExist' could not be found in library plugin '{{.*}}MacroDefinition.{{dylib|so|dll}}'","notes":[],"position":{{{.*}}},"severity":"error"}]}}
// CHECK-NEXT ->(plugin:[[#PID2]]) {"expandFreestandingMacro":{"discriminator":"${{.*}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"MacroDefinition","name":"missing","typeName":"TypeDoesNotExist"},"macroRole":"expression","syntax":{"kind":"expression","location":{{{*}}},"source":" #notMacro()"}}}
// CHECK-NEXT <-(plugin:[[#PID2]]) {"expandMacroResult":{"diagnostics":[{"fixIts":[],"highlights":[{{{.*}}}],"message":"type 'MacroDefinition.NotMacroStruct' is not a valid macro implementation type in library plugin '{{.*}}MacroDefinition.{{dylib|so|dll}}'","notes":[],"position":{{{.*}}},"severity":"error"}]}}

@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")
@freestanding(expression) macro evil(_ value: Int) -> String = #externalMacro(module: "EvilMacros", type: "CrashingMacro")
// FIXME: Diagnose for missing type.
@freestanding(expression) macro missing() = #externalMacro(module: "MacroDefinition", type: "TypeDoesNotExist")
// FIXME: Diagnose for non-macro type.
@freestanding(expression) macro notMacro() = #externalMacro(module: "MacroDefinition", type: "NotMacroStruct")

func testStringify(a: Int, b: Int) {
  let s1: String = #stringify(a + b).1
  print(s1)

  // expected-error @+1 {{failed to receive result from plugin (from macro 'evil')}}
  let s2: String = #evil(42)
  print(s2)

  let s3: String = #stringify(b + a).1
  print(s3)

  // expected-error @+1 {{type 'MacroDefinition.TypeDoesNotExist' could not be found in library plugin '}}
  _ = #missing()

  // expected-error @+1 {{type 'MacroDefinition.NotMacroStruct' is not a valid macro implementation type in library plugin '}}
  _ = #notMacro()
}

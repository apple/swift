// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)

// RUN: split-file %s %t

//#-- Prepare the Wasm macro plugin.
// RUN: %swift-build-wasm-c-plugin %t/MacroDefinition.c -o %t/Plugin.wasm

// RUN: %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -swift-version 5 \
// RUN:   -load-plugin %t/Plugin.wasm:%swift-plugin-server#MacroDefinition \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift

//--- test.swift
// expected-warning @+2 {{Wasm plugin does not have a '_start' entrypoint}}
// expected-note @+1 {{declared here}}
@freestanding(expression) macro constInt() -> Int = #externalMacro(module: "MacroDefinition", type: "ConstMacro")

func foo() {
  // expected-error @+1 {{Wasm plugin does not have a '_start' entrypoint}}
  let _: Int = #constInt
}

//--- MacroDefinition.c
__attribute__((export_name("_tart")))
void _start(void) {}

__attribute__((export_name("swift_wasm_macro_v1_pump")))
void pump(void) {}

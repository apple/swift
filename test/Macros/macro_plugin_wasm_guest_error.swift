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
// RUN:   %t/test.swift \
// RUN:   2>%t/macro-loading.txt

// RUN: %FileCheck %s < %t/macro-loading.txt

// CHECK: guest error!

//--- test.swift
@freestanding(expression) macro constInt() -> Int = #externalMacro(module: "MacroDefinition", type: "ConstMacro")

func foo() {
  // expected-error @+1 {{failed to communicate with external macro}}
  let _: Int = #constInt
}

//--- MacroDefinition.c
#include "Inputs/wasi_shim.h"

__attribute__((export_name("_start")))
void _start(void) {}

__attribute__((export_name("swift_wasm_macro_v1_pump")))
void pump(void) {
  swift_abort("guest error!\n");
}

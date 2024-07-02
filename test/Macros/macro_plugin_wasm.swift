// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)

// RUN: split-file %s %t

//#-- Prepare the Wasm macro plugin.
// RUN: %swift-build-wasm-c-plugin %t/MacroDefinition.c -o %t/Plugin.wasm

// RUN: env SWIFT_DUMP_PLUGIN_MESSAGING=1 %target-swift-frontend \
// RUN:   -typecheck \
// RUN:   -swift-version 5 \
// RUN:   -load-plugin %t/Plugin.wasm:%swift-plugin-server#MacroDefinition \
// RUN:   -Rmacro-loading \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift \
// RUN:   > %t/macro-loading.txt 2>&1

// RUN: %FileCheck %s < %t/macro-loading.txt

// CHECK: ->(plugin:[[#PID:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION:]]}}}
// CHECK: <-(plugin:[[#PID]]) {"getCapabilityResult":{"capability":{"features":["load-plugin-library"],"protocolVersion":7}}}
// CHECK: ->(plugin:[[#PID]]) {"loadPluginLibrary":{"libraryPath":"{{.+}}/Plugin.wasm","moduleName":"MacroDefinition"}}
// CHECK: <-(plugin:[[#PID]]) {"loadPluginLibraryResult":{"diagnostics":[],"loaded":true}}
// CHECK: ->(plugin:[[#PID]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"MacroDefinition","name":"constInt","typeName":"ConstMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":16,"fileID":"MyApp/test.swift","fileName":"{{.+}}test.swift","line":4,"offset":143},"source":"#constInt"}}}
// CHECK: <-(plugin:[[#PID]]) {"expandMacroResult":{"diagnostics":[],"expandedSource":"1"}}

//--- test.swift
@freestanding(expression) macro constInt() -> Int = #externalMacro(module: "MacroDefinition", type: "ConstMacro")

func foo() {
  let _: Int = #constInt
}

//--- MacroDefinition.c
#include "Inputs/wasi_shim.h"

static void write_json(const char *json) {
  wasi_size_t len = swift_strlen(json);
  uint64_t len64 = (uint64_t)len;
  swift_write(1, &len64, sizeof(len64));
  swift_write(1, json, len);
}

int was_start_called = 0;
int pump_calls = 0;

__attribute__((export_name("_start")))
void _start(void) {
  if (was_start_called) swift_abort("_start called twice!");
  was_start_called = 1;
}

__attribute__((export_name("swift_wasm_macro_v1_pump")))
void pump(void) {
  if (!was_start_called) swift_abort("_start not called!");
  if (pump_calls++ != 0) swift_abort("expected pump to be called once");
  write_json("{\"expandMacroResult\": {\"expandedSource\": \"1\", \"diagnostics\": []}}");
}

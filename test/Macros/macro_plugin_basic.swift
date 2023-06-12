// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %clang \
// RUN:  -isysroot %host_sdk \
// RUN:  -I %swift_src_root/include \
// RUN:  -L %swift-lib-dir -l_swiftMockPlugin \
// RUN:  -Wl,-rpath,%swift-lib-dir \
// RUN:  -o %t/mock-plugin \
// RUN:  %t/plugin.c

// RUN: SWIFT_DUMP_PLUGIN_MESSAGING=1 %swift-target-frontend \
// RUN:   -typecheck -verify \
// RUN:   -swift-version 5 \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift \
// RUN:   2>&1 | tee %t/macro-expansions.txt

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// CHECK: ->(plugin:[[#PID:]]) {"getCapability":{}}
// CHECK: <-(plugin:[[#PID]]) {"getCapabilityResult":{"capability":{"protocolVersion":1}}}
// CHECK: ->(plugin:[[#PID]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","macro":{"moduleName":"TestPlugin","name":"testString","typeName":"TestStringMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"BUILD_DIR{{.+}}test.swift","line":5,"offset":301},"source":"#testString(123)"}}}
// CHECK: <-(plugin:[[#PID]]) {"expandFreestandingMacroResult":{"diagnostics":[],"expandedSource":"\"123\"\n  +   \"foo  \""}}
// CHECK: ->(plugin:[[#PID]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","macro":{"moduleName":"TestPlugin","name":"testStringWithError","typeName":"TestStringWithErrorMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"BUILD_DIR{{.+}}test.swift","line":6,"offset":336},"source":"#testStringWithError(321)"}}}
// CHECK: <-(plugin:[[#PID]]) {"expandFreestandingMacroResult":{"diagnostics":[{"fixIts":[],"highlights":[],"message":"message from plugin","notes":[],"position":{"fileName":"BUILD_DIR{{.*}}test.swift","offset":336},"severity":"error"}],"expandedSource":"\"bar\""}}

//--- test.swift
@freestanding(expression) macro testString(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "TestStringMacro")
@freestanding(expression) macro testStringWithError(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "TestStringWithErrorMacro")

func test() {
  let _: String = #testString(123)
  let _: String = #testStringWithError(321)
  // expected-error @-1 {{message from plugin}} 
}

//--- plugin.c
#include "swift-c/MockPlugin/MockPlugin.h"

MOCK_PLUGIN([
  {
    "expect": {"getCapability": {}},
    "response": {"getCapabilityResult": {"capability": {"protocolVersion": 1}}}
  },
  {
    "expect": {"expandFreestandingMacro": {
                "macro": {"moduleName": "TestPlugin", "typeName": "TestStringMacro"},
                "syntax": {"kind": "expression", "source": "#testString(123)"}}},
    "response": {"expandFreestandingMacroResult": {"expandedSource": "\"123\"\n  +   \"foo  \"", "diagnostics": []}}
  },
  {
    "expect": {"expandFreestandingMacro": {
                "macro": {"moduleName": "TestPlugin", "typeName": "TestStringWithErrorMacro"},
                "syntax": {"kind": "expression", "source": "#testStringWithError(321)"}}},
    "response": {"expandFreestandingMacroResult": {
                   "expandedSource": "\"bar\"",
                   "diagnostics": [
                     {"severity": "error",
                      "position": {"offset": "=req.expandFreestandingMacro.syntax.location.offset",
                                   "fileName": "=req.expandFreestandingMacro.syntax.location.fileName"},
                      "message":"message from plugin",
                      "highlights": [],
                      "notes": [],
                      "fixIts": []}
                   ]}}
  }
])

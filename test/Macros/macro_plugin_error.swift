// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %swift-build-c-plugin -o %t/mock-plugin %t/plugin.c

// RUN: env SWIFT_DUMP_PLUGIN_MESSAGING=1 %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -swift-version 5 -enable-experimental-feature Macros \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift \
// RUN:   > %t/macro-expansions.txt 2>&1

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// CHECK: ->(plugin:[[#PID1:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION:]]}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"getCapabilityResult":{"capability":{"protocolVersion":1}}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"{{.+}}test.swift","line":7,"offset":[[#]]},"source":"#fooMacro(1)"}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"invalidResponse":{}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"{{.+}}test.swift","line":9,"offset":[[#]]},"source":"#fooMacro(2)"}}}
// ^ This messages causes the mock plugin exit because there's no matching expected message.

// CHECK: ->(plugin:[[#PID2:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION]]}}}
// CHECK-NEXT: <-(plugin:[[#PID2]]) {"getCapabilityResult":{"capability":{"protocolVersion":1}}}
// CHECK-NEXT: ->(plugin:[[#PID2]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","lexicalContext":[{{.*}}],"macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"macroRole":"expression","syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"{{.+}}test.swift","line":11,"offset":[[#]]},"source":"#fooMacro(3)"}}}
// CHECK-NEXT: <-(plugin:[[#PID2:]]) {"expandFreestandingMacroResult":{"diagnostics":[],"expandedSource":"3.description"}}

//--- test.swift
@freestanding(expression) macro fooMacro(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "FooMacro")

func test() {
  // FIXME: Should be more user friendly message.
  // FIXME: -module-abi-name ABI name is leaking.

  let _: String = #fooMacro(1)
  // expected-error @-1 {{typeMismatch(_CompilerSwiftCompilerPluginMessageHandling.PluginToHostMessage}}
  let _: String = #fooMacro(2)
  // expected-error @-1 {{failed to receive result from plugin (from macro 'fooMacro')}}
  let _: String = #fooMacro(3)
  // ^ This should succeed.
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
                "macro": {"moduleName": "TestPlugin", "typeName": "FooMacro"},
                "syntax": {"kind": "expression", "source": "#fooMacro(1)"}}},
    "response": {"invalidResponse": {}}
  },
  {
    "expect": {"getCapability": {}},
    "response": {"getCapabilityResult": {"capability": {"protocolVersion": 1}}}
  },
  {
    "expect": {"expandFreestandingMacro": {
                "macro": {"moduleName": "TestPlugin", "typeName": "FooMacro"},
                "syntax": {"kind": "expression", "source": "#fooMacro(3)"}}},
    "response": {"expandFreestandingMacroResult": {"expandedSource": "3.description", "diagnostics": []}}
  }
])

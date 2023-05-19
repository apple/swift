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
// RUN:   -swift-version 5 -enable-experimental-feature Macros \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift \
// RUN:   2>&1 | tee %t/macro-expansions.txt

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// CHECK: ->(plugin:[[#PID1:]]) {"getCapability":{}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"getCapabilityResult":{"capability":{"protocolVersion":1}}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"BUILD_DIR{{.+}}test.swift","line":6,"offset":[[#]]},"source":"#fooMacro(1)"}}}
// CHECK-NEXT: <-(plugin:[[#PID1]]) {"invalidResponse":{}}
// CHECK-NEXT: ->(plugin:[[#PID1]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"BUILD_DIR{{.+}}test.swift","line":8,"offset":[[#]]},"source":"#fooMacro(2)"}}}
// ^ This messages causes the mock plugin exit because there's no matching expected message.

// CHECK: ->(plugin:[[#PID2:]]) {"expandFreestandingMacro":{"discriminator":"$s{{.+}}","macro":{"moduleName":"TestPlugin","name":"fooMacro","typeName":"FooMacro"},"syntax":{"kind":"expression","location":{"column":19,"fileID":"MyApp/test.swift","fileName":"BUILD_DIR{{.+}}test.swift","line":10,"offset":[[#]]},"source":"#fooMacro(3)"}}}
// CHECK-NEXT: <-(plugin:[[#PID2:]]) {"expandFreestandingMacroResult":{"diagnostics":[],"expandedSource":"3.description"}}

//--- test.swift
@freestanding(expression) macro fooMacro(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "FooMacro")

func test() {
  // FIXME: Should be more user friendly message.

  let _: String = #fooMacro(1)
  // expected-error @-1 {{typeMismatch(swiftASTGen.PluginToHostMessage}}
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
    "expect": {"expandFreestandingMacro": {
                "macro": {"moduleName": "TestPlugin", "typeName": "FooMacro"},
                "syntax": {"kind": "expression", "source": "#fooMacro(3)"}}},
    "response": {"expandFreestandingMacroResult": {"expandedSource": "3.description", "diagnostics": []}}
  }
])

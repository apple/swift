// REQUIRES: swift_swift_parser
// REQUIRES: CPU=arm64 && OS=macosx
// REQUIRES: rosetta2_runtime

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %clang -isysroot %host_sdk -target x86_64-apple-macosx10.13 -o %t/mock-plugin %t/plugin.cpp

// RUN: env SWIFT_DUMP_PLUGIN_MESSAGING=1 %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -swift-version 5 \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -module-name MyApp \
// RUN:   %t/test.swift \
// RUN:   > %t/macro-expansions.txt 2>&1

// RUN: %FileCheck -strict-whitespace %s < %t/macro-expansions.txt

// CHECK: ->(plugin:[[#PID:]]) {"getCapability":{"capability":{"protocolVersion":[[#PROTOCOL_VERSION:]]}}}
// CHECK: <-(plugin:[[#PID]]) {"getCapabilityResult":{"capability":{"protocolVersion":7}}}
// CHECK: ->(plugin:[[#PID]]) {"expandFreestandingMacro":{{.*$}}
// CHECK: <-(plugin:[[#PID]]) {"expandMacroResult":{"diagnostics":[],"expandedSource":"42"}}

//--- test.swift
@freestanding(expression) macro testInt() -> Int = #externalMacro(module: "TestPlugin", type: "TestIntMacro")

func test() {
  let _: Int =  #testInt
}

//--- plugin.cpp
// dumb plugin just returnig expected responses.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

void send_message(const char *str) {
  uint64_t size = strlen(str);
  fwrite(&size, sizeof(size), 1, stdout);
  fwrite(str, size, 1, stdout);
  fflush(stdout);
}

int main() {
#define JSON(...) #__VA_ARGS__
  send_message(JSON({"getCapabilityResult":{"capability":{"protocolVersion":7}}}));
  send_message(JSON({"expandMacroResult":{"diagnostics":[],"expandedSource":"42"}}));
  sleep(1);
  return 0;
}

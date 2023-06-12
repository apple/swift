// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/plugin)
// RUN: %empty-directory(%t/lib)
// RUN: %empty-directory(%t/src)

// RUN: split-file %s %t/src

//#-- Prepare the macro shared library plugin.
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library -o %t/plugin/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroDefinition \
// RUN:   %S/Inputs/syntax_macro_definitions.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

//#-- Prepare the macro executable plugin.
// RUN: %clang \
// RUN:  -isysroot %host_sdk \
// RUN:  -I %swift_src_root/include \
// RUN:  -L %swift-lib-dir -l_swiftMockPlugin \
// RUN:  -Wl,-rpath,%swift-lib-dir \
// RUN:  -o %t/mock-plugin \
// RUN:  %t/src/plugin.c

//#-- Prepare the macro library.
// RUN: %target-swift-frontend \
// RUN:   -swift-version 5 \
// RUN:   -emit-module -o %t/lib/MacroLib.swiftmodule \
// RUN:   -module-name MacroLib \
// RUN:   -plugin-path %t/plugin \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -primary-file %t/src/macro_library.swift \
// RUN:   -emit-reference-dependencies-path %t/macro_library.swiftdeps \
// RUN:   -emit-dependencies-path %t/macro_library.d
// RUN: %S/../Inputs/process_fine_grained_swiftdeps.sh %swift-dependency-tool %t/macro_library.swiftdeps %t/macro_library.swiftdeps.processed
// RUN: %FileCheck --check-prefix WITH_PLUGIN %s < %t/macro_library.swiftdeps.processed

//#-- Without macro (no -D USE_MACRO)
// RUN: %target-swift-frontend \
// RUN:   -swift-version 5 -typecheck \
// RUN:   -primary-file %t/src/test.swift \
// RUN:   %t/src/other.swift \
// RUN:   -I %t/lib -plugin-path %t/plugin \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -emit-reference-dependencies-path %t/without_macro.swiftdeps \
// RUN:   -emit-dependencies-path %t/without_macro.d
// RUN: %S/../Inputs/process_fine_grained_swiftdeps.sh %swift-dependency-tool %t/without_macro.swiftdeps %t/without_macro.swiftdeps.processed
// RUN: %FileCheck --check-prefix WITHOUT_PLUGIN %s < %t/without_macro.swiftdeps.processed

//#-- With macro - primary (-D USE_MACRO)
// RUN: %target-swift-frontend \
// RUN:   -D USE_MACRO \
// RUN:   -swift-version 5 -typecheck \
// RUN:   -primary-file %t/src/test.swift \
// RUN:   %t/src/other.swift \
// RUN:   -I %t/lib -plugin-path %t/plugin \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -emit-reference-dependencies-path %t/with_macro_primary.swiftdeps \
// RUN:   -emit-dependencies-path %t/with_macro_primary.d
// RUN: %S/../Inputs/process_fine_grained_swiftdeps.sh %swift-dependency-tool %t/with_macro_primary.swiftdeps %t/with_macro_primary.swiftdeps.processed
// RUN: %FileCheck --check-prefix WITH_PLUGIN %s < %t/with_macro_primary.swiftdeps.processed

//#-- With macro - non-primary (-D USE_MACRO)
// RUN: %target-swift-frontend \
// RUN:   -D USE_MACRO \
// RUN:   -swift-version 5 -typecheck \
// RUN:   %t/src/test.swift \
// RUN:   -primary-file %t/src/other.swift \
// RUN:   -I %t/lib -plugin-path %t/plugin \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin \
// RUN:   -emit-reference-dependencies-path %t/with_macro_nonprimary.swiftdeps \
// RUN:   -emit-dependencies-path %t/with_macro_nonprimary.d
// RUN: %S/../Inputs/process_fine_grained_swiftdeps.sh %swift-dependency-tool %t/with_macro_nonprimary.swiftdeps %t/with_macro_nonprimary.swiftdeps.processed
// RUN: %FileCheck --check-prefix WITHOUT_PLUGIN %s < %t/with_macro_nonprimary.swiftdeps.processed

// WITH_PLUGIN: externalDepend interface '' 'BUILD_DIR{{.*}}mock-plugin' false
// WITH_PLUGIN: externalDepend interface '' 'BUILD_DIR{{.*}}libMacroDefinition.{{(dylib|so|dll)}}' false

// WITHOUT_PLUGIN-NOT:  MacroDefinition
// WITHOUT_PLUGIN-NOT:  mock-plugin

//--- macro_library.swift
@freestanding(expression) public macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")
@freestanding(expression) public macro testString(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "TestStringMacro")

public func funcInMacroLib() {}

//--- test.swift
import MacroLib

func test(a: Int, b: Int) {
  // Just using MacroLib without macro
  funcInMacroLib()

#if USE_MACRO
  _ = #stringify(a + b)
  _ = #testString(123)
#endif
}

//--- other.swift
import MacroLib

func test() {
  // Just using MacroLib without macro
  funcInMacroLib()
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
    "response": {"expandFreestandingMacroResult": {"expandedSource": "\"test\"", "diagnostics": []}}
  }
])

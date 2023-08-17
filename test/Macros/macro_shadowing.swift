// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)

// Build macro implementations
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath -swift-version 5

// Build library that declares macros
// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/freestanding_macro_library.swiftmodule %S/Inputs/freestanding_macro_library.swift -module-name freestanding_macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)

// 
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -I %t

import freestanding_macro_library

struct stringify<T> { }

func testStringify(a: Int, b: Int) {
  _ = #stringify(a + b)
}

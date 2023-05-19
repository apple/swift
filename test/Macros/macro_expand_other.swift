// Expanding macros that are defined in terms of other macros.

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -parse-as-library -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// Diagnostics testing
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-experimental-feature FreestandingMacros -load-plugin-library %t/%target-library-name(MacroDefinition) -I %swift-host-lib-dir -module-name MacroUser -DTEST_DIAGNOSTICS

// Execution testing
// RUN: %target-build-swift -swift-version 5 -enable-experimental-feature FreestandingMacros -load-plugin-library %t/%target-library-name(MacroDefinition) -I %swift-host-lib-dir -L %swift-host-lib-dir %s -o %t/main -module-name MacroUser
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s
// REQUIRES: swift_swift_parser, executable_test

@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

@freestanding(expression) macro stringifySeven() -> (Int, String) = #stringify(7)

@freestanding(expression) macro recurse(_: Bool) = #externalMacro(module: "MacroDefinition", type: "RecursiveMacro")

@freestanding(expression) macro recurseThrough(_ value: Bool) = #recurse(value)

func testFreestandingExpansionOfOther() {
  // CHECK: ---testFreestandingExpansionOfOther
  print("---testFreestandingExpansionOfOther")

  // CHECK-NEXT: (7, "7")
  print(#stringifySeven)

  #recurseThrough(false)

  #if TEST_DIAGNOSTICS
#recurseThrough(true)
  // expected-note@-1 {{in expansion of macro 'recurseThrough' here}}
  #endif
}

testFreestandingExpansionOfOther()

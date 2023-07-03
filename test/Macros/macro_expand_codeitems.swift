// REQUIRES: swift_swift_parser, executable_test, asserts
// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath -swift-version 5

// Diagnostics testing
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-experimental-feature CodeItemMacros -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS
// RUN: %target-swift-frontend -swift-version 5 -emit-sil -enable-experimental-feature CodeItemMacros -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -serialize-diagnostics-path %t/macro_expand_codeitems.dia %s -emit-macro-expansion-files no-diagnostics
// RUN: c-index-test -read-diagnostics %t/macro_expand_codeitems.dia 2>&1 | %FileCheck -check-prefix CHECK-DIAGS %s

// Execution testing
// RUN: %target-build-swift -swift-version 5 -g -enable-experimental-feature CodeItemMacros -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@freestanding(codeItem)
macro codeItems() = #externalMacro(module: "MacroDefinition", type: "SimpleCodeItemMacro")

func testFreestandingMacroExpansion() {
  func usedInExpandedStmt() { print("from usedInExpandedStmt") }

  // CHECK: from stmt
  // CHECK: from usedInExpandedStmt
  // CHECK: from expr
  // CHECK-DIAGS: note: condition always evaluates to false
  // CHECK-DIAGS: CONTENTS OF FILE @__swiftmacro_9MacroUser016testFreestandingA9ExpansionyyF9codeItemsfMf0_.swift:
  // CHECK-DIAGS: struct $s9MacroUser016testFreestandingA9ExpansionyyF9codeItemsfMf0_3foofMu_ {
  // CHECK-DIAGS: END CONTENTS OF FILE
  #codeItems

  // CHECK: from stmt
  // CHECK: from usedInExpandedStmt
  // CHECK: from expr
  #codeItems
}
testFreestandingMacroExpansion()

@freestanding(codeItem) macro varDecl() = #externalMacro(module: "MacroDefinition", type: "VarDeclMacro")

func testVarDecl() {
  func use<T>(_ t: T) {}
  #varDecl()
}

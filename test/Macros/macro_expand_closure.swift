// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-build-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s


// Debug info SIL testing
// RUN: %target-swift-frontend -swift-version 5 -emit-sil -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name MacroUser -o - -g | %FileCheck --check-prefix CHECK-SIL %s


@freestanding(expression) public macro multiStatement() -> Int = #externalMacro(module: "MacroDefinition", type: "MultiStatementClosure")

func multiStatementInference() -> Int {
  #multiStatement()
}

// The closure intruduced by the macro expansion should not contain any inline
// locations, but instead point directly into the macro buffer.
// CHECK-SIL: sil_scope [[S0:[0-9]+]] { loc "@__swiftmacro_9MacroUser0031macro_expand_closureswift_yFFIifMX16_2_14multiStatementfMf_.swift":1:1 parent @$s9MacroUser23multiStatementInferenceSiyFSiyXEfU_
// CHECK-SIL: sil_scope [[S2:[0-9]+]] { loc "@__swiftmacro_9MacroUser0031macro_expand_closureswift_yFFIifMX16_2_14multiStatementfMf_.swift":2:14 parent [[S0]] }

// CHECK-SIL: sil {{.*}} @$s9MacroUser23multiStatementInferenceSiyFSiyXEfU_
// CHECK-SIL-NOT: return
// CHECK-SIL: %0 = integer_literal $Builtin.Int{{64|32}}, 10, loc "@__swiftmacro_9MacroUser0031macro_expand_closureswift_yFFIifMX16_2_14multiStatementfMf_.swift":2:14, scope [[S2]]

// CHECK: 10
print(multiStatementInference())

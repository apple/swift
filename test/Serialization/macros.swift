// REQUIRES: swift_swift_parser, asserts

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t-scratch)
// RUN: %host-build-swift -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/def_macro_plugin.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-swift-frontend -emit-module -o %t/def_macros.swiftmodule %S/Inputs/def_macros.swift -module-name def_macros -enable-experimental-feature Macros -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: %target-swift-frontend -typecheck -I%t -verify %s -verify-ignore-unknown  -enable-experimental-feature Macros -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: llvm-bcanalyzer %t/def_macros.swiftmodule | %FileCheck %s
// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -print-module -module-to-print=def_macros -I %t -source-filename=%s | %FileCheck -check-prefix=CHECK-PRINT %s

import def_macros

func test(a: Int, b: Int) {
  _ = #publicStringify(a + b)
  _ = #publicStringify(a + b, label: "hello")
  _ = #unlabeledStringify(a + b)

  _ = #internalStringify(a + b)
  // expected-error@-1{{no macro named 'internalStringify'}}
}

struct TestStruct {
  @myWrapper var x: Int
  // expected-error@-1{{expansion of macro 'myWrapper()' did not produce a non-observing accessor}}
}

@ArbitraryMembers
struct OtherTestStruct {
}

// CHECK: MACRO_DECL

// CHECK-NOT: UnknownCode

// CHECK-PRINT-DAG: macro macroWithBuilderArgs(@Builder _: () -> Void)

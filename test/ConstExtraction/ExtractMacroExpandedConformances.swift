// REQUIRES: swift_swift_parser
// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/Macros.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractFromMacroExpansion.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: cat %t/ExtractFromMacroExpansion.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto { }
protocol MyExtraProto { }

@attached(extension, conformances: MyProto, MyExtraProto)
macro specificExtensionMacro() = #externalMacro(module: "MacroDefinition", type: "AddSpecificExtensionMacro")

@specificExtensionMacro
struct MyStruct {
  struct Inner { }
}

// CHECK:    "typeName": "ExtractMacroExpandedConformances.MyStruct",
// CHECK:    "mangledTypeName": "32ExtractMacroExpandedConformances8MyStructV",
// CHECK:    "kind": "struct",
// CHECK:    "conformances": [
// CHECK-DAG:      "Swift.Sendable",
// CHECK-DAG:      "Swift.BitwiseCopyable",
// CHECK-DAG:      "ExtractMacroExpandedConformances.MyProto"
// CHECK-NOT:      "ExtractMacroExpandedConformances.MyExtraProto"

// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// Make sure we see the conformances from another file.
// RUN: %target-typecheck-verify-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -swift-version 5 -primary-file %S/Inputs/macro_expand_conformances_other.swift -DDISABLE_TOP_LEVEL_CODE

@attached(conformance)
macro Equatable() = #externalMacro(module: "MacroDefinition", type: "EquatableMacro")

@attached(conformance)
macro Hashable() = #externalMacro(module: "MacroDefinition", type: "HashableMacro")

func requireEquatable(_ value: some Equatable) -> Int {
  print(value == value)
  return 0
}

func requireHashable(_ value: some Hashable) {
  print(value.hashValue)
}

@Equatable
struct S {}

protocol MyProtocol {}

@attached(extension, conformances: MyProtocol)
macro ConformanceViaExtension() = #externalMacro(module: "MacroDefinition", type: "ConformanceViaExtensionMacro")

@ConformanceViaExtension
class Parent {}

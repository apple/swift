// REQUIRES: swift_swift_parser, executable_test
//
// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -parse-as-library -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-swift-frontend -parse-as-library -emit-sil -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name MacroUser 2>&1 | %FileCheck --check-prefix CHECK-SIL %s

// Type check testing
// RUN: %target-typecheck-verify-swift -swift-version 5 -parse-as-library -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5  %S/Inputs/top_level_freestanding_other.swift

// Type check testing with imported macro declarations
// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/freestanding_macro_library.swiftmodule %S/Inputs/freestanding_macro_library.swift -module-name freestanding_macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)

// RUN: %target-typecheck-verify-swift -swift-version 5 -parse-as-library -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -DIMPORT_MACRO_LIBRARY -swift-version 5  %S/Inputs/top_level_freestanding_other.swift -I %t

// Check diagnostic buffer names
// RUN: not %target-swift-frontend -typecheck -swift-version 5 -parse-as-library -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5 %s %S/Inputs/top_level_freestanding_other.swift -diagnostic-style llvm 2> %t.diags
// RUN: %FileCheck -check-prefix DIAG_BUFFERS %s < %t.diags

// Execution testing
// RUN: %target-build-swift -g -swift-version 5 -parse-as-library -load-plugin-library %t/%target-library-name(MacroDefinition) %s %S/Inputs/top_level_freestanding_other.swift -o %t/main -module-name MacroUser -swift-version 5
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

#if IMPORT_MACRO_LIBRARY
import freestanding_macro_library
#else
@freestanding(declaration, names: named(StructWithUnqualifiedLookup))
macro structWithUnqualifiedLookup() = #externalMacro(module: "MacroDefinition", type: "DefineStructWithUnqualifiedLookupMacro")
@freestanding(declaration)
macro anonymousTypes(public: Bool = false, causeErrors: Bool = false, _: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")
@freestanding(declaration)
macro introduceTypeCheckingErrors() = #externalMacro(module: "MacroDefinition", type: "IntroduceTypeCheckingErrorsMacro")
@freestanding(declaration)
macro freestandingWithClosure<T>(_ value: T, body: (T) -> T) = #externalMacro(module: "MacroDefinition", type: "EmptyDeclarationMacro")
@freestanding(declaration, names: arbitrary) macro bitwidthNumberedStructs(_ baseName: String) = #externalMacro(module: "MacroDefinition", type: "DefineBitwidthNumberedStructsMacro")
@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")
@freestanding(declaration, names: named(value)) macro varValue() = #externalMacro(module: "MacroDefinition", type: "VarValueMacro")

@freestanding(expression) macro checkGeneric_root<A>() = #externalMacro(module: "MacroDefinition", type: "GenericToVoidMacro")
@freestanding(expression) macro checkGeneric<A>() = #checkGeneric_root<A>()

@freestanding(expression) macro checkGeneric2_root<A, B>() = #externalMacro(module: "MacroDefinition", type: "GenericToVoidMacro")
@freestanding(expression) macro checkGeneric2<A, B>() = #checkGeneric2_root<A, B>()

@freestanding(expression) macro checkGenericHashableCodable_root<A: Hashable, B: Codable>() = #externalMacro(module: "MacroDefinition", type: "GenericToVoidMacro")
@freestanding(expression) macro checkGenericHashableCodable<A: Hashable, B: Codable>() = #checkGenericHashableCodable_root<A, B>()

#endif

// Test unqualified lookup from within a macro expansion

let world = 3 // to be used by the macro expansion below

#structWithUnqualifiedLookup

func lookupGlobalFreestandingExpansion() {
  // CHECK: 4
  print(StructWithUnqualifiedLookup().foo())
}

#anonymousTypes(public: true) { "hello" }

// CHECK-SIL: sil @$s9MacroUser03$s9A115User0033top_level_freestandingswift_DbGHjfMX60_0_33_082AE7CFEFA6960C804A9FE7366EB5A0Ll14anonymousTypesfMf_4namefMu_C5helloSSyF

@main
struct Main {
  static func main() {
    lookupGlobalFreestandingExpansion()
  }
}

// Unqualified lookup for names defined within macro arguments.
#freestandingWithClosure(0) { x in x }

#freestandingWithClosure(1) {
  let x = $0
  return x
}

struct HasInnerClosure {
  #freestandingWithClosure(0) { x in x }
  #freestandingWithClosure(1) { x in x }
}

#if TEST_DIAGNOSTICS
// Arbitrary names at global scope

#bitwidthNumberedStructs("MyIntGlobal")
// expected-error@-1 {{'declaration' macros are not allowed to introduce arbitrary names at global scope}}

func testArbitraryAtGlobal() {
  _ = MyIntGlobal16()
  // expected-error@-1 {{cannot find 'MyIntGlobal16' in scope}}
}
#endif

// DIAG_BUFFERS-DAG: @__swiftmacro_9MacroUser0039top_level_freestanding_otherswift_jrGEmfMX12_17_33_7FDB3F9D78D0279543373AD342C3C331Ll9stringifyfMf1{{.*}}: warning: 'deprecated()' is deprecated
// DIAG_BUFFERS-DAG: @__swiftmacro_9MacroUser0039top_level_freestanding_otherswift_jrGEmfMX16_17_33_7FDB3F9D78D0279543373AD342C3C331Ll9stringifyfMf2{{.*}}: warning: 'deprecated()' is deprecated

#varValue

func testGlobalVariable() {
  _ = value
}

#if TEST_DIAGNOSTICS

// expected-note @+1 6 {{in expansion of macro 'anonymousTypes' here}}
#anonymousTypes(causeErrors: true) { "foo" }
// DIAG_BUFFERS-DAG: @__swiftmacro_9MacroUser0033top_level_freestandingswift_DbGHjfMX108_0_33_082AE7CFEFA6960C804A9FE7366EB5A0Ll14anonymousTypesfMf0_{{.*}}: error: use of protocol 'Equatable' as a type must be written 'any Equatable'
// DIAG_BUFFERS-DAG: @__swiftmacro_9MacroUser00142___swiftmacro_9MacroUser0033top_level_freestandingswift_DbGHjfMX108_0_33_082AE7CFEFA6960C804A9FE7366EB5A0Ll14anonymousTypesfMf0_swift_DAIABdjIbfMX23_2_33_082AE7CFEFA6960C804A9FE7366EB5A0Ll27introduceTypeCheckingErrorsfMf_{{.*}}: error: use of protocol 'Hashable' as a type must be written 'any Hashable'

// expected-note @+1 2 {{in expansion of macro 'anonymousTypes' here}}
#anonymousTypes { () -> String in
  // expected-error @+1 {{use of protocol 'Equatable' as a type must be written 'any Equatable'}}
  _ = 0 as Equatable
  return "foo"
}

#endif


@freestanding(declaration)
macro Empty<T>(_ closure: () -> T) = #externalMacro(module: "MacroDefinition", type: "EmptyDeclarationMacro")

#Empty {
  S(a: 10, b: 10)
}

@attached(extension, conformances: Initializable, names: named(init))
macro Initializable() = #externalMacro(module: "MacroDefinition", type: "InitializableMacro")

protocol Initializable {
  init(value: Int)
}

@Initializable
struct S {
  init(a: Int, b: Int) {}
}

// Check that generic type arguments are passed along in expansions,
// when macro is implemented using another macro.

#checkGeneric<String>()
#checkGeneric2<String, Int>()
#checkGenericHashableCodable<String, Int>()

// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)

// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/../Macros/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -emit-sil -emit-sorted-sil -Xllvm -sil-full-demangle -profile-generate -profile-coverage-mapping -module-name coverage_macros %s -load-plugin-library %t/%target-library-name(MacroDefinition) | %FileCheck %s --implicit-check-not sil_coverage_map
// RUN: %target-swift-frontend -emit-ir -profile-generate -profile-coverage-mapping -load-plugin-library %t/%target-library-name(MacroDefinition) %s

@attached(accessor)
macro accessViaStorage() = #externalMacro(module: "MacroDefinition", type: "AccessViaStorageMacro")

@attached(
  member,
  names: named(`init`), named(Storage), named(storage), named(getStorage()), named(method)
)
macro addMembers() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

@freestanding(expression)
macro nestedDeclInExpr() -> () -> Void = #externalMacro(module: "MacroDefinition", type: "NestedDeclInExprMacro")

// Note we use implicit-check-not, so this test ensures we don't emit
// coverage maps for the macro expansions.

struct S1 {
  // CHECK: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.x
  var x: Int = 0

  // CHECK: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.y
  var y: Int = 0
}

@addMembers
struct S2 {
  // CHECK: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S2.(_storage
  private var _storage = S1()

  @accessViaStorage
  var x: Int

  // No coverage map for the initializer, as it gets subsumed.
  @accessViaStorage
  var y: Int = 17
}

// CHECK: sil_coverage_map{{.*}}s15coverage_macros3fooyyF
func foo() {
  _ = #nestedDeclInExpr()
}

// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -enable-experimental-feature ExtensionMacros -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) %s -I %t -disable-availability-checking -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt
// RUN: %target-typecheck-verify-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5 -I %t
// RUN: %target-build-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5 -emit-tbd -emit-tbd-path %t/MacroUser.tbd -I %t
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@attached(extension, conformances: P, names: named(requirement))
macro DelegatedConformance() = #externalMacro(module: "MacroDefinition", type: "DelegatedConformanceMacro")

protocol P {
  static func requirement()
}

struct Wrapped: P {
  static func requirement() {
    print("Wrapped.requirement")
  }
}

@DelegatedConformance
struct Generic<Element> {}

// CHECK-DUMP: @__swiftmacro_23macro_expand_extensions7Generic20DelegatedConformancefMe_.swift
// CHECK-DUMP: extension Generic: P where Element: P {
// CHECK-DUMP:   static func requirement() {
// CHECK-DUMP:     Element.requirement()
// CHECK-DUMP:   }
// CHECK-DUMP: }

func requiresP(_ value: (some P).Type) {
  value.requirement()
}

// CHECK: Wrapped.requirement
requiresP(Generic<Wrapped>.self)

struct Outer {
  @DelegatedConformance
  struct Nested<Element> {}
}

// CHECK-DUMP: @__swiftmacro_23macro_expand_extensions5OuterV6Nested20DelegatedConformancefMe_.swift
// CHECK-DUMP: extension Outer.Nested: P where Element: P {
// CHECK-DUMP:   static func requirement() {
// CHECK-DUMP:     Element.requirement()
// CHECK-DUMP:   }
// CHECK-DUMP: }

// CHECK: Wrapped.requirement
requiresP(Outer.Nested<Wrapped>.self)

#if TEST_DIAGNOSTICS
func testLocal() {
  @DelegatedConformance
  struct Local<Element> {}
  // expected-error@-1{{local type cannot have attached extension macro}}
}
#endif

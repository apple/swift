// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -enable-experimental-feature ExtensionMacros -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) %s -I %t -disable-availability-checking -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt
// RUN: %target-typecheck-verify-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5 -I %t

// RUN: not %target-swift-frontend -enable-experimental-feature ExtensionMacros -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -serialize-diagnostics-path %t/macro_expand.dia %s -emit-macro-expansion-files no-diagnostics
// RUN: c-index-test -read-diagnostics %t/macro_expand.dia 2>&1 | %FileCheck -check-prefix CHECK-DIAGS %s

// RUN: %target-build-swift -enable-experimental-feature ExtensionMacros -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5 -emit-tbd -emit-tbd-path %t/MacroUser.tbd -I %t
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@attached(extension, conformances: P, names: named(requirement))
macro DelegatedConformance() = #externalMacro(module: "MacroDefinition", type: "DelegatedConformanceViaExtensionMacro")

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

@DelegatedConformance
struct AlreadyConforms: P {
  static func requirement() {}
}

// CHECK-DUMP-LABEL: @__swiftmacro_23macro_expand_extensions15AlreadyConforms20DelegatedConformancefMe_.swift
// CHECK-DUMP-NOT: extension AlreadyConforms

@DelegatedConformance
struct AlreadyConformsInExtension {}

extension AlreadyConformsInExtension: P {
  static func requirement() {}
}

// CHECK-DUMP-LABEL: @__swiftmacro_23macro_expand_extensions26AlreadyConformsInExtension20DelegatedConformancefMe_.swift
// CHECK-DUMP-NOT: extension AlreadyConformsInExtension

class ConformsExplicitly<Element>: P {
  static func requirement() {}
}

@DelegatedConformance
class InheritsConformance<Element>: ConformsExplicitly<Element> {}

// CHECK-DUMP-LABEL: @__swiftmacro_23macro_expand_extensions19InheritsConformance09DelegatedE0fMe_.swift
// CHECK-DUMP-NOT: extension InheritsConformance

@DelegatedConformance
class ConformsViaMacro<Element> {
}

// CHECK-DUMP-LABEL: @__swiftmacro_23macro_expand_extensions16ConformsViaMacro20DelegatedConformancefMe_.swift
// CHECK-DUMP: extension ConformsViaMacro: P where Element: P {
// CHECK-DUMP:   static func requirement() {
// CHECK-DUMP:     Element.requirement()
// CHECK-DUMP:   }
// CHECK-DUMP: }

@DelegatedConformance
class InheritsConformanceViaMacro<Element: P>: ConformsViaMacro<Element> {}

// CHECK-DUMP-LABEL: @__swiftmacro_23macro_expand_extensions27InheritsConformanceViaMacro09DelegatedE0fMe_.swift
// CHECK-DUMP-NOT: extension InheritsConformanceViaMacro

#if TEST_DIAGNOSTICS
func testLocal() {
  @DelegatedConformance
  struct Local<Element> {}
  // expected-error@-1{{local type cannot have attached extension macro}}
}

@DelegatedConformance
typealias A = Int
// expected-error@-1 {{'extension' macro cannot be attached to type alias}}

@DelegatedConformance
extension Int {}
// expected-error@-1 {{'extension' macro cannot be attached to extension}}

@attached(extension, conformances: P)
macro UndocumentedNamesInExtension() = #externalMacro(module: "MacroDefinition", type: "DelegatedConformanceViaExtensionMacro")

@UndocumentedNamesInExtension
struct S<Element> {}
// expected-note@-1 {{in expansion of macro 'UndocumentedNamesInExtension' here}}

// CHECK-DIAGS: error: declaration name 'requirement()' is not covered by macro 'UndocumentedNamesInExtension'
#endif

// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift

@freestanding(declaration)
macro anonymousTypes(public: Bool, _: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")

// RUN: %target-swift-frontend -swift-version 5 -emit-ir -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser %S/Inputs/AnonTypes1.swift %S/Inputs/AnonTypes2.swift %s -o - -g | %FileCheck --check-prefix CHECK-IR %s

// CHECK-IR: $s9MacroUser{{.*}}fMX{{.*}}_33{{.*}}14anonymousTypesfMf_4namefMu_
// CHECK-IR-NOT: $s9MacroUser{{.*}}fMX{{.*}}_33{{.*}}14anonymousTypesfMf0_4namefMu_
// CHECK-IR: $s9MacroUser{{.*}}fMX{{.*}}_33{{.*}}14anonymousTypesfMf_4namefMu_
// CHECK-IR-NOT: $s9MacroUser{{.*}}fMX{{.*}}_33{{.*}}14anonymousTypesfMf0_4namefMu_

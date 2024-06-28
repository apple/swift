// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift

// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/freestanding_macro_library.swiftmodule %S/Inputs/freestanding_macro_library.swift -module-name freestanding_macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)

// RUN: %target-swift-frontend -parse-as-library -enable-library-evolution -emit-sil -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name MacroUser -experimental-skip-non-exportable-decls | %FileCheck %s

@freestanding(declaration)
macro anonymousTypes(public: Bool = false, causeErrors: Bool = false, _: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")

// CHECK: sil @$s9MacroUser03$s9A118User0036skip_non_exportable_declsswift_tjBBlfMX14_0_33_B2D49A1BE4DC7AF5CC327EB8EE2214BDLl14anonymousTypesfMf_4namefMu_C5helloSSyF : $@convention(method) (@guaranteed $s9MacroUser0036skip_non_exportable_declsswift_tjBBlfMX14_0_33_B2D49A1BE4DC7AF5CC327EB8EE2214BDLl14anonymousTypesfMf_4namefMu_) -> @owned String {

#anonymousTypes(public: true) { "hello" }

#anonymousTypes(public: false) { "goodbye" }

// FIXME: Changing 'public: false' to 'public: true' above doesn't seem to
// have any effect on the generated SIL. Perhaps there is a bug here.

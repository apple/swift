
@freestanding(declaration)
macro anonymousTypes(_: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")

#anonymousTypes { "hello" }

// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)

//##-- Prepare the macro plugin.
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/../../Macros/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// This command just dumps the SILGen in case the buffer name changes and you need to find it. It will always pass.
// RUN: %target-swift-frontend -primary-file %s -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -emit-silgen

// Check the output of the `#anonymousTypes` macro
// RUN: %sourcekitd-test -req=semantic-tokens @__swiftmacro_9MacroUser0031macro_semantic_tokenswift_yFFIifMX4_0_33_8C2BB8A10AE555140C0EDFDEB4A9572DLl14anonymousTypesfMf_.swift -primary-file %s -- -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser %s | %FileCheck %s --check-prefix IN_BUFFER

// Check that we get some semantic tokens. Checking exact offsets is brittle.
// IN_BUFFER: source.lang.swift.ref.struct
// IN_BUFFER: key.kind: source.lang.swift.ref.class

// Check that we don't get semantic tokens inside the buffer when requesting semantic tokens for the outer file
// RUN: %sourcekitd-test -req=semantic-tokens %s -- -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser %s | %FileCheck %s --check-prefix PRIMARY_FILE
// Reference to String in line 2
// PRIMARY_FILE: key.kind: source.lang.swift.ref.struct
// Reference to macro in line 4
// PRIMARY_FILE: key.kind: source.lang.swift.ref.macro
// There should be no other references
// PRIMARY_FILE-NOT: key.kind

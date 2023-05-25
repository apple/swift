// REQUIRES: swift_swift_parser

// This test ensures that we only attempt to parse the syntax tree of a
// secondary file if the primary file needs it.

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/stats-no-lookup)
// RUN: %empty-directory(%t/stats-lookup)
// RUN: split-file %s %t

// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -parse-as-library -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -typecheck -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -stats-output-dir %t/stats-no-lookup -primary-file %t/b.swift %t/a.swift
// RUN: %target-swift-frontend -typecheck -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -stats-output-dir %t/stats-lookup -primary-file %t/c.swift %t/a.swift

// We use '<=' here instead of '==' to take account of the fact that in debug
// builds we'll be doing round-trip checking, which will parse the syntax tree
// for the primary.
// RUN: %{python} %utils/process-stats-dir.py --evaluate 'ExportedSourceFileRequest <= 1' %t/stats-no-lookup
// RUN: %{python} %utils/process-stats-dir.py --evaluate 'ExportedSourceFileRequest <= 2' %t/stats-lookup
// RUN: %{python} %utils/process-stats-dir.py --evaluate-delta 'ExportedSourceFileRequest == 1' %t/stats-no-lookup %t/stats-lookup

//--- a.swift

@attached(
  member,
  names: named(init), named(Storage), named(storage), named(getStorage()), named(method), named(init(other:))
)
macro addMembers() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

@addMembers
struct S {}

//--- b.swift

// No lookup into S, so we don't need to evaluate any macros.
func foo(_ x: S) {}

//--- c.swift

// A lookup into S, we need to parse the syntax tree.
func foo(_ x: S) {
  _ = x.getStorage()
}

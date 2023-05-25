// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/stats)
// RUN: %empty-directory(%t/deps)
// RUN: split-file %s %t
// RUN: mv %t/Foo.swiftinterface %t/deps/

// This test ensures we don't attempt to do syntax tree parsing for dependency
// scanning.

// RUN: %target-swift-frontend -scan-dependencies -o %t/deps.json %t/a.swift %t/b.swift -I %t/deps -stats-output-dir %t/stats
// RUN: %{python} %utils/process-stats-dir.py --evaluate 'ExportedSourceFileRequest == 0' %t/stats

//--- Foo.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name Foo

@attached(
  member,
  names: named(init), named(Storage), named(storage), named(getStorage()), named(method), named(init(other:))
)
public macro addMembers() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

@addMembers
public struct S1 {}

//--- a.swift

import Foo

@addMembers
struct S2 {}

//--- b.swift

@freestanding(expression) macro myError(_ message: String) = #externalMacro(module: "MacroDefinition", type: "ErrorMacro")

func foo(_ x: S) {
  _ = x.getStorage()
}

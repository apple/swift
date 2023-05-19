// REQUIRES: swift_swift_parser, OS=macosx

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/src
// RUN: mkdir -p %t/plugins

// RUN: split-file %s %t/src

//#-- Prepare the macro dylib plugin.
// RUN: %host-build-swift \
// RUN:   -swift-version 5 \
// RUN:   -emit-library -o %t/plugins/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroDefinition \
// RUN:   %t/src/MacroDefinition.swift \
// RUN:   -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend \
// RUN:   -typecheck -verify \
// RUN:   -enable-experimental-feature FreestandingMacros \
// RUN:   -parse-as-library \
// RUN:   -dump-macro-expansions \
// RUN:   -plugin-path %t/plugins \
// RUN:   %t/src/test.swift


//--- MacroDefinition.swift
import SwiftDiagnostics
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct GlobalFuncAndVarMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["func globalFunc() {}", "var globalVar: Int { 1 }"]
  }
}

public struct MemberFuncAndVarMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["func memberFunc() {}", "var memberVar: Int { 1 }"]
  }
}

public struct LocalFuncAndVarMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["func LocalFunc() {}", "var LocalVar: Int { 1 }"]
  }
}

public struct FuncFromClosureMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard
      let closure = node.trailingClosure,
      let arg1 = node.argumentList.first?.expression else {
      return []
    }

    return ["""
      func fromClosure() {
        print(\(arg1))
        \(closure.statements)
      }
      """]
  }
}

//--- test.swift

@freestanding(declaration, names: named(globalFunc), named(globalVar)) macro globalDecls() = #externalMacro(module: "MacroDefinition", type: "GlobalFuncAndVarMacro")
@freestanding(declaration, names: named(memberFunc), named(memberVar)) macro memberDecls() = #externalMacro(module: "MacroDefinition", type: "MemberFuncAndVarMacro")
@freestanding(declaration, names: named(localFunc), named(localVar)) macro localDecls() = #externalMacro(module: "MacroDefinition", type: "LocalFuncAndVarMacro")

@available(SwiftStdlib 9999, *)
#globalDecls

func testGlobal() { // expected-note 2 {{add @available attribute to enclosing global function}}
  globalFunc() // expected-error {{'globalFunc()' is only available in macOS 9999 or newer}} expected-note {{add 'if #available' version check}}
  _ = globalVar // expected-error {{'globalVar' is only available in macOS 9999 or newer}} expected-note {{add 'if #available' version check}}
}

struct S {
  @available(SwiftStdlib 9999, *)
  #memberDecls
}
func testMember(value: S) { // expected-note 2 {{add @available attribute to enclosing global function}}
  value.memberFunc() // expected-error {{'memberFunc()' is only available in macOS 9999 or newer}} expected-note {{add 'if #available' version check}}
  _ = value.memberVar // expected-error {{'memberVar' is only available in macOS 9999 or newer}} expected-note {{add 'if #available' version check}}
}

struct T {
  static #memberDecls
}
func testStatic() {
  T.memberFunc() // OK
  _ = T.memberVar // OK
}

func testLocal() {
// FIXME(109376102): Local vars with freestanding macro is not supported yet.
#if false
  #localDecls
  do {
  }
#endif
}

@freestanding(declaration, names: named(fromClosure)) macro funcFromClosureMacro(_: String, _: () -> Void) = #externalMacro(module: "MacroDefinition", type: "FuncFromClosureMacro")

@available(macOS 99, *)
func APIFrom99() -> String { "" }
@available(macOS 999, *)
func APIFrom999() -> String { "" }

@available(macOS 99, *)
#funcFromClosureMacro(APIFrom99()) {
  _ = APIFrom99()
  if #available(macOS 999, *) {
    _ = APIFrom99()
    _ = APIFrom999()
  }
}

struct S1 {
  @available(macOS 99, *)
  #funcFromClosureMacro(APIFrom99()) {
    _ = APIFrom99()
    if #available(macOS 999, *) {
      _ = APIFrom99()
      _ = APIFrom999()
    }
  }
}

// FIXME: Diagnostics could be better.
struct S2 { // expected-note 4 {{add @available attribute to enclosing struct}}
  // expected-note@+3 6 {{in expansion of macro 'funcFromClosureMacro' here}}
  // expected-error@+2 {{'APIFrom99()' is only available in macOS 99 or newer}}
  // expected-error@+2 {{'APIFrom99()' is only available in macOS 99 or newer}} expected-note@+2 {{add 'if #available' version check}}
  #funcFromClosureMacro(APIFrom99()) {
    _ = APIFrom99()
    if #available(macOS 999, *) {
      _ = APIFrom99()
      _ = APIFrom999()
    }
  }
}

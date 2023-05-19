// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

// Check that a macro is automatically formatted and that it keeps any leading
// comments (but not whitespace).

// Create a plugin that adds a new function as a member without any trivia
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroPlugin) -module-name=MacroPlugin %t/MacroPlugin.swift -g -no-toolchain-stdlib-rpath

//--- MacroPlugin.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct AddNamedFuncMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    // TODO: Newline shouldn't be required here. BasicFormat should be adding
    // it.
    let newFunc = FunctionDeclSyntax(
      leadingTrivia: [.newlines(1), .docLineComment("/// My member macro function!"), .newlines(1)],
      modifiers: ModifierListSyntax([
        DeclModifierSyntax(name: .keyword(.public))
      ]),
      identifier: .identifier("newFunc"),
      signature: FunctionSignatureSyntax(
        input: ParameterClauseSyntax(
          parameterList: FunctionParameterListSyntax([]))
      ),
      body: CodeBlockSyntax(
        statements: CodeBlockItemListSyntax([
          CodeBlockItemSyntax(item: .expr(ExprSyntax("_ = 1")))
        ])
      )
    )
    return [
      DeclSyntax(newFunc),
    ]
  }
}

public struct AddPreformattedFuncMacro: MemberMacro {
  public static var formatMode: FormatMode = .disabled

  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let newFunc: DeclSyntax = """

       /// My preformatted member macro function!
    public func preformattedFunc() {
    _ = 2
    }

    """
    return [
      newFunc,
    ]
  }
}

//--- test.swift
@attached(
  member,
  names: named(newFunc)
)
public macro AddNamedFunc() = #externalMacro(module: "MacroPlugin", type: "AddNamedFuncMacro")

@attached(
  member,
  names: named(preformattedFunc)
)
public macro AddPreformattedFunc() = #externalMacro(module: "MacroPlugin", type: "AddPreformattedFuncMacro")

// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=%(line+1):2 %t/test.swift -- -target %target-triple -load-plugin-library %t/%target-library-name(MacroPlugin) %t/test.swift | %FileCheck -check-prefix=EXPAND -strict-whitespace %s
@AddNamedFunc
public struct ExpandTest {
  public func existingFunc() {}
}
// EXPAND: [[@LINE-1]]:1-[[@LINE-1]]:1 (@__swiftmacro_4test10ExpandTest12AddNamedFuncfMm_.swift) "/// My member macro function!
// EXPAND-NEXT: {{^}}public func newFunc() {
// EXPAND-NEXT: {{^}}    _ = 1
// EXPAND-NEXT: {{^}}}"

// RUN: %sourcekitd-test -req=refactoring.inline.macro -pos=%(line+1):1 %t/test.swift -- -target %target-triple -load-plugin-library %t/%target-library-name(MacroPlugin) %t/test.swift | %FileCheck -check-prefix=INLINE %s
@AddNamedFunc
public struct InlineTest {
  public func existingFunc() {}
}
// INLINE: [[@LINE-1]]:1-[[@LINE-1]]:1 (@__swiftmacro_4test10InlineTest12AddNamedFuncfMm_.swift) "
// INLINE-EMPTY:
// INLINE-NEXT: /// My member macro function!
// INLINE-NEXT: {{^}}public func newFunc() {
// INLINE-NEXT: {{^}}    _ = 1
// INLINE-NEXT: {{^}}}
// INLINE-NEXT: {{^}}"

// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=%(line+1):2 %t/test.swift -- -target %target-triple -load-plugin-library %t/%target-library-name(MacroPlugin) %t/test.swift | %FileCheck -check-prefix=PREFORMATTED -strict-whitespace %s
@AddPreformattedFunc
public struct PreformattedTest {
  public func existingFunc() {}
}
// PREFORMATTED: [[@LINE-1]]:1-[[@LINE-1]]:1 (@__swiftmacro_4test16PreformattedTest03AddB4FuncfMm_.swift) "/// My preformatted member macro function!
// PREFORMATTED-NEXT: {{^}}public func preformattedFunc() {
// PREFORMATTED-NEXT: {{^}}_ = 2
// PREFORMATTED-NEXT: {{^}}}"

// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

// Create the plugin
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroPlugin) -module-name=MacroPlugin %t/MacroPlugin.swift -g -no-toolchain-stdlib-rpath

// RUN: not %target-swift-frontend -typecheck -swift-version 5 -load-plugin-library %t/%target-library-name(MacroPlugin) -module-name TestModule %t/TestModule.swift 2>&1 | %FileCheck %s
// CHECK: unexpected token '}' in expanded member list

//--- MacroPlugin.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct InvalidMemberMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let invalidMember: DeclSyntax =
      """
      // no member here
      }
      """

    return [
      invalidMember
    ]
  }
}

//--- TestModule.swift
@attached(member)
public macro InvalidMember() = #externalMacro(module: "MacroPlugin", type: "InvalidMemberMacro")

@InvalidMember
struct TestStruct { }

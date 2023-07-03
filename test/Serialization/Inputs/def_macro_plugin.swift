import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct StringifyMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.argumentList.first?.expression else {
      fatalError("Boom")
    }

    return "(\(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }
}

public struct MyWrapperMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    return []
  }
}

public struct WrapAllProperties: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo parent: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    return []
  }
}

public struct ArbitraryMembersMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      init(coding: String) {
        fatalError("boom")
      }
      """
    ]
  }
}

public struct SendableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let sendableExtension: DeclSyntax =
      """
      extension \(type.trimmed): Sendable {}
      """

    guard let extensionDecl = sendableExtension.as(ExtensionDeclSyntax.self) else {
      return []
    }

    return [extensionDecl]
  }
}

import SwiftDiagnostics
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct AddStructDeclMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
    """
    @AddExtension
    struct MacroAddedStruct {
      var macroAddedStructMember = 1
    }
    """
    ]
  }
}

public struct AddVarDeclMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
    """
    static let macroAddedVar = 2
    """
    ]
  }
}

public struct AddExtensionMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let typeName = declaration.declGroupName
    return protocols.map {
      ("""
      extension \(typeName): \($0) {
        struct _Extension_\($0): \($0) {
          var nested = 8
        }
      }
      """ as DeclSyntax)
      .cast(ExtensionDeclSyntax.self)
    } + [
    ("""
    extension \(typeName) {
      static let _extension_\(typeName) = 3
    }
    """ as DeclSyntax).cast(ExtensionDeclSyntax.self)
    ]
  }
}

public struct AddSpecificExtensionMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    var extensions = [ExtensionDeclSyntax]()
    let protocolNames = Set(protocols.compactMap { $0.as(IdentifierTypeSyntax.self)?.name.text })
    if protocolNames.contains("MyProto") {
        extensions.append(try ExtensionDeclSyntax("extension \(type.trimmed): MyProto") { })
    }
    return extensions
  }
}

public struct AddPeerVarMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let name = declaration.declName
    return [
    """
    static var _peer_\(name) = 4
    """
    ]
  }
}

public struct AddMemberMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let typeName = declaration.declGroupName
    return [
    """
    static let _member_\(typeName) = 5
    """
    ]
  }
}

public struct AddMemberAttributeMacro: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    if member.isProtocol(DeclGroupSyntax.self) {
      return ["@AddExtension"]
    }
    return ["@AddPeerVar"]
  }
}

public struct GetterMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    return ["get { 6 }"]
  }
}

public struct AddPeerStructMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let name = declaration.declName
    return [
    """
    @AddExtension
    @AddMemberVar
    struct _Peer_\(name) {
      #AddMacroAddedVar
      
      @AddPeerVar
      var peerMacroVar = 7
    }
    """
    ]
  }
}

extension DeclGroupSyntax {
  var declGroupName: TokenSyntax {
    if let structDecl = self.as(StructDeclSyntax.self) {
      return structDecl.name.trimmed
    }
    fatalError("Not implemented")
  }
}

extension DeclSyntaxProtocol {
  var declName: TokenSyntax {
    if let varDecl = self.as(VariableDeclSyntax.self),
       let first = varDecl.bindings.first,
       let pattern = first.pattern.as(IdentifierPatternSyntax.self) {
      return pattern.identifier.trimmed
    } else if let funcDecl = self.as(FunctionDeclSyntax.self) {
      return funcDecl.name.trimmed
    } else if let structDecl = self.as(StructDeclSyntax.self) {
      return structDecl.name.trimmed
    }
    fatalError("Not implemented")
  }
}

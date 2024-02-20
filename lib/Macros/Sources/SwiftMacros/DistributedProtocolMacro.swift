//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

@_spi(RawSyntax) import SwiftSyntax
import SwiftSyntaxMacros
import SwiftDiagnostics
import SwiftOperators
import SwiftSyntaxBuilder


/// Introduces:
/// - `distributed actor $MyDistributedActor<ActorSystem>: $MyDistributedActor, _DistributedActorStub where ...`
/// - `extension MyDistributedActor where Self: _DistributedActorStub {}`
public struct DistributedProtocolMacro: ExtensionMacro, PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      let message: ErrorMessage = "must be attached to  a protocol"
      context.diagnose(node: node, error: message)
      return []
    }

    let access = proto.accessLevelString

    let extensionDecl: DeclSyntax =
      """
      extension \(proto.name.trimmed) where Self: Distributed._DistributedActorStub {
        \(raw: proto.distributedRequirementStubs(access: access))
      }
      """
    return [extensionDecl.cast(ExtensionDeclSyntax.self)]
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      return []
    }

    // FIXME must detect this off the protocol
    let serializationRequirementType =
      "Codable"

    let access = proto.accessLevelString

    let stubActorDecl: DeclSyntax =
      """
      \(raw: access)distributed actor $\(proto.name.trimmed)<ActorSystem>: \(proto.name.trimmed), 
        Distributed._DistributedActorStub
        where ActorSystem: DistributedActorSystem<any \(raw: serializationRequirementType)>, 
          ActorSystem.ActorID: \(raw: serializationRequirementType) 
      {
        \(raw: proto.distributedRequirementStubs(access: access))
      }
      """

    return [stubActorDecl]
  }

}

// ===== ---------------------------------------------------------------------
// MARK: - Diagnostics

fileprivate struct ErrorMessage: DiagnosticMessage, ExpressibleByStringInterpolation {
  init(stringLiteral value: String) {
    self.message = value
  }
  var message: String
  var diagnosticID: MessageID { .init(domain: "DistributedActorProtocol", id: "DistributedActorProtocol")}
  var severity: DiagnosticSeverity { .error }
}

extension MacroExpansionContext {
  fileprivate func diagnose(node: some SyntaxProtocol, error message: ErrorMessage) {
    diagnose(Diagnostic(node: node, message: message))
  }
}

// ===== ---------------------------------------------------------------------
// MARK: - Syntax helpers

extension ProtocolDeclSyntax {
  fileprivate var protocolRequirements: [MemberBlockItemSyntax] {
    memberBlock.members.map { member in
      member.trimmed
    }
  }

  fileprivate func distributedRequirementStubs(access: String) -> String {
    return protocolRequirements
      .map { req in
        """
        \(access)\(req) {
            if #available(SwiftStdlib 5.11, *) {
              Distributed._distributedStubFatalError()
            } else {
              fatalError("distributed method stud: \\(#function)")
            }
        }
        """
      }
      .joined(separator: "\n    ")
  }

  fileprivate var accessLevelString: String {
    for modifier in modifiers {
      for token in modifier.tokens(viewMode: .all) {
        switch token.tokenKind {
        case .keyword(.public),
             .keyword(.package),
             .keyword(.internal),
             .keyword(.fileprivate),
             .keyword(.private):
          if let text = token.tokenKind.defaultText {
            return "\(text) "
          } else {
            return ""
          }
        default: continue
        }
      }
    }
    return ""
  }
}
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

import SwiftSyntax
import SwiftSyntaxMacros
import SwiftDiagnostics
import SwiftSyntaxBuilder

/// Introduces:
/// - `distributed actor $MyDistributedActor<ActorSystem>: $MyDistributedActor, _DistributedActorStub where ...`
/// - `extension MyDistributedActor where Self: _DistributedActorStub {}`
public struct DistributedResolvableMacro: ExtensionMacro, PeerMacro {
}

// ===== -----------------------------------------------------------------------
// MARK: Default Stub implementations Extension

extension DistributedResolvableMacro {

  /// Introduce the `extension MyDistributedActor` which contains default
  /// implementations of the protocol's requirements.
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      // we diagnose here, only once
      try throwIllegalTargetDecl(node: node, declaration)
    }

    guard !proto.memberBlock.members.isEmpty else {
      // ok, the protocol has no requirements so we no-op it
      return []
    }

    let accessModifiers = proto.accessControlModifiers

    let requirementStubs =
      proto.memberBlock.members // requirements
        .filter { member in
          switch member.decl.kind {
          case .functionDecl: return true
          case .variableDecl: return true
          default:
            return false
          }
        }
        .map { member in
          stubMethodDecl(access: accessModifiers, member.trimmed)
        }
        .joined(separator: "\n    ")

    let extensionDecl: DeclSyntax =
      """
      extension \(proto.name.trimmed) where Self: Distributed._DistributedActorStub {
        \(raw: requirementStubs)
      }
      """
    return [extensionDecl.cast(ExtensionDeclSyntax.self)]
  }

  static func stubMethodDecl(access: DeclModifierListSyntax, _ requirement: MemberBlockItemListSyntax.Element) -> String {
    // do we need to stub a computed variable?
    if let variable = requirement.decl.as(VariableDeclSyntax.self) {
      var accessorStubs: [String] = []

      for binding in variable.bindings {
        if let accessorBlock = binding.accessorBlock {
          for accessor in accessorBlock.accessors.children(viewMode: .all) {
            let accessorStub = "\(accessor) { \(stubFunctionBody()) }"
            accessorStubs.append(accessorStub)
          }
        }
      }

      let name = variable.bindings.first!.pattern.trimmed
      let typeAnnotation = variable.bindings.first?.typeAnnotation.map { "\($0.trimmed)" } ?? "Any"
      return """
             \(access)\(variable.modifiers)\(variable.bindingSpecifier) \(name) \(typeAnnotation) {
               \(accessorStubs.joined(separator: "\n  "))
             }
             """
    }

    // normal function stub
    return """
           \(access)\(requirement) {
             \(stubFunctionBody())
           }
           """
  }

  static func stubFunctionBody() -> DeclSyntax {
    """
    if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
      Distributed._distributedStubFatalError()
    } else {
      fatalError()
    }
    """
  }
}

// ===== -----------------------------------------------------------------------
// MARK: Distributed Actor Stub type

extension DistributedResolvableMacro {

  /// Introduce the `distributed actor` stub type.
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      // don't diagnose here (again),
      // we'll already report an error here from the other macro role
      return []
    }

    var isGenericStub = false
    var specificActorSystemRequirement: TypeSyntax?

    let accessModifiers = proto.accessControlModifiers

    for req in proto.genericWhereClause?.requirements ?? [] {
      switch req.requirement {
      case .conformanceRequirement(let conformanceReq)
           where conformanceReq.leftType.isActorSystem:
        specificActorSystemRequirement = conformanceReq.rightType.trimmed
        isGenericStub = true

      case .sameTypeRequirement(let sameTypeReq)
           where sameTypeReq.leftType.isActorSystem:
        specificActorSystemRequirement = sameTypeReq.rightType.trimmed
        isGenericStub = false

      default:
        continue
      }
    }

    if isGenericStub, let specificActorSystemRequirement {
      return [
        """
        \(proto.modifiers) distributed actor $\(proto.name.trimmed)<ActorSystem>: \(proto.name.trimmed), 
          Distributed._DistributedActorStub
          where ActorSystem: \(specificActorSystemRequirement) 
        { }
        """
      ]
    } else if let specificActorSystemRequirement {
      return [
        """
        \(proto.modifiers) distributed actor $\(proto.name.trimmed): \(proto.name.trimmed), 
          Distributed._DistributedActorStub
        { 
          \(typealiasActorSystem(access: accessModifiers, proto, specificActorSystemRequirement)) 
        }
        """
      ]
    } else {
      // there may be no `where` clause specifying an actor system,
      // but perhaps there is a typealias (or extension with a typealias),
      // specifying a concrete actor system so we let this synthesize
      // an empty `$Greeter` -- this may fail, or succeed depending on
      // surrounding code using a default distributed actor system,
      // or extensions providing it.
      return [
        """
        \(proto.modifiers) distributed actor $\(proto.name.trimmed): \(proto.name.trimmed), 
          Distributed._DistributedActorStub
        {
        }
        """
      ]
    }
  }

  private static func typealiasActorSystem(access: DeclModifierListSyntax,
                                           _ proto: ProtocolDeclSyntax,
                                           _ type: TypeSyntax) -> DeclSyntax {
    "\(access)typealias ActorSystem = \(type)"
  }
}

// ===== -----------------------------------------------------------------------
// MARK: Convenience Extensions

extension TypeSyntax {
  fileprivate var isActorSystem: Bool {
    self.trimmedDescription == "ActorSystem"
  }
}

extension DeclSyntaxProtocol {
  var isClass: Bool {
    return self.is(ClassDeclSyntax.self)
  }

  var isActor: Bool {
    return self.is(ActorDeclSyntax.self)
  }

  var isEnum: Bool {
    return self.is(EnumDeclSyntax.self)
  }

  var isStruct: Bool {
    return self.is(StructDeclSyntax.self)
  }
}

extension DeclModifierSyntax {
  var isAccessControl: Bool {
    switch self.name.tokenKind {
    case .keyword(.private): fallthrough
    case .keyword(.fileprivate): fallthrough
    case .keyword(.internal): fallthrough
    case .keyword(.package): fallthrough
    case .keyword(.public):
      return true
    default:
      return false
    }
  }
}

// ===== -----------------------------------------------------------------------
// MARK: @Distributed.Resolvable macro errors

extension DistributedResolvableMacro {
  static func throwIllegalTargetDecl(node: AttributeSyntax, _ declaration: some DeclSyntaxProtocol) throws -> Never {
    let kind: String
    if declaration.isClass {
      kind = "class"
    } else if declaration.isActor {
      kind = "actor"
    } else if declaration.isStruct {
      kind = "struct"
    } else if declaration.isStruct {
      kind = "enum"
    } else {
      kind = "\(declaration.kind)"
    }

    throw DiagnosticsError(
      syntax: node,
      message: "'@Resolvable' can only be applied to 'protocol', but was attached to '\(kind)'", id: .invalidApplication)
  }
}

struct DistributedResolvableMacroDiagnostic: DiagnosticMessage {
  enum ID: String {
    case invalidApplication = "invalid type"
    case missingInitializer = "missing initializer"
  }

  var message: String
  var diagnosticID: MessageID
  var severity: DiagnosticSeverity

  init(message: String, diagnosticID: SwiftDiagnostics.MessageID, severity: SwiftDiagnostics.DiagnosticSeverity = .error) {
    self.message = message
    self.diagnosticID = diagnosticID
    self.severity = severity
  }

  init(message: String, domain: String, id: ID, severity: SwiftDiagnostics.DiagnosticSeverity = .error) {
    self.message = message
    self.diagnosticID = MessageID(domain: domain, id: id.rawValue)
    self.severity = severity
  }
}

extension DiagnosticsError {
  init<S: SyntaxProtocol>(
    syntax: S,
    message: String,
    domain: String = "Distributed",
    id: DistributedResolvableMacroDiagnostic.ID,
    severity: SwiftDiagnostics.DiagnosticSeverity = .error) {
    self.init(diagnostics: [
      Diagnostic(
        node: Syntax(syntax),
        message: DistributedResolvableMacroDiagnostic(
          message: message,
          domain: domain,
          id: id,
          severity: severity))
    ])
  }
}

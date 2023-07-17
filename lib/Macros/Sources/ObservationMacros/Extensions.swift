//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

import SwiftSyntax
import SwiftSyntaxMacros

@_implementationOnly import SwiftDiagnostics
@_implementationOnly import SwiftOperators
@_implementationOnly import SwiftSyntaxBuilder

extension VariableDeclSyntax {
  var identifierPattern: IdentifierPatternSyntax? {
    bindings.first?.pattern.as(IdentifierPatternSyntax.self)
  }
  
  var isInstance: Bool {
    if let modifiers {
      for modifier in modifiers {
        for token in modifier.tokens(viewMode: .all) {
          if token.tokenKind == .keyword(.static) || token.tokenKind == .keyword(.class) {
            return false
          }
        }
      }
    }
    return true
  }
  
  var identifier: TokenSyntax? {
    identifierPattern?.identifier
  }
  
  var type: TypeSyntax? {
    bindings.first?.typeAnnotation?.type
  }

  func accessorsMatching(_ predicate: (TokenKind) -> Bool) -> [AccessorDeclSyntax] {
    let patternBindings = bindings.compactMap { binding in
      binding.as(PatternBindingSyntax.self)
    }
    let accessors: [AccessorListSyntax.Element] = patternBindings.compactMap { patternBinding in
      switch patternBinding.accessor {
      case .accessors(let accessors):
        return accessors
      default:
        return nil
      }
    }.flatMap { $0.accessors }
    return accessors.compactMap { accessor in
      guard let decl = accessor.as(AccessorDeclSyntax.self) else {
        return nil
      }
      if predicate(decl.accessorSpecifier.tokenKind) {
        return decl
      } else {
        return nil
      }
    }
  }
  
  var willSetAccessors: [AccessorDeclSyntax] {
    accessorsMatching { $0 == .keyword(.willSet) }
  }
  var didSetAccessors: [AccessorDeclSyntax] {
    accessorsMatching { $0 == .keyword(.didSet) }
  }
  
  var isComputed: Bool {
    if accessorsMatching({ $0 == .keyword(.get) }).count > 0 {
      return true
    } else {
      return bindings.compactMap { binding in
        binding.as(PatternBindingSyntax.self)?.accessor?.as(CodeBlockSyntax.self)
      }.count > 0
    }
  }
  
  
  var isImmutable: Bool {
    return bindingSpecifier.tokenKind == .keyword(.let)
  }
  
  func isEquivalent(to other: VariableDeclSyntax) -> Bool {
    if isInstance != other.isInstance {
      return false
    }
    return identifier?.text == other.identifier?.text
  }
  
  var initializer: InitializerClauseSyntax? {
    bindings.first?.initializer
  }
  
  func hasMacroApplication(_ name: String) -> Bool {
    guard let attributes else { return false }
    for attribute in attributes {
      switch attribute {
      case .attribute(let attr):
        if attr.attributeName.tokens(viewMode: .all).map({ $0.tokenKind }) == [.identifier(name)] {
          return true
        }
      default:
        break
      }
    }
    return false
  }
}

extension TypeSyntax {
  var identifier: String? {
    for token in tokens(viewMode: .all) {
      switch token.tokenKind {
      case .identifier(let identifier):
        return identifier
      default:
        break
      }
    }
    return nil
  }
  
  func genericSubstitution(_ parameters: GenericParameterListSyntax?) -> String? {
    var genericParameters = [String : TypeSyntax?]()
    if let parameters {
      for parameter in parameters {
        genericParameters[parameter.name.text] = parameter.inheritedType
      }
    }
    var iterator = self.asProtocol(TypeSyntaxProtocol.self).tokens(viewMode: .sourceAccurate).makeIterator()
    guard let base = iterator.next() else {
      return nil
    }
    
    if let genericBase = genericParameters[base.text] {
      if let text = genericBase?.identifier {
        return "some " + text
      } else {
        return nil
      }
    }
    var substituted = base.text
    
    while let token = iterator.next() {
      switch token.tokenKind {
      case .leftAngle:
        substituted += "<"
      case .rightAngle:
        substituted += ">"
      case .comma:
        substituted += ","
      case .identifier(let identifier):
        let type: TypeSyntax = "\(raw: identifier)"
        guard let substituedType = type.genericSubstitution(parameters) else {
          return nil
        }
        substituted += substituedType
        break
      default:
        // ignore?
        break
      }
    }
    
    return substituted
  }
}

extension FunctionDeclSyntax {
  var isInstance: Bool {
    if let modifiers {
      for modifier in modifiers {
        for token in modifier.tokens(viewMode: .all) {
          if token.tokenKind == .keyword(.static) || token.tokenKind == .keyword(.class) {
            return false
          }
        }
      }
    }
    return true
  }
  
  struct SignatureStandin: Equatable {
    var isInstance: Bool
    var identifier: String
    var parameters: [String]
    var returnType: String
  }
  
  var signatureStandin: SignatureStandin {
    var parameters = [String]()
    for parameter in signature.input.parameterList {
      parameters.append(parameter.firstName.text + ":" + (parameter.type.genericSubstitution(genericParameterClause?.parameters) ?? "" ))
    }
    let returnType = signature.output?.returnType.genericSubstitution(genericParameterClause?.parameters) ?? "Void"
    return SignatureStandin(isInstance: isInstance, identifier: identifier.text, parameters: parameters, returnType: returnType)
  }
  
  func isEquivalent(to other: FunctionDeclSyntax) -> Bool {
    return signatureStandin == other.signatureStandin
  }
}

extension DeclGroupSyntax {
  var memberFunctionStandins: [FunctionDeclSyntax.SignatureStandin] {
    var standins = [FunctionDeclSyntax.SignatureStandin]()
    for member in memberBlock.members {
      if let function = member.as(MemberDeclListItemSyntax.self)?.decl.as(FunctionDeclSyntax.self) {
        standins.append(function.signatureStandin)
      }
    }
    return standins
  }
  
  func hasMemberFunction(equvalentTo other: FunctionDeclSyntax) -> Bool {
    for member in memberBlock.members {
      if let function = member.as(MemberDeclListItemSyntax.self)?.decl.as(FunctionDeclSyntax.self) {
        if function.isEquivalent(to: other) {
          return true
        }
      }
    }
    return false
  }
  
  func hasMemberProperty(equivalentTo other: VariableDeclSyntax) -> Bool {
    for member in memberBlock.members {
      if let variable = member.as(MemberDeclListItemSyntax.self)?.decl.as(VariableDeclSyntax.self) {
        if variable.isEquivalent(to: other) {
          return true
        }
      }
    }
    return false
  }
  
  var definedVariables: [VariableDeclSyntax] {
    memberBlock.members.compactMap { member in
      if let variableDecl = member.as(MemberDeclListItemSyntax.self)?.decl.as(VariableDeclSyntax.self) {
        return variableDecl
      }
      return nil
    }
  }
  
  func addIfNeeded(_ decl: DeclSyntax?, to declarations: inout [DeclSyntax]) {
    guard let decl else { return }
    if let fn = decl.as(FunctionDeclSyntax.self) {
      if !hasMemberFunction(equvalentTo: fn) {
        declarations.append(decl)
      }
    } else if let property = decl.as(VariableDeclSyntax.self) {
      if !hasMemberProperty(equivalentTo: property) {
        declarations.append(decl)
      }
    }
  }
  
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

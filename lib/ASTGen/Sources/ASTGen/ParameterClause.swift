//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import CASTBridging
import SwiftSyntax

// MARK: - ParamDecl

fileprivate protocol ValueParameterSyntax: SyntaxProtocol {
  /// The `firstName` with optional type.
  ///
  /// This is the lowest denominator between `FunctionParameterSyntax` and `EnumCaseParameterSyntax`.
  // FIXME: Rename once we support covariant witnesses.
  var optionalFirstName: TokenSyntax? { get }

  var secondName: TokenSyntax? { get }

  /// The `firstName` with optional type.
  ///
  /// This is the lowest denominator between `FunctionParameterSyntax` and `EnumCaseParameterSyntax`.
  // FIXME: Rename once we support covariant witnesses.
  var optionalType: TypeSyntax? { get }

  var defaultValue: InitializerClauseSyntax? { get }
}

extension FunctionParameterSyntax: ValueParameterSyntax {
  fileprivate var optionalFirstName: TokenSyntax? {
    firstName
  }

  fileprivate var optionalType: TypeSyntax? {
    type
  }
}

extension EnumCaseParameterSyntax: ValueParameterSyntax {
  fileprivate var optionalFirstName: TokenSyntax? {
    firstName
  }

  fileprivate var optionalType: TypeSyntax? {
    type
  }
}

extension ASTGenVisitor {
  private func getArgumentLabel(_ token: TokenSyntax?) -> (swift.Identifier, swift.SourceLoc) {
    guard let token = token else { return (.init(), .init()) }
    let loc = token.sourceLoc(in: self)
    if token.tokenKind == .wildcard {
      // Swift AST represents "_" as a null identifier.
      return (.init(), loc)
    } else {
      return (token.identifier(in: self), loc)
    }
  }

  private func makeParamDecl(_ node: some ValueParameterSyntax) -> UnsafeMutablePointer<swift.ParamDecl> {
    // FIXME: This location should be derived from the type repr.
    let specifierLoc: swift.SourceLoc = .init()

    let (firstName, firstNameLoc) = getArgumentLabel(node.optionalFirstName)

    let secondName: swift.Identifier
    let secondNameLoc: swift.SourceLoc
    if let secondNameToken = node.secondName {
      (secondName, secondNameLoc) = getArgumentLabel(secondNameToken)
    } else {
      secondName = firstName
      secondNameLoc = firstNameLoc
    }

    return swift.ParamDecl.createParsed(
      self.ctx,
      specifierLoc: specifierLoc,
      argumentNameLoc: firstNameLoc,
      argumentName: firstName,
      parameterNameLoc: secondNameLoc,
      parameterName: secondName,
      typeRepr: self.generate(node.optionalType),
      defaultValue: self.generate(node.defaultValue?.value)?.expr,
      declContext: self.declContext
    )
  }

  func generate(_ node: FunctionParameterSyntax) -> UnsafeMutablePointer<swift.ParamDecl> {
    self.makeParamDecl(node)
  }

  func generate(_ node: EnumCaseParameterSyntax) -> UnsafeMutablePointer<swift.ParamDecl> {
    self.makeParamDecl(node)
  }
}

// MARK: - ParameterList

extension ASTGenVisitor {
  func generate(_ node: FunctionParameterClauseSyntax) -> swift.BridgableParameterList {
    .createParsed(
      ctx,
      lParenLoc: node.leftParen.sourceLoc(in: self),
      params: .init(from: node.parameters.lazy.map(self.generate), in: self),
      rParenLoc: node.rightParen.sourceLoc(in: self)
    )
  }

  func generate(_ node: EnumCaseParameterClauseSyntax) -> swift.BridgableParameterList {
    .createParsed(
      ctx,
      lParenLoc: node.leftParen.sourceLoc(in: self),
      params: .init(from: node.parameters.lazy.map(self.generate), in: self),
      rParenLoc: node.rightParen.sourceLoc(in: self)
    )
  }
}

import CASTBridging
import CBasicBridging
import SwiftSyntax

extension ASTGenVisitor {
  func generate(_ node: GenericParameterClauseSyntax) -> swift.BridgableGenericParamList {
    let whereClause = node.genericWhereClause
    return .createParsed(
      self.ctx,
      lAngleLoc: node.leftAngle.sourceLoc(in: self),
      params: .init(from: node.parameters.lazy.map(self.generate), in: self),
      whereLoc: (whereClause?.whereKeyword).sourceLoc(in: self),
      requirements: .init(from: whereClause?.requirements.lazy.map(self.generate), in: self),
      rAngleLoc: node.rightAngle.sourceLoc(in: self)
    )
  }

  func generate(_ node: GenericParameterSyntax) -> swift.BridgableGenericTypeParamDecl {
    let (name, nameLoc) = node.name.identifierAndSourceLoc(in: self)

    var genericParameterIndex: Int?
    for (index, sibling) in (node.parent?.as(GenericParameterListSyntax.self) ?? []).enumerated() {
      if sibling == node {
        genericParameterIndex = index
        break
      }
    }
    guard let genericParameterIndex = genericParameterIndex else {
      preconditionFailure("Node not part of the parent?")
    }

    return .createParsed(
      declContext: declContext, name: name, nameLoc: nameLoc,
      eachLoc: node.eachKeyword.sourceLoc(in: self),
      index: UInt32(genericParameterIndex),
      inherited: self.generate(node.inheritedType)
    )
  }

  func generate(_ node: GenericRequirementSyntax) -> swift.RequirementRepr {
    switch node.requirement {
    case .conformanceRequirement(let conformance):
      return .getTypeConstraint(
        subject: self.generate(conformance.leftType),
        colonLoc: conformance.colon.sourceLoc(in: self),
        constraint: self.generate(conformance.rightType),
        isExpansionPattern: false
      )
    case .sameTypeRequirement(let sameType):
      return .getSameType(
        first: self.generate(sameType.leftType),
        equalLoc: sameType.equal.sourceLoc(in: self),
        second: self.generate(sameType.rightType),
        isExpansionPattern: false
      )
    case .layoutRequirement(_):
      // FIXME: Implement layout requirement translation.
      fatalError("Translation of layout requirements not implemented!")
    }
  }

  func generate(_ node: GenericWhereClauseSyntax) -> swift.BridgableTrailingWhereClause {
    .create(
      self.ctx,
      whereLoc: node.whereKeyword.sourceLoc(in: self),
      requirements: .init(from: node.requirements.lazy.map(self.generate), in: self)
    )
  }
}

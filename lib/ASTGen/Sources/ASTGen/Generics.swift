import CASTBridging
import SwiftParser

// Needed to use SyntaxTransformVisitor's visit method.
@_spi(SyntaxTransformVisitor)
import SwiftSyntax

extension ASTGenVisitor {
  func visit(_ node: GenericParameterClauseSyntax) -> ASTNode {
    let lAngleLoc = bridgedSourceLoc(for: node.leftAngle)
    let whereLoc = bridgedSourceLoc(for: node.genericWhereClause?.whereKeyword)
    let rAngleLoc = bridgedSourceLoc(for: node.rightAngle)
    return .misc(
      self.withBridgedParametersAndRequirements(node) { params, reqs in
        return GenericParamList_create(self.ctx, lAngleLoc, params, whereLoc, reqs, rAngleLoc)
      })
  }

  func visit(_ node: GenericParameterSyntax) -> ASTNode {
    var nodeName = node.name.text
    let name = nodeName.withBridgedString { bridgedName in
      return ASTContext_getIdentifier(ctx, bridgedName)
    }
    let nameLoc = bridgedSourceLoc(for: node.name)
    let eachLoc = bridgedSourceLoc(for: node.eachKeyword)

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

    return .decl(
      GenericTypeParamDecl_create(
        self.ctx, self.declContext, name, nameLoc, eachLoc, genericParameterIndex,
        eachLoc.raw != nil))
  }
}

extension ASTGenVisitor {
  private func withBridgedParametersAndRequirements<T>(
    _ node: GenericParameterClauseSyntax,
    action: (BridgedArrayRef, BridgedArrayRef) -> T
  ) -> T {
    var params = [UnsafeMutableRawPointer]()
    var requirements = [BridgedRequirementRepr]()
    for param in node.parameters {
      let loweredParameter = self.visit(param).rawValue
      params.append(loweredParameter)

      guard let requirement = param.inheritedType else {
        continue
      }

      let loweredRequirement = self.visit(requirement)
      GenericTypeParamDecl_setInheritedType(self.ctx, loweredParameter, loweredRequirement.rawValue)
    }

    if let nodeRequirements = node.genericWhereClause?.requirements {
      for requirement in nodeRequirements {
        switch requirement.requirement {
        case .conformanceRequirement(let conformance):
          let firstType = self.visit(conformance.leftType).rawValue
          let separatorLoc = bridgedSourceLoc(for: conformance.colon)
          let secondType = self.visit(conformance.rightType).rawValue
          requirements.append(
            BridgedRequirementRepr(
              SeparatorLoc: separatorLoc,
              Kind: .typeConstraint,
              FirstType: firstType,
              SecondType: secondType))
        case .sameTypeRequirement(let sameType):
          let firstType = self.visit(sameType.leftType).rawValue
          let separatorLoc = bridgedSourceLoc(for: sameType.equal)
          let secondType = self.visit(sameType.rightType).rawValue
          requirements.append(
            BridgedRequirementRepr(
              SeparatorLoc: separatorLoc,
              Kind: .sameType,
              FirstType: firstType,
              SecondType: secondType))
        case .layoutRequirement(_):
          fatalError("Cannot handle layout requirements!")
        }
      }
    }
    return params.withBridgedArrayRef { params in
      return requirements.withBridgedArrayRef { reqs in
        return action(params, reqs)
      }
    }
  }
}

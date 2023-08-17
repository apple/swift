import CASTBridging
import SwiftParser

// Needed to use SyntaxTransformVisitor's visit method.
@_spi(SyntaxTransformVisitor)
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: ClosureExprSyntax) -> ASTNode {
    let statements = node.statements.map { self.visit($0).bridged }
    let body: UnsafeMutableRawPointer = statements.withBridgedArrayRef { ref in
      let startLoc = bridgedSourceLoc(for: node.leftBrace)
      let endLoc = bridgedSourceLoc(for: node.rightBrace)
      return BraceStmt_create(ctx, startLoc, ref, endLoc)
    }

    return .expr(ClosureExpr_create(ctx, body, declContext))
  }

  public func visit(_ node: FunctionCallExprSyntax) -> ASTNode {
    // Transform the trailing closure into an argument.
    if let trailingClosure = node.trailingClosure {
      let tupleElement = LabeledExprSyntax(
        label: nil, colon: nil, expression: ExprSyntax(trailingClosure), trailingComma: nil)

      var node = node
      node.arguments.append(tupleElement)
      node.trailingClosure = nil
      return visit(node)
    }

    let args = visit(node.arguments).rawValue
    let callee = visit(node.calledExpression).rawValue

    return .expr(FunctionCallExpr_create(self.ctx, callee, args))
  }

  public func visit(_ node: DeclReferenceExprSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)

    var text = node.baseName.text
    let id = text.withBridgedString { bridgedText in
      return ASTContext_getIdentifier(ctx, bridgedText)
    }

    return .expr(IdentifierExpr_create(ctx, id, loc))
  }

  public func visit(_ node: IdentifierPatternSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)

    var text = node.identifier.text
    let id = text.withBridgedString { bridgedText in
      return ASTContext_getIdentifier(ctx, bridgedText)
    }

    return .expr(IdentifierExpr_create(ctx, id, loc))
  }

  public func visit(_ node: MemberAccessExprSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)
    let base = visit(node.base!).rawValue
    var nameText = node.declName.baseName.text
    let name = nameText.withBridgedString { bridgedName in
      return ASTContext_getIdentifier(ctx, bridgedName)
    }

    return .expr(UnresolvedDotExpr_create(ctx, base, loc, name, loc))
  }

  public func visit(_ node: IfExprSyntax) -> ASTNode {
    let stmt = makeIfStmt(node).rawValue

    // Wrap in a SingleValueStmtExpr to embed as an expression.
    let sve = SingleValueStmtExpr_createWithWrappedBranches(
      ctx, stmt, declContext, /*mustBeExpr*/ true)
    return .expr(sve)
  }

  public func visit(_ node: LabeledExprListSyntax) -> ASTNode {
    let elements = node.map { self.visit($0).rawValue }
    let labels: [BridgedIdentifier?] = node.map {
      guard var name = $0.label?.text else {
        return nil
      }
      return name.withBridgedString { bridgedName in
        ASTContext_getIdentifier(ctx, bridgedName)
      }
    }
    let labelLocs: [BridgedSourceLoc] = node.map {
      let pos = $0.label?.position ?? $0.position
      return bridgedSourceLoc(at: pos)
    }

    let lParenLoc = bridgedSourceLoc(for: node)
    let rParenLoc = bridgedSourceLoc(at: node.endPosition)

    return .expr(
      elements.withBridgedArrayRef { elementsRef in
        labels.withBridgedArrayRef { labelsRef in
          labelLocs.withBridgedArrayRef { labelLocRef in
            TupleExpr_create(self.ctx, lParenLoc, elementsRef, labelsRef,
                             labelLocRef, rParenLoc)
          }
        }
      })
  }
}

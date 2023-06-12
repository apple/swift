import CASTBridging
import SwiftParser
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: CodeBlockSyntax) -> ASTNode {
    let statements = node.statements.map { self.visit($0).bridged }
    let startLoc = bridgedSourceLoc(for: node.leftBrace)
    let endLoc = bridgedSourceLoc(for: node.rightBrace)

    return .stmt(
      statements.withBridgedArrayRef { ref in
        BraceStmt_create(ctx, startLoc, ref, endLoc)
      })
  }

  func makeIfStmt(_ node: IfExprSyntax) -> ASTNode {
    let conditions = node.conditions.map { self.visit($0).rawValue }
    assert(conditions.count == 1)  // TODO: handle multiple conditions.

    let body = visit(node.body).rawValue
    let loc = bridgedSourceLoc(for: node)

    if let elseBody = node.elseBody, node.elseKeyword != nil {
      return .stmt(IfStmt_create(ctx, loc, conditions.first!, body, loc,
                                 visit(elseBody).rawValue))
    }

    return .stmt(IfStmt_create(ctx, loc, conditions.first!, body, nil, nil))
  }

  public func visit(_ node: ExpressionStmtSyntax) -> ASTNode {
    switch Syntax(node.expression).as(SyntaxEnum.self) {
    case .ifExpr(let e):
      return makeIfStmt(e)
    default:
      fatalError("Unhandled case!")
    }
  }

  public func visit(_ node: ReturnStmtSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)

    let expr: ASTNode?
    if let expression = node.expression {
      expr = visit(expression)
    } else {
      expr = nil
    }

    return .stmt(ReturnStmt_create(ctx, loc, expr?.rawValue))
  }
}

import CASTBridging
import SwiftSyntax

extension ASTGenVisitor {
  public func generate(_ node: CodeBlockSyntax) -> ASTNode {
    .stmt(
      BraceStmt_create(
        self.ctx.bridged,
        node.leftBrace.bridgedSourceLoc(in: self),
        self.generate(node.statements),
        node.rightBrace.bridgedSourceLoc(in: self)
      )
    )
  }

  func makeIfStmt(_ node: IfExprSyntax) -> ASTNode {
    let conditions = node.conditions.map { self.generate($0).rawValue }
    assert(conditions.count == 1)  // TODO: handle multiple conditions.

    return .stmt(
      IfStmt_create(
        self.ctx.bridged,
        node.ifKeyword.bridgedSourceLoc(in: self),
        conditions.first!,
        self.generate(node.body).rawValue,
        node.elseKeyword.bridgedSourceLoc(in: self),
        self.generate(node.elseBody)?.rawValue
      )
    )
  }

  public func generate(_ node: ExpressionStmtSyntax) -> ASTNode {
    switch Syntax(node.expression).as(SyntaxEnum.self) {
    case .ifExpr(let e):
      return makeIfStmt(e)
    default:
      fatalError("Unhandled case!")
    }
  }

  public func generate(_ node: ReturnStmtSyntax) -> ASTNode {
    .stmt(
      ReturnStmt_create(
        self.ctx.bridged,
        node.returnKeyword.bridgedSourceLoc(in: self),
        self.generate(node.expression)?.rawValue
      )
    )
  }
}

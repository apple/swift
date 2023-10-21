import CASTBridging
import SwiftSyntax
import SwiftDiagnostics

extension ASTGenVisitor {
  public func generate(_ node: ClosureExprSyntax) -> ASTNode {
    let body = BraceStmt_create(
      self.ctx.bridged,
      node.leftBrace.bridgedSourceLoc(in: self),
      self.generate(node.statements),
      node.rightBrace.bridgedSourceLoc(in: self)
    )

    // FIXME: Translate the signature, capture list, 'in' location, etc.
    return .expr(ClosureExpr_create(self.ctx.bridged, body, self.declContext.bridged))
  }

  public func generate(_ node: FunctionCallExprSyntax) -> ASTNode {
    if !node.arguments.isEmpty || node.trailingClosure == nil {
      if node.leftParen == nil {
        self.diagnose(Diagnostic(node: node, message: MissingChildTokenError(parent: node, kindOfTokenMissing: .leftParen)))
      }
      if node.rightParen == nil {
        self.diagnose(Diagnostic(node: node, message: MissingChildTokenError(parent: node, kindOfTokenMissing: .rightParen)))
      }
    }

    var node = node

    // Transform the trailing closure into an argument.
    if let trailingClosure = node.trailingClosure {
      let tupleElement = LabeledExprSyntax(
        label: nil, colon: nil, expression: ExprSyntax(trailingClosure), trailingComma: nil)

      node.arguments.append(tupleElement)
      node.trailingClosure = nil
    }

    let argumentTuple = self.generate(node.arguments, leftParen: node.leftParen, rightParen: node.rightParen)
      .rawValue
    let callee = generate(node.calledExpression).rawValue

    return .expr(FunctionCallExpr_create(self.ctx.bridged, callee, argumentTuple))
  }

  public func generate(_ node: DeclReferenceExprSyntax) -> ASTNode {
    let (name, nameLoc) = node.baseName.bridgedIdentifierAndSourceLoc(in: self)

    return .expr(IdentifierExpr_create(self.ctx.bridged, name, nameLoc))
  }

  public func generate(_ node: IdentifierPatternSyntax) -> ASTNode {
    let (name, nameLoc) = node.identifier.bridgedIdentifierAndSourceLoc(in: self)

    return .expr(IdentifierExpr_create(self.ctx.bridged, name, nameLoc))
  }

  public func generate(_ node: MemberAccessExprSyntax) -> ASTNode {
    let loc = node.bridgedSourceLoc(in: self)
    let base = generate(node.base!).rawValue
    let name = node.declName.baseName.bridgedIdentifier(in: self)

    return .expr(UnresolvedDotExpr_create(ctx.bridged, base, loc, name, loc))
  }

  public func generate(_ node: IfExprSyntax) -> ASTNode {
    let stmt = makeIfStmt(node).rawValue

    // Wrap in a SingleValueStmtExpr to embed as an expression.
    let sve = SingleValueStmtExpr_createWithWrappedBranches(
      ctx.bridged, stmt, declContext.bridged, /*mustBeExpr*/ true)
    return .expr(sve)
  }

  public func generate(_ node: TupleExprSyntax) -> ASTNode {
    self.generate(node.elements, leftParen: node.leftParen, rightParen: node.rightParen)
  }
}

extension ASTGenVisitor {
  /// Generate a tuple expression from a ``LabeledExprListSyntax`` and parentheses.
  func generate(_ node: LabeledExprListSyntax, leftParen: TokenSyntax?, rightParen: TokenSyntax?) -> ASTNode {
    let expressions = node.lazy.map {
      self.generate($0.expression).rawValue
    }
    let labels = node.lazy.map {
      $0.label.bridgedIdentifier(in: self)
    }
    let labelLocations = node.lazy.map {
      if let label = $0.label {
        return label.bridgedSourceLoc(in: self)
      }

      return $0.bridgedSourceLoc(in: self)
    }

    return .expr(
      TupleExpr_create(
        self.ctx.bridged,
        leftParen.bridgedSourceLoc(in: self),
        expressions.bridgedArray(in: self),
        labels.bridgedArray(in: self),
        labelLocations.bridgedArray(in: self),
        rightParen.bridgedSourceLoc(in: self)
      )
    )
  }
}

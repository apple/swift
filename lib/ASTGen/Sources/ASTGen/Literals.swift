import CASTBridging
import SwiftSyntax

extension ASTGenVisitor {
  public func generate(_ node: StringLiteralExprSyntax) -> ASTNode {
    let openDelimiterOrQuoteLoc = (node.openingPounds ?? node.openingQuote).bridgedSourceLoc(in: self)

    // FIXME: Handle interpolated strings.
    var segment = node.segments.first!.as(StringSegmentSyntax.self)!.content.text
    return .expr(
      segment.withBridgedString { bridgedSegment in
        return StringLiteralExpr_create(self.ctx.bridged, bridgedSegment, openDelimiterOrQuoteLoc)
      }
    )
  }

  public func generate(_ node: IntegerLiteralExprSyntax) -> ASTNode {
    var segment = node.literal.text
    return .expr(
      segment.withBridgedString { bridgedSegment in
        return IntegerLiteralExpr_create(ctx.bridged, bridgedSegment, node.literal.bridgedSourceLoc(in: self))
      }
    )
  }

  public func generate(_ node: BooleanLiteralExprSyntax) -> ASTNode {
    let value = node.literal.tokenKind == .keyword(.true)
    return .expr(BooleanLiteralExpr_create(ctx.bridged, value, node.literal.bridgedSourceLoc(in: self)))
  }

  public func generate(_ node: ArrayExprSyntax) -> ASTNode {
    let expressions = node.elements.lazy.map {
      self.generate($0).rawValue
    }

    let commaLocations = node.elements.compactMap(in: self) {
      $0.trailingComma.bridgedSourceLoc(in: self)
    }

    return .expr(
      ArrayExpr_create(
        self.ctx.bridged,
        node.leftSquare.bridgedSourceLoc(in: self),
        expressions.bridgedArray(in: self),
        commaLocations,
        node.rightSquare.bridgedSourceLoc(in: self)
      )
    )
  }

  func generate(_ node: NilLiteralExprSyntax) -> ASTNode {
    .expr(NilLiteralExpr_create(astContext: self.ctx.bridged, nilKeywordLoc: node.nilKeyword.bridgedSourceLoc(in: self)))
  }
}

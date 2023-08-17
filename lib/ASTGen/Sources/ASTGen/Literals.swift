import CASTBridging
import SwiftParser
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: StringLiteralExprSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)
    var segment = node.segments.first!.as(StringSegmentSyntax.self)!.content.text
    return .expr(
      segment.withBridgedString { bridgedSegment in
        return StringLiteralExpr_create(ctx, bridgedSegment, loc)
      })
  }

  public func visit(_ node: IntegerLiteralExprSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)
    var segment = node.literal.text
    return .expr(
      segment.withBridgedString { bridgedSegment in
        return IntegerLiteralExpr_create(ctx, bridgedSegment, loc)
      })
  }

  public func visit(_ node: BooleanLiteralExprSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)
    let value = node.literal == .keyword(.true)
    return .expr(BooleanLiteralExpr_create(ctx, value, loc))
  }

  public func visit(_ node: ArrayExprSyntax) -> ASTNode {
    let lLoc = bridgedSourceLoc(for: node.leftSquare)
    let rLoc = bridgedSourceLoc(for: node.rightSquare)

    let elements = node.elements.map { self.visit($0).rawValue }
    let commas = node.elements
      .compactMap { $0.trailingComma }
      .map {
        bridgedSourceLoc(for: $0)
      }

    return elements.withBridgedArrayRef { elementsRef in
      commas.withBridgedArrayRef { commasRef in
        .expr(ArrayExpr_create(ctx, lLoc, elementsRef, commasRef, rLoc))
      }
    }
  }
}

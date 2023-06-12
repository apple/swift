import CASTBridging
import SwiftParser
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: MemberDeclListItemSyntax) -> ASTNode {
    visit(Syntax(node.decl))
  }

  public func visit(_ node: TupleExprElementSyntax) -> ASTNode {
    visit(node.expression)
  }

  public func visit(_ node: InitializerClauseSyntax) -> ASTNode {
    visit(node.value)
  }

  public func visit(_ node: ConditionElementSyntax) -> ASTNode {
    visit(node.condition)
  }

  public func visit(_ node: CodeBlockItemSyntax) -> ASTNode {
    visit(node.item)
  }

  public func visit(_ node: ArrayElementSyntax) -> ASTNode {
    visit(node.expression)
  }

  /// Form a source location at the given absolute position in the current
  /// file.
  public func bridgedSourceLoc(at position: AbsolutePosition) -> BridgedSourceLoc {
    return BridgedSourceLoc(at: position, in: base)
  }

  /// Form a source location at the given node's position in the current file.
  public func bridgedSourceLoc<Node: SyntaxProtocol>(for node: Node?) -> BridgedSourceLoc {
    guard let node = node else {
      return nil
    }
    return BridgedSourceLoc(at: node.position, in: base)
  }
}

extension BridgedSourceLoc: ExpressibleByNilLiteral {
  public init(nilLiteral: ()) {
    self.init(raw: nil)
  }
}

extension BridgedIdentifier: ExpressibleByNilLiteral {
  public init(nilLiteral: ()) {
    self.init(raw: nil)
  }
}

extension BridgedSourceLoc {
  /// Form a source location at the given absolute position in `buffer`.
  init(
    at position: AbsolutePosition,
    in buffer: UnsafeBufferPointer<UInt8>
  ) {
    if let start = buffer.baseAddress,
      position.utf8Offset >= 0 && position.utf8Offset < buffer.count {
      self = SourceLoc_advanced(BridgedSourceLoc(raw: start), position.utf8Offset)
    } else {
      self = nil
    }
  }
}

extension String {
  mutating func withBridgedString<R>(_ body: (BridgedString) throws -> R) rethrows -> R {
    try withUTF8 { buffer in
      try body(BridgedString(data: buffer.baseAddress, length: buffer.count))
    }
  }
}

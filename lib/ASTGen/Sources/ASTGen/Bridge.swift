import CASTBridging
import CBasicBridging
import SwiftSyntax

import ASTGenBridging

protocol BridgableArrayRefProtocol {
  associatedtype Element
  init(_ data: UnsafePointer<Element>?, _ length: Int)
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
    precondition(position.utf8Offset >= 0 && position.utf8Offset <= buffer.count)
    self = SourceLoc_advanced(BridgedSourceLoc(raw: buffer.baseAddress!), position.utf8Offset)
  }
}

extension BridgedSourceRange {
  @inline(__always)
  init(startToken: TokenSyntax, endToken: TokenSyntax, in astgen: ASTGenVisitor) {
    self.init(startLoc: startToken.bridgedSourceLoc(in: astgen), endLoc: endToken.bridgedSourceLoc(in: astgen))
  }
}

extension swift.SourceRange {
  @inline(__always)
  init(start: TokenSyntax, end: TokenSyntax, in astgen: ASTGenVisitor) {
    self.init(start.sourceLoc(in: astgen), end.sourceLoc(in: astgen))
  }
}

extension String {
  mutating func withBridgedString<R>(_ body: (BridgedString) throws -> R) rethrows -> R {
    try withUTF8 { buffer in
      try body(BridgedString(data: buffer.baseAddress, length: buffer.count))
    }
  }

  mutating func withStringRef<R>(_ body: (llvm.StringRef) throws -> R) rethrows -> R {
    try withUTF8 { buffer in
      try body(llvm.StringRef(buffer.baseAddress, buffer.count))
    }
  }
}

/// Allocate a copy of the given string as a null-terminated UTF-8 string.
func allocateBridgedString(
  _ string: String
) -> BridgedString {
  var string = string
  return string.withUTF8 { utf8 in
    let ptr = UnsafeMutablePointer<UInt8>.allocate(
      capacity: utf8.count + 1
    )
    if let baseAddress = utf8.baseAddress {
      ptr.initialize(from: baseAddress, count: utf8.count)
    }

    // null terminate, for client's convenience.
    ptr[utf8.count] = 0

    return BridgedString(data: ptr, length: utf8.count)
  }
}

@_cdecl("swift_ASTGen_freeBridgedString")
public func freeBridgedString(bridged: BridgedString) {
  bridged.data?.deallocate()
}

extension BridgedString {
  var isEmptyInitialized: Bool {
    return self.data == nil && self.length == 0
  }
}

extension AbsolutePosition {
  @inline(__always)
  func sourceLoc(in astgen: ASTGenVisitor) -> swift.SourceLoc {
    astgen.bufferStartLoc.getAdvancedLoc(Int32(self.utf8Offset))
  }
}

extension SyntaxProtocol {
  /// Obtains the bridged start location of the node excluding leading trivia in the source buffer provided by `astgen`
  ///
  /// - Parameter astgen: The visitor providing the source buffer.
  @inline(__always)
  func bridgedSourceLoc(in astgen: ASTGenVisitor) -> BridgedSourceLoc {
    return BridgedSourceLoc(at: self.positionAfterSkippingLeadingTrivia, in: astgen.base)
  }

  @inline(__always)
  func sourceLoc(in astgen: ASTGenVisitor) -> swift.SourceLoc {
    positionAfterSkippingLeadingTrivia.sourceLoc(in: astgen)
  }
}

extension Optional where Wrapped: SyntaxProtocol {
  /// Obtains the bridged start location of the node excluding leading trivia in the source buffer provided by `astgen`.
  ///
  /// - Parameter astgen: The visitor providing the source buffer.
  @inline(__always)
  func bridgedSourceLoc(in astgen: ASTGenVisitor) -> BridgedSourceLoc {
    guard let self else {
      return nil
    }
    
    return self.bridgedSourceLoc(in: astgen)
  }

  @inline(__always)
  func sourceLoc(in astgen: ASTGenVisitor) -> swift.SourceLoc {
    guard let self else {
      return swift.SourceLoc()
    }
    return self.sourceLoc(in: astgen)
  }
}

extension TokenSyntax {
  /// Obtains a bridged, `ASTContext`-owned copy of this token's text.
  ///
  /// - Parameter astgen: The visitor providing the `ASTContext`.
  @inline(__always)
  func bridgedIdentifier(in astgen: ASTGenVisitor) -> BridgedIdentifier {
    var text = self.text
    return text.withBridgedString { bridged in
      ASTContext_getIdentifier(astgen.ctx.bridged, bridged)
    }
  }

  @inline(__always)
  func identifier(in astgen: ASTGenVisitor) -> swift.Identifier {
    var text = self.text
    return text.withStringRef { stringRef in
      astgen.ctx.getIdentifier(stringRef)
    }
  }

  /// Obtains a bridged, `ASTContext`-owned copy of this token's text, and its bridged start location in the
  /// source buffer provided by `astgen`.
  ///
  /// - Parameter astgen: The visitor providing the `ASTContext` and source buffer.
  @inline(__always)
  func bridgedIdentifierAndSourceLoc(in astgen: ASTGenVisitor) -> (BridgedIdentifier, BridgedSourceLoc) {
    return (self.bridgedIdentifier(in: astgen), self.bridgedSourceLoc(in: astgen))
  }

  @inline(__always)
  func identifierAndSourceLoc(in astgen: ASTGenVisitor) -> (swift.Identifier, swift.SourceLoc) {
    (self.identifier(in: astgen), self.sourceLoc(in: astgen))
  }

  /// Obtains a bridged, `ASTContext`-owned copy of this token's text, and its bridged start location in the
  /// source buffer provided by `astgen`.
  ///
  /// - Parameter astgen: The visitor providing the `ASTContext` and source buffer.
  @inline(__always)
  func bridgedIdentifierAndSourceLoc(in astgen: ASTGenVisitor) -> BridgedIdentifierAndSourceLoc {
    let (name, nameLoc) = self.bridgedIdentifierAndSourceLoc(in: astgen)
    return .init(name: name, nameLoc: nameLoc)
  }
}

extension Optional<TokenSyntax> {
  /// Obtains a bridged, `ASTContext`-owned copy of this token's text.
  ///
  /// - Parameter astgen: The visitor providing the `ASTContext`.
  @inline(__always)
  func bridgedIdentifier(in astgen: ASTGenVisitor) -> BridgedIdentifier {
    guard let self else {
      return nil
    }

    return self.bridgedIdentifier(in: astgen)
  }

  @inline(__always)
  func identifier(in astgen: ASTGenVisitor) -> swift.Identifier {
    guard let self else {
      return .init()
    }

    return self.identifier(in: astgen)
  }

  /// Obtains a bridged, `ASTContext`-owned copy of this token's text, and its bridged start location in the
  /// source buffer provided by `astgen` excluding leading trivia.
  ///
  /// - Parameter astgen: The visitor providing the `ASTContext` and source buffer.
  @inline(__always)
  func bridgedIdentifierAndSourceLoc(in astgen: ASTGenVisitor) -> (BridgedIdentifier, BridgedSourceLoc) {
    guard let self else {
      return (nil, nil)
    }

    return self.bridgedIdentifierAndSourceLoc(in: astgen)
  }

  @inline(__always)
  func identifierAndSourceLoc(in astgen: ASTGenVisitor) -> (swift.Identifier, swift.SourceLoc) {
    guard let self else {
      return (.init(), .init())
    }
    return self.identifierAndSourceLoc(in: astgen)
  }
}

extension swift.BridgableASTContext {
  var bridged: BridgedASTContext { .init(raw: .init(self.Ctx)) }
}
extension UnsafeMutablePointer where Pointee == swift.DeclContext {
  var bridged: BridgedDeclContext { .init(raw: self) }
}

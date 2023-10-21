import CASTBridging
import CBasicBridging
import ASTGenBridging

// Needed to use BumpPtrAllocator
@_spi(BumpPtrAllocator)
import SwiftSyntax
import struct SwiftDiagnostics.Diagnostic

extension UnsafePointer {
  public var raw: UnsafeMutableRawPointer {
    UnsafeMutableRawPointer(mutating: self)
  }
}

enum ASTNode {
  case decl(UnsafeMutablePointer<swift.Decl>)
  case stmt(UnsafeMutableRawPointer)
  case expr(UnsafeMutableRawPointer)
  case type(UnsafeMutableRawPointer)
  case misc(UnsafeMutableRawPointer)

  var rawValue: UnsafeMutableRawPointer {
    switch self {
    case .decl(let ptr):
      return UnsafeMutableRawPointer(ptr)
    case .stmt(let ptr):
      return ptr
    case .expr(let ptr):
      return ptr
    case .type(let ptr):
      return ptr
    case .misc(let ptr):
      return ptr
    }
  }

  var expr: UnsafeMutablePointer<swift.Expr> {
    guard case .expr(let expr) = self else {
      fatalError("Expected expr")
    }
    return expr.assumingMemoryBound(to: swift.Expr.self)
  }

  var bridged: BridgedASTNode {
    switch self {
    case .expr(let e):
      return BridgedASTNode(ptr: e, kind: .expr)
    case .stmt(let s):
      return BridgedASTNode(ptr: s, kind: .stmt)
    case .decl(let d):
      return BridgedASTNode(ptr: d, kind: .decl)
    default:
      fatalError("Must be expr, stmt, or decl.")
    }
  }
}

/// Little utility wrapper that lets us have some mutable state within
/// immutable structs, and is therefore pretty evil.
@propertyWrapper
class Boxed<Value> {
  var wrappedValue: Value

  init(wrappedValue: Value) {
    self.wrappedValue = wrappedValue
  }
}

struct ASTGenVisitor {
  typealias ResultType = ASTNode

  fileprivate let diagnosticEngine: BridgedDiagnosticEngine

  let base: UnsafeBufferPointer<UInt8>

  @Boxed private(set) var declContext: UnsafeMutablePointer<swift.DeclContext>

  let ctx: swift.BridgableASTContext

  fileprivate let allocator: SwiftSyntax.BumpPtrAllocator = .init(slabSize: 256)

  init(
    diagnosticEngine: BridgedDiagnosticEngine,
    sourceBuffer: UnsafeBufferPointer<UInt8>,
    declContext: UnsafeMutablePointer<swift.DeclContext>,
    astContext: swift.BridgableASTContext
  ) {
    self.diagnosticEngine = diagnosticEngine
    self.base = sourceBuffer
    self.declContext = declContext
    self.ctx = astContext
  }

  public func generate(_ node: SourceFileSyntax) -> [UnsafeMutableRawPointer] {
    var out = [UnsafeMutableRawPointer]()

    for element in node.statements {
      let loc = element.bridgedSourceLoc(in: self)
      let swiftASTNodes = generate(element)
      switch swiftASTNodes {
      case .decl(let d):
        out.append(d)
      case .stmt(let s):
        out.append(TopLevelCodeDecl_createStmt(astContext: self.ctx.bridged, declContext: self.declContext.bridged, startLoc: loc, statement: s, endLoc: loc))
      case .expr(let e):
        out.append(TopLevelCodeDecl_createExpr(astContext: self.ctx.bridged, declContext: self.declContext.bridged, startLoc: loc, expression: e, endLoc: loc))
      default:
        fatalError("Top level nodes must be decls, stmts, or exprs.")
      }
    }

    return out
  }
}

extension ASTGenVisitor {
  var bufferStartLoc: swift.SourceLoc {
    .fromPointer(base.baseAddress!)
  }
}

extension ASTGenVisitor {
  /// Replaces the current declaration context with `declContext` for the duration of its execution, and calls `body`.
  @inline(__always)
  func withDeclContext(_ declContext: BridgedDeclContext, _ body: () -> Void) {
    withDeclContext(declContext.raw.assumingMemoryBound(to: swift.DeclContext.self), body)
  }

  /// Replaces the current declaration context with `declContext` for the duration of its execution, and calls `body`.
  @inline(__always)
  func withDeclContext(_ declContext: UnsafeMutablePointer<swift.DeclContext>, _ body: () -> Void) {
    let oldDeclContext = self.declContext
    self.declContext = declContext
    body()
    self.declContext = oldDeclContext
  }
}

extension ASTGenVisitor {
  /// Emits the given diagnostic via the C++ diagnostic engine.
  @inline(__always)
  func diagnose(_ diagnostic: Diagnostic) {
    emitDiagnostic(
      diagnosticEngine: self.diagnosticEngine,
      sourceFileBuffer: self.base,
      diagnostic: diagnostic,
      diagnosticSeverity: diagnostic.diagMessage.severity
    )
  }
}

extension ASTGenVisitor {
  func generate(_ node: DeclSyntax) -> ASTNode {
    return generate(Syntax(node))
  }

  func generate(_ node: ExprSyntax) -> ASTNode {
    return generate(Syntax(node))
  }

  func generate(_ node: PatternSyntax) -> ASTNode {
    return generate(Syntax(node))
  }

  func generate(_ node: StmtSyntax) -> ASTNode {
    return generate(Syntax(node))
  }

  func generate(_ node: TypeSyntax) -> ASTNode {
    return generate(Syntax(node))
  }

  func generate(_ node: some SyntaxChildChoices) -> ASTNode {
    return self.generate(Syntax(node))
  }
    
  func generate(_ node: Syntax) -> ASTNode {
    switch node.as(SyntaxEnum.self) {
    case .actorDecl(let node):
      return generate(node)
    case .arrayElement(let node):
      return generate(node)
    case .arrayExpr(let node):
      return generate(node)
    case .arrayType(let node):
      return generate(node)
    case .associatedTypeDecl(let node):
      return generate(node)
    case .attributedType(let node):
      return generate(node)
    case .booleanLiteralExpr(let node):
      return generate(node)
    case .classDecl(let node):
      return generate(node)
    case .closureExpr(let node):
      return generate(node)
    case .codeBlock(let node):
      return generate(node)
    case .codeBlockItem(let node):
      return generate(node)
    case .compositionType(let node):
      return generate(node)
    case .conditionElement(let node):
      return generate(node)
    case .declReferenceExpr(let node):
      return generate(node)
    case .deinitializerDecl(let node):
      return generate(node)
    case .dictionaryType(let node):
      return generate(node)
    case .enumCaseDecl(let node):
      return generate(node)
    case .enumCaseElement(let node):
      return .decl(generate(node).pointee.asDecl())
    case .enumDecl(let node):
      return generate(node)
    case .expressionStmt(let node):
     return generate(node)
    case .extensionDecl(let node):
      return generate(node)
    case .functionCallExpr(let node):
      return generate(node)
    case .functionDecl(let node):
      return generate(node)
    case .functionType(let node):
      return generate(node)
    case .identifierPattern(let node):
      return generate(node)
    case .identifierType(let node):
      return generate(node)
    case .ifExpr(let node):
      return generate(node)
    case .implicitlyUnwrappedOptionalType(let node):
      return generate(node)
    case .importDecl(let node):
      return generate(node)
    case .initializerClause(let node):
      return generate(node)
    case .initializerDecl(let node):
      return generate(node)
    case .integerLiteralExpr(let node):
      return generate(node)
    case .labeledExprList:
      fatalError("case does not correspond to an ASTNode")
    case .memberAccessExpr(let node):
      return generate(node)
    case .memberBlockItem(let node):
      return generate(node)
    case .memberType(let node):
      return generate(node)
    case .metatypeType(let node):
      return generate(node)
    case .namedOpaqueReturnType(let node):
      return generate(node)
    case .nilLiteralExpr(let node):
      return generate(node)
    case .operatorDecl(let node):
      return generate(node)
    case .optionalType(let node):
      return generate(node)
    case .packExpansionType(let node):
      return generate(node)
    case .precedenceGroupDecl(let node):
      return generate(node)
    case .protocolDecl(let node):
      return generate(node)
    case .returnStmt(let node):
      return generate(node)
    case .someOrAnyType(let node):
      return generate(node)
    case .stringLiteralExpr(let node):
      return generate(node)
    case .structDecl(let node):
      return generate(node)
    case .tupleExpr(let node):
      return generate(node)
    case .tupleType(let node):
      return generate(node)
    case .typeAliasDecl(let node):
      return generate(node)
    case .variableDecl(let node):
      return generate(node)
    default:
      fatalError("not implemented")
    }
  }
}

// Misc visits.
// TODO: Some of these are called within a single file/method; we may want to move them to the respective files.
extension ASTGenVisitor {
  public func generate(_ node: MemberBlockItemSyntax) -> ASTNode {
    generate(node.decl)
  }

  public func generate(_ node: InitializerClauseSyntax) -> ASTNode {
    generate(node.value)
  }

  public func generate(_ node: ConditionElementSyntax) -> ASTNode {
    generate(node.condition)
  }

  public func generate(_ node: CodeBlockItemSyntax) -> ASTNode {
    generate(node.item)
  }

  public func generate(_ node: ArrayElementSyntax) -> ASTNode {
    generate(node.expression)
  }

  @inline(__always)
  func generate(_ node: CodeBlockItemListSyntax) -> BridgedArrayRef {
    node.lazy.map { self.generate($0).bridged }.bridgedArray(in: self)
  }
}

// Forwarding overloads that take optional syntax nodes. These are defined on demand to achieve a consistent
// 'self.visit(<expr>)' recursion pattern between optional and non-optional inputs.
extension ASTGenVisitor {
  @inline(__always)
  func _generate(_ node: TypeSyntax?) -> ASTNode? {
    guard let node else {
      return nil
    }

    return self.generate(node)
  }

  @inline(__always)
  func generate(_ node: TypeSyntax?) -> UnsafeMutablePointer<swift.TypeRepr>? {
    self._generate(node)?.rawValue.assumingMemoryBound(to: swift.TypeRepr.self)
  }

  @inline(__always)
  func generate(_ node: ExprSyntax?) -> ASTNode? {
    guard let node else {
      return nil
    }

    return self.generate(node)
  }

  @inline(__always)
  func generate(_ node: (some SyntaxChildChoices)?) -> ASTNode? {
    guard let node else {
      return nil
    }

    // This call recurses without disambiguation.
    return self.generate(node) as ASTNode
  }

  @inline(__always)
  func _generate(_ node: GenericParameterClauseSyntax?) -> ASTNode? {
    guard let node else { return nil }
    return .misc(UnsafeMutableRawPointer(generate(node).Ptr!))
  }

  @inline(__always)
  func generate(_ node: GenericParameterClauseSyntax?) -> swift.BridgableGenericParamList {
    guard let node else { return .init(nil) }
    return self.generate(node)
  }

  @inline(__always)
  func _generate(_ node: GenericWhereClauseSyntax?) -> ASTNode? {
    guard let node else { return nil }
    return .misc(UnsafeMutableRawPointer(generate(node).Ptr!))
  }

  @inline(__always)
  func generate(_ node: GenericWhereClauseSyntax?) -> swift.BridgableTrailingWhereClause {
    guard let node else { return .init(nil) }
    return self.generate(node)
  }

  @inline(__always)
  func generate(_ node: EnumCaseParameterClauseSyntax?) -> swift.BridgableParameterList {
    guard let node else { return .init(nil) }
    return self.generate(node)
  }

  @inline(__always)
  func _generate(_ node: EnumCaseParameterClauseSyntax?) -> ASTNode? {
    guard let node else { return nil }
    return .misc(UnsafeMutableRawPointer(generate(node).Ptr!))
  }

  @inline(__always)
  func generate(_ node: InheritedTypeListSyntax?) -> BridgedArrayRef {
    guard let node else {
      return .init(data: nil, numElements: 0)
    }

    return self.generate(node)
  }

  @inline(__always)
  func generate(_ node: PrecedenceGroupNameListSyntax?) -> BridgedArrayRef {
    guard let node else {
      return .init(data: nil, numElements: 0)
    }

    return self.generate(node)
  }
}

extension Collection {
  /// Like ``Sequence.compactMap(_:)``, but returns a `BridgedArrayRef` with a lifetime tied to that of `astgen`.
  ///
  /// - Note: The purpose of this method is to make up for the performance toll of calling ``Collection.bridgedArray``
  ///   on a ``LazyFilterSequence`` due to the `count` access.
  func compactMap<T>(in astgen: ASTGenVisitor, _ transform: (Element) -> T?) -> BridgedArrayRef {
    if self.isEmpty {
      return .init(data: nil, numElements: 0)
    }

    let baseAddress = astgen.allocator.allocate(T.self, count: self.count).baseAddress!
    do {
      // A loop instead of `initialize(from: self.lazy.compactMap(transform))` because we aren't
      // doing a great job optimizing the latter.
      var currentAddress = baseAddress
      for element in self {
        guard let transformed = transform(element) else {
          continue
        }

        currentAddress.initialize(to: transformed)
        currentAddress += 1
      }
    }

    return .init(data: baseAddress, numElements: self.count)
  }
}

extension LazyCollectionProtocol {
  /// Returns a copy of the collection's elements as a `BridgedArrayRef` with a lifetime tied to that of `astgen`.
  func bridgedArray(in astgen: ASTGenVisitor) -> BridgedArrayRef {
    if self.isEmpty {
      return .init(data: nil, numElements: 0)
    }

    let buffer = astgen.allocator.allocate(Element.self, count: self.count)
    _ = buffer.initialize(from: self)

    return .init(data: buffer.baseAddress, numElements: self.count)
  }
}

extension BridgableArrayRefProtocol {
  init<C: Collection>(from collection: C?, in astgen: ASTGenVisitor) where C.Element == Element {
    guard let collection = collection, !collection.isEmpty else {
      self.init(nil, 0)
      return
    }

    let buffer = astgen.allocator.allocate(Element.self, count: collection.count)
    _ = buffer.initialize(from: collection)

    self.init(buffer.baseAddress, collection.count)
  }
}

// 'ReversedCollection' does not conform to 'LazyCollectionProtocol', and cannot here because it only
// conditionally conforms to 'LazySequenceProtocol' in the standard library.
// FIXME: We could make it conform unconditionally
extension ReversedCollection {
  /// Returns a copy of the collection's elements as a `BridgedArrayRef` with a lifetime tied to that of `astgen`.
  @inline(__always)
  func bridgedArray(in astgen: ASTGenVisitor) -> BridgedArrayRef {
    self.lazy.bridgedArray(in: astgen)
  }
}

extension Optional where Wrapped: LazyCollectionProtocol {
  /// Returns a copy of the collection's elements as a `BridgedArrayRef` with a lifetime tied to that of `astgen`.
  @inline(__always)
  func bridgedArray(in astgen: ASTGenVisitor) -> BridgedArrayRef {
    guard let self else {
      return .init(data: nil, numElements: 0)
    }

    return self.bridgedArray(in: astgen)
  }
}

/// Generate AST nodes for all top-level entities in the given source file.
@_cdecl("swift_ASTGen_buildTopLevelASTNodes")
public func buildTopLevelASTNodes(
  diagEnginePtr: UnsafeMutablePointer<UInt8>,
  sourceFilePtr: UnsafePointer<UInt8>,
  dc: UnsafeMutableRawPointer,
  ctx: UnsafeMutableRawPointer,
  outputContext: UnsafeMutableRawPointer,
  callback: @convention(c) (UnsafeMutableRawPointer, UnsafeMutableRawPointer) -> Void
) {
  sourceFilePtr.withMemoryRebound(to: ExportedSourceFile.self, capacity: 1) { sourceFile in
    ASTGenVisitor(
      diagnosticEngine: .init(raw: diagEnginePtr),
      sourceBuffer: sourceFile.pointee.buffer,
      declContext: dc.assumingMemoryBound(to: swift.DeclContext.self),
      astContext: .init(OpaquePointer(ctx))
    )
    .generate(sourceFile.pointee.syntax)
    .forEach { callback($0, outputContext) }
  }
}

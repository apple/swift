import CASTBridging

@_spi(ExperimentalLanguageFeatures)
import SwiftSyntax
import SwiftDiagnostics

// MARK: - TypeDecl

extension ASTGenVisitor {
  public func generate(_ node: TypeAliasDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.identifierAndSourceLoc(in: self)
    return .decl(
      swift.TypeAliasDecl.createParsed(
        self.ctx,
        keywordLoc: node.typealiasKeyword.sourceLoc(in: self),
        name: name,
        nameLoc: nameLoc,
        equalLoc: node.initializer.equal.sourceLoc(in: self),
        underlyingType: self.generate(node.initializer.value),
        genericParams: self.generate(node.genericParameterClause),
        whereClause: self.generate(node.genericWhereClause),
        declContext: self.declContext
      ).pointee.asDecl()
    )
  }

  public func generate(_ node: EnumDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.identifierAndSourceLoc(in: self)

    let enumDecl = swift.EnumDecl.createParsed(
      self.ctx,
      keywordLoc: node.enumKeyword.sourceLoc(in: self),
      name: name,
      nameLoc: nameLoc,
      genericParams: self.generate(node.genericParameterClause),
      inherited: .init(from: node.inheritanceClause?.inheritedTypes.lazy.map(self.generate), in: self),
      whereClause: self.generate(node.genericWhereClause),
      declContext: self.declContext
    )
    enumDecl.pointee.setBraces(.init(start: node.memberBlock.leftBrace, end: node.memberBlock.rightBrace, in: self))
    let decl = enumDecl.pointee.asDecl()

    self.withDeclContext(decl.pointee.getInnermostDeclContext()) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl)
    }

    return .decl(decl)
  }

  public func generate(_ node: StructDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)

    let decl = StructDecl_create(
      astContext: self.ctx.bridged,
      declContext: self.declContext.bridged,
      structKeywordLoc: node.structKeyword.bridgedSourceLoc(in: self),
      name: name,
      nameLoc: nameLoc,
      genericParamList: self._generate(node.genericParameterClause)?.rawValue,
      inheritedTypes: self.generate(node.inheritanceClause?.inheritedTypes),
      genericWhereClause: self._generate(node.genericWhereClause)?.rawValue,
      braceRange: BridgedSourceRange(startToken: node.memberBlock.leftBrace, endToken: node.memberBlock.rightBrace, in: self)
    )

    self.withDeclContext(decl.asDeclContext) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl.asDecl)
    }

    return .decl(decl.asDecl.assumingMemoryBound(to: swift.Decl.self))
  }

  public func generate(_ node: ClassDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)

    let decl = ClassDecl_create(
      astContext: self.ctx.bridged,
      declContext: self.declContext.bridged,
      classKeywordLoc: node.classKeyword.bridgedSourceLoc(in: self),
      name: name,
      nameLoc: nameLoc,
      genericParamList: self._generate(node.genericParameterClause)?.rawValue,
      inheritedTypes: self.generate(node.inheritanceClause?.inheritedTypes),
      genericWhereClause: self._generate(node.genericWhereClause)?.rawValue,
      braceRange: BridgedSourceRange(startToken: node.memberBlock.leftBrace, endToken: node.memberBlock.rightBrace, in: self),
      isActor: false
    )

    self.withDeclContext(decl.asDeclContext) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl.asDecl)
    }

    return .decl(decl.asDecl.assumingMemoryBound(to: swift.Decl.self))
  }

  public func generate(_ node: ActorDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)

    let decl = ClassDecl_create(
      astContext: self.ctx.bridged,
      declContext: self.declContext.bridged,
      classKeywordLoc: node.actorKeyword.bridgedSourceLoc(in: self),
      name: name,
      nameLoc: nameLoc,
      genericParamList: self._generate(node.genericParameterClause)?.rawValue,
      inheritedTypes: self.generate(node.inheritanceClause?.inheritedTypes),
      genericWhereClause: self._generate(node.genericWhereClause)?.rawValue,
      braceRange: BridgedSourceRange(startToken: node.memberBlock.leftBrace, endToken: node.memberBlock.rightBrace, in: self),
      isActor: true
    )

    self.withDeclContext(decl.asDeclContext) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl.asDecl)
    }

    return .decl(decl.asDecl.assumingMemoryBound(to: swift.Decl.self))
  }

  func generate(_ node: ProtocolDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)
    let primaryAssociatedTypeNames = node.primaryAssociatedTypeClause?.primaryAssociatedTypes.lazy.map {
      $0.name.bridgedIdentifierAndSourceLoc(in: self) as BridgedIdentifierAndSourceLoc
    }

    let decl = ProtocolDecl_create(
      astContext: self.ctx.bridged,
      declContext: self.declContext.bridged,
      protocolKeywordLoc: node.protocolKeyword.bridgedSourceLoc(in: self),
      name: name,
      nameLoc: nameLoc,
      primaryAssociatedTypeNames: primaryAssociatedTypeNames.bridgedArray(in: self),
      inheritedTypes: self.generate(node.inheritanceClause?.inheritedTypes),
      genericWhereClause: self._generate(node.genericWhereClause)?.rawValue,
      braceRange: BridgedSourceRange(startToken: node.memberBlock.leftBrace, endToken: node.memberBlock.rightBrace, in: self)
    )

    self.withDeclContext(decl.asDeclContext) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl.asDecl)
    }

    return .decl(decl.asDecl.assumingMemoryBound(to: swift.Decl.self))
  }

  func generate(_ node: AssociatedTypeDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.identifierAndSourceLoc(in: self)

    return .decl(
      swift.AssociatedTypeDecl.createParsed(
        self.ctx,
        keywordLoc: node.associatedtypeKeyword.sourceLoc(in: self),
        name: name,
        nameLoc: nameLoc,
        inherited: .init(from: node.inheritanceClause?.inheritedTypes.lazy.map(self.generate), in: self),
        defaultType: self.generate(node.initializer?.value),
        whereClause: self.generate(node.genericWhereClause),
        declContext: self.declContext
      ).pointee.asDecl()
    )
  }
}

// MARK: - ExtensionDecl

extension ASTGenVisitor {
  func generate(_ node: ExtensionDeclSyntax) -> ASTNode {
    let extDecl = swift.ExtensionDecl.createParsed(
      self.ctx,
      extensionKeywordLoc: node.extensionKeyword.sourceLoc(in: self),
      type: self.generate(node.extendedType),
      inherited: .init(from: node.inheritanceClause?.inheritedTypes.lazy.map(self.generate), in: self),
      whereClause: self.generate(node.genericWhereClause),
      declContext: self.declContext
    )
    extDecl.pointee.setBraces(.init(start: node.memberBlock.leftBrace, end: node.memberBlock.rightBrace, in: self))
    let decl = extDecl.pointee.asDecl()

    self.withDeclContext(decl.pointee.getInnermostDeclContext()) {
      IterableDeclContext_setParsedMembers(self.generate(node.memberBlock.members), ofDecl: decl)
    }
    return .decl(decl)
  }
}

// MARK: - EnumCaseDecl

extension ASTGenVisitor {
  func generate(_ node: EnumCaseElementSyntax) -> UnsafeMutablePointer<swift.EnumElementDecl> {
    let (baseName, nameLoc) = node.name.identifierAndSourceLoc(in: self)
    let params = self.generate(node.parameterClause)
    let name = params.isNull() ? swift.DeclName(baseName) 
                               : swift.DeclName(ctx, base: .init(baseName), params: params)
    return swift.EnumElementDecl.createParsed(
      self.ctx,
      nameLoc: nameLoc,
      name: name,
      params: params,
      equalsLoc: (node.rawValue?.equal).sourceLoc(in: self),
      // FIXME: Validation
      rawValue: self.generate(node.rawValue?.value)?.rawValue.assumingMemoryBound(to: swift.LiteralExpr.self),
      declContext: self.declContext
    )
  }

  func generate(_ node: EnumCaseDeclSyntax) -> ASTNode {
    .decl(
      swift.BridgableEnumCaseDecl.createParsed(
        keywordLoc: node.caseKeyword.sourceLoc(in: self),
        elements: .init(from: node.elements.lazy.map(self.generate), in: self),
        declContext: self.declContext
      ).asDecl()
    )
  }
}

// MARK: - AbstractStorageDecl

extension ASTGenVisitor {
  public func generate(_ node: VariableDeclSyntax) -> ASTNode {
    let pattern = generate(node.bindings.first!.pattern).rawValue
    let initializer = generate(node.bindings.first!.initializer!).rawValue

    let isStatic = false  // TODO: compute this
    let isLet = node.bindingSpecifier.tokenKind == .keyword(.let)

    return .decl(
      VarDecl_create(
        astContext: self.ctx.bridged,
        declContext: self.declContext.bridged,
        bindingKeywordLoc: node.bindingSpecifier.bridgedSourceLoc(in: self),
        nameExpr: pattern,
        initializer: initializer,
        isStatic: isStatic,
        isLet: isLet
      ).assumingMemoryBound(to: swift.Decl.self)
    )
  }
}

// MARK: - AbstractFunctionDecl

extension ASTGenVisitor {
  public func generate(_ node: FunctionDeclSyntax) -> ASTNode {
    // FIXME: Compute this location
    let staticLoc: swift.SourceLoc = .init()

    let (name, nameLoc) = node.name.identifierAndSourceLoc(in: self)
    let params = self.generate(node.signature.parameterClause)
    let decl = swift.BridgableFuncDecl.createParsed(
      self.ctx,
      staticLoc: staticLoc,
      staticSpelling: /*FIXME*/ swift.StaticSpellingKind.None,
      funcKeywordLoc: node.funcKeyword.sourceLoc(in: self),
      name: .init(ctx, base: .init(name), params: params),
      nameLoc: nameLoc,
      genericParams: self.generate(node.genericParameterClause),
      params: params,
      asyncLoc: (node.signature.effectSpecifiers?.asyncSpecifier).sourceLoc(in: self),
      throwsLoc: (node.signature.effectSpecifiers?.throwsSpecifier).sourceLoc(in: self),
      throwsType: self.generate(node.signature.effectSpecifiers?.thrownError?.type),
      resultType: self.generate(node.signature.returnClause?.type),
      whereClause: self.generate(node.genericWhereClause),
      declContext: self.declContext
    ).asDecl()

    if let body = node.body {
      self.withDeclContext(decl.pointee.getInnermostDeclContext()) {
        AbstractFunctionDecl_setBody(self.generate(body).rawValue, ofDecl: decl)
      }
    }
    
    return .decl(decl)
  }

  func generate(_ node: InitializerDeclSyntax) -> ASTNode {
    let params = generate(node.signature.parameterClause)

    let decl = swift.ConstructorDecl.createParsed(
      ctx,
      name: .init(ctx, base: .createConstructor(), params: params),
      constructorLoc: node.initKeyword.sourceLoc(in: self),
      failabilityLoc: node.optionalMark.sourceLoc(in: self),
      isIUO: node.optionalMark?.tokenKind == .exclamationMark,
      asyncLoc: (node.signature.effectSpecifiers?.asyncSpecifier).sourceLoc(in: self),
      throwsLoc: (node.signature.effectSpecifiers?.throwsSpecifier).sourceLoc(in: self),
      thrownType: self.generate(node.signature.effectSpecifiers?.thrownError?.type),
      bodyParams: params,
      genericParams: self.generate(node.genericParameterClause),
      whereClause: self.generate(node.genericWhereClause),
      declContext: declContext
    ).pointee.asDecl()

    if let body = node.body {
      self.withDeclContext(decl.pointee.getInnermostDeclContext()) {
        AbstractFunctionDecl_setBody(self.generate(body).rawValue, ofDecl: decl)
      }
    }
    
    return .decl(decl)
  }

  func generate(_ node: DeinitializerDeclSyntax) -> ASTNode {
    let decl = swift.DestructorDecl.createParsed(
      ctx, 
      destructorLoc: node.deinitKeyword.sourceLoc(in: self),
      declContext: declContext
    ).pointee.asDecl()

    if let body = node.body {
      self.withDeclContext(decl.pointee.getInnermostDeclContext()) {
        AbstractFunctionDecl_setBody(self.generate(body).rawValue, ofDecl: decl)
      }
    }

    return .decl(decl)
  }
}

// MARK: - OperatorDecl

extension BridgedOperatorFixity {
  fileprivate init?(from tokenKind: TokenKind) {
    switch tokenKind {
    case .keyword(.infix): self = .infix
    case .keyword(.prefix): self = .prefix
    case .keyword(.postfix): self = .postfix
    default: return nil
    }
  }
}

extension ASTGenVisitor {
  func generate(_ node: OperatorDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)
    let (precedenceGroupName, precedenceGroupLoc) = (node.operatorPrecedenceAndTypes?.precedenceGroup).bridgedIdentifierAndSourceLoc(in: self)

    let fixity: BridgedOperatorFixity
    if let value = BridgedOperatorFixity(from: node.fixitySpecifier.tokenKind) {
      fixity = value
    } else {
      fixity = .infix
      self.diagnose(Diagnostic(node: node.fixitySpecifier, message: UnexpectedTokenKindError(token: node.fixitySpecifier)))
    }

    return .decl(
      OperatorDecl_create(
        astContext: self.ctx.bridged,
        declContext: self.declContext.bridged,
        fixity: fixity,
        operatorKeywordLoc: node.operatorKeyword.bridgedSourceLoc(in: self),
        name: name,
        nameLoc: nameLoc,
        colonLoc: (node.operatorPrecedenceAndTypes?.colon).bridgedSourceLoc(in: self),
        precedenceGroupName: precedenceGroupName,
        PrecedenceGroupLoc: precedenceGroupLoc
      ).assumingMemoryBound(to: swift.Decl.self)
    )
  }
}

// MARK: - PrecedenceGroupDecl

extension BridgedAssociativity {
  fileprivate init?(from tokenKind: TokenKind) {
    switch tokenKind {
    case .keyword(.none): self = .none
    case .keyword(.left): self = .left
    case .keyword(.right): self = .right
    default: return nil
    }
  }
}

extension ASTGenVisitor {
  func generate(_ node: PrecedenceGroupDeclSyntax) -> ASTNode {
    let (name, nameLoc) = node.name.bridgedIdentifierAndSourceLoc(in: self)

    struct PrecedenceGroupBody {
      var associativity: PrecedenceGroupAssociativitySyntax? = nil
      var assignment: PrecedenceGroupAssignmentSyntax? = nil
      var higherThanRelation: PrecedenceGroupRelationSyntax? = nil
      var lowerThanRelation: PrecedenceGroupRelationSyntax? = nil
    }

    func diagnoseDuplicateSyntax(_ duplicate: some SyntaxProtocol, original: some SyntaxProtocol) {
      self.diagnose(Diagnostic(node: duplicate, message: DuplicateSyntaxError(duplicate: duplicate, original: original)))
    }

    let body = node.groupAttributes.reduce(into: PrecedenceGroupBody()) { body, element in
      switch element {
      case .precedenceGroupRelation(let relation):
        let keyword = relation.higherThanOrLowerThanLabel
        switch keyword.tokenKind {
        case .keyword(.higherThan):
          if let current = body.higherThanRelation {
            diagnoseDuplicateSyntax(relation, original: current)
          } else {
            body.higherThanRelation = relation
          }
        case .keyword(.lowerThan):
          if let current = body.lowerThanRelation {
            diagnoseDuplicateSyntax(relation, original: current)
          } else {
            body.lowerThanRelation = relation
          }
        default:
          return self.diagnose(Diagnostic(node: keyword, message: UnexpectedTokenKindError(token: keyword)))
        }
      case .precedenceGroupAssignment(let assignment):
        if let current = body.assignment {
          diagnoseDuplicateSyntax(assignment, original: current)
        } else {
          body.assignment = assignment
        }
      case .precedenceGroupAssociativity(let associativity):
        if let current = body.associativity {
          diagnoseDuplicateSyntax(node, original: current)
        } else {
          body.associativity = associativity
        }
      }
    }

    let associativityValue: BridgedAssociativity
    if let token = body.associativity?.value {
      if let value = BridgedAssociativity(from: token.tokenKind) {
        associativityValue = value
      } else {
        self.diagnose(Diagnostic(node: token, message: UnexpectedTokenKindError(token: token)))
        associativityValue = .none
      }
    } else {
      associativityValue = .none
    }

    let assignmentValue: Bool
    if let token = body.assignment?.value {
      if token.tokenKind == .keyword(.true) {
        assignmentValue = true
      } else {
        self.diagnose(Diagnostic(node: token, message: UnexpectedTokenKindError(token: token)))
        assignmentValue = false
      }
    } else {
      assignmentValue = false
    }

    return .decl(
      PrecedenceGroupDecl_create(
        declContext: self.declContext.bridged,
        precedencegroupKeywordLoc: node.precedencegroupKeyword.bridgedSourceLoc(in: self),
        name: name,
        nameLoc: nameLoc,
        leftBraceLoc: node.leftBrace.bridgedSourceLoc(in: self),
        associativityLabelLoc: (body.associativity?.associativityLabel).bridgedSourceLoc(in: self),
        associativityValueLoc: (body.associativity?.value).bridgedSourceLoc(in: self),
        associativity: associativityValue,
        assignmentLabelLoc: (body.assignment?.assignmentLabel).bridgedSourceLoc(in: self),
        assignmentValueLoc: (body.assignment?.value).bridgedSourceLoc(in: self),
        isAssignment: assignmentValue,
        higherThanKeywordLoc: (body.higherThanRelation?.higherThanOrLowerThanLabel).bridgedSourceLoc(in: self),
        higherThanNames: self.generate(body.higherThanRelation?.precedenceGroups),
        lowerThanKeywordLoc: (body.lowerThanRelation?.higherThanOrLowerThanLabel).bridgedSourceLoc(in: self),
        lowerThanNames: self.generate(body.lowerThanRelation?.precedenceGroups),
        rightBraceLoc: node.rightBrace.bridgedSourceLoc(in: self)
      ).assumingMemoryBound(to: swift.Decl.self)
    )
  }
}

// MARK: - ImportDecl

extension BridgedImportKind {
  fileprivate init?(from tokenKind: TokenKind) {
    switch tokenKind {
    case .keyword(.typealias): self = .type
    case .keyword(.struct): self = .struct
    case .keyword(.class): self = .class
    case .keyword(.enum): self = .enum
    case .keyword(.protocol): self = .protocol
    case .keyword(.var), .keyword(.let): self = .var
    case .keyword(.func): self = .func
    default: return nil
    }
  }
}

extension ASTGenVisitor {
  func generate(_ node: ImportDeclSyntax) -> ASTNode {
    let importKind: BridgedImportKind
    if let specifier = node.importKindSpecifier {
      if let value = BridgedImportKind(from: specifier.tokenKind) {
        importKind = value
      } else {
        self.diagnose(Diagnostic(node: specifier, message: UnexpectedTokenKindError(token: specifier)))
        importKind = .module
      }
    } else {
      importKind = .module
    }

    return .decl(
      ImportDecl_create(
        astContext: self.ctx.bridged,
        declContext: self.declContext.bridged,
        importKeywordLoc: node.importKeyword.bridgedSourceLoc(in: self),
        importKind: importKind,
        importKindLoc: node.importKindSpecifier.bridgedSourceLoc(in: self),
        path: node.path.lazy.map {
          $0.name.bridgedIdentifierAndSourceLoc(in: self) as BridgedIdentifierAndSourceLoc
        }.bridgedArray(in: self)
      ).assumingMemoryBound(to: swift.Decl.self)
    )
  }
}

extension ASTGenVisitor {
  @inline(__always)
  func generate(_ node: MemberBlockItemListSyntax) -> BridgedArrayRef {
    node.lazy.map { self.generate($0).rawValue }.bridgedArray(in: self)
  }

  @inline(__always)
  func generate(_ node: InheritedTypeListSyntax) -> BridgedArrayRef {
    node.lazy.map { self.generate($0.type).rawValue }.bridgedArray(in: self)
  }

  @inline(__always)
  func generate(_ node: InheritedTypeSyntax) -> swift.InheritedEntry {
    .init(.init(self.generate(node.type)))
  }

  @inline(__always)
  func generate(_ node: PrecedenceGroupNameListSyntax) -> BridgedArrayRef {
    node.lazy.map {
      $0.name.bridgedIdentifierAndSourceLoc(in: self) as BridgedIdentifierAndSourceLoc
    }.bridgedArray(in: self)
  }
}

import CASTBridging
import SwiftParser
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: SimpleTypeIdentifierSyntax) -> ASTNode {
    let loc = self.base.advanced(by: node.position.utf8Offset).raw

    // If this is the bare 'Any' keyword, produce an empty composition type.
    if node.name.tokenKind == .keyword(.Any) && node.genericArgumentClause == nil {
      return .type(EmptyCompositionTypeRepr_create(self.ctx, loc))
    }

    var text = node.name.text
    let id = text.withUTF8 { buf in
      return SwiftASTContext_getIdentifier(ctx, buf.baseAddress, buf.count)
    }

    guard let generics = node.genericArgumentClause else {
      return .type(SimpleIdentTypeRepr_create(ctx, loc, id))
    }

    let lAngle = self.base.advanced(by: generics.leftAngleBracket.position.utf8Offset).raw
    let rAngle = self.base.advanced(by: generics.rightAngleBracket.position.utf8Offset).raw
    return .type(
      generics.arguments.map({
        self.visit($0.argumentType).rawValue
      }).withBridgedArrayRef {
          genericArgs in
          GenericIdentTypeRepr_create(
            self.ctx, id, loc, genericArgs, lAngle, rAngle)
      })
  }

  public func visit(_ node: MemberTypeIdentifierSyntax) -> ASTNode {
    // Gather the member components, in decreasing depth order.
    var reverseMemberComponents = [UnsafeMutableRawPointer]()

    var baseType = Syntax(node)
    while let memberType = baseType.as(MemberTypeIdentifierSyntax.self) {
      let nameToken = memberType.name
      let generics = memberType.genericArgumentClause

      var nameText = nameToken.text
      let name = nameText.withUTF8 { buf in
        return SwiftASTContext_getIdentifier(ctx, buf.baseAddress, buf.count)
      }
      let nameLoc = self.base.advanced(by: nameToken.position.utf8Offset).raw

      if let generics = generics {
        let lAngle = self.base.advanced(by: generics.leftAngleBracket.position.utf8Offset).raw
        let rAngle = self.base.advanced(by: generics.rightAngleBracket.position.utf8Offset).raw
        reverseMemberComponents.append(
          generics.arguments.map({ self.visit($0.argumentType).rawValue }).withBridgedArrayRef {
            genericArgs in
            GenericIdentTypeRepr_create(self.ctx, name, nameLoc, genericArgs, lAngle, rAngle)
          })
      } else {
        reverseMemberComponents.append(SimpleIdentTypeRepr_create(self.ctx, nameLoc, name))
      }

      baseType = Syntax(memberType.baseType)
    }

    let baseComponent = visit(baseType).rawValue

    return .type(
      reverseMemberComponents.reversed().withBridgedArrayRef { memberComponents in
        return MemberTypeRepr_create(self.ctx, baseComponent, memberComponents)
      })
  }

  public func visit(_ node: ArrayTypeSyntax) -> ASTNode {
    let elementType = visit(node.elementType).rawValue
    let lSquareLoc = self.base.advanced(by: node.leftSquareBracket.position.utf8Offset).raw
    let rSquareLoc = self.base.advanced(by: node.rightSquareBracket.position.utf8Offset).raw
    return .type(ArrayTypeRepr_create(self.ctx, elementType, lSquareLoc, rSquareLoc))
  }

  public func visit(_ node: DictionaryTypeSyntax) -> ASTNode {
    let keyType = visit(node.keyType).rawValue
    let valueType = visit(node.valueType).rawValue
    let colonLoc = self.base.advanced(by: node.colon.position.utf8Offset).raw
    let lSquareLoc = self.base.advanced(by: node.leftSquareBracket.position.utf8Offset).raw
    let rSquareLoc = self.base.advanced(by: node.rightSquareBracket.position.utf8Offset).raw
    return .type(
      DictionaryTypeRepr_create(self.ctx, keyType, valueType, colonLoc, lSquareLoc, rSquareLoc))
  }

  public func visit(_ node: MetatypeTypeSyntax) -> ASTNode {
    let baseType = visit(node.baseType).rawValue
    let tyLoc = self.base.advanced(by: node.typeOrProtocol.position.utf8Offset).raw
    if node.typeOrProtocol.text == "Type" {
      return .type(MetatypeTypeRepr_create(self.ctx, baseType, tyLoc))
    } else {
      assert(node.typeOrProtocol.text == "Protocol")
      return .type(ProtocolTypeRepr_create(self.ctx, baseType, tyLoc))
    }
  }

  public func visit(_ node: ImplicitlyUnwrappedOptionalTypeSyntax) -> ASTNode {
    let base = visit(node.wrappedType).rawValue
    let exclaimLoc = self.base.advanced(by: node.exclamationMark.position.utf8Offset).raw
    return .type(ImplicitlyUnwrappedOptionalTypeRepr_create(self.ctx, base, exclaimLoc))
  }

  public func visit(_ node: OptionalTypeSyntax) -> ASTNode {
    let base = visit(node.wrappedType).rawValue
    let questionLoc = self.base.advanced(by: node.questionMark.position.utf8Offset).raw
    return .type(OptionalTypeRepr_create(self.ctx, base, questionLoc))
  }

  public func visit(_ node: PackExpansionTypeSyntax) -> ASTNode {
    let base = visit(node.patternType).rawValue
    let repeatLoc = self.base.advanced(by: node.repeatKeyword.position.utf8Offset).raw
    return .type(PackExpansionTypeRepr_create(self.ctx, base, repeatLoc))
  }

  public func visit(_ node: TupleTypeSyntax) -> ASTNode {
    return self.withBridgedTupleElements(node.elements) { elements in
      let lParenLoc = self.base.advanced(by: node.leftParen.position.utf8Offset).raw
      let rParenLoc = self.base.advanced(by: node.rightParen.position.utf8Offset).raw
      return .type(TupleTypeRepr_create(self.ctx, elements, lParenLoc, rParenLoc))
    }
  }

  public func visit(_ node: CompositionTypeSyntax) -> ASTNode {
    assert(node.elements.count > 1)
    let types = node.elements.map { visit($0.type) }.map { $0.rawValue }
    let firstTypeLoc = self.base.advanced(by: node.elements.first!.type.position.utf8Offset).raw
    let firstAmpOffset = node.elements.first?.ampersand.map { $0.position.utf8Offset } ?? 0
    let firstAmpLoc = self.base.advanced(by: firstAmpOffset).raw
    return .type(
      types.withBridgedArrayRef { types in
        return CompositionTypeRepr_create(self.ctx, types, firstTypeLoc, firstAmpLoc)
      })
  }

  public func visit(_ node: FunctionTypeSyntax) -> ASTNode {
    return self.withBridgedTupleElements(node.arguments) { elements in
      let lParenLoc = self.base.advanced(by: node.leftParen.position.utf8Offset).raw
      let rParenLoc = self.base.advanced(by: node.rightParen.position.utf8Offset).raw
      let args = TupleTypeRepr_create(self.ctx, elements, lParenLoc, rParenLoc)
      let asyncLoc = node.effectSpecifiers?.asyncSpecifier.map { self.base.advanced(by: $0.position.utf8Offset).raw }
      let throwsLoc = node.effectSpecifiers?.throwsSpecifier.map {
        self.base.advanced(by: $0.position.utf8Offset).raw
      }
      let arrowLoc = self.base.advanced(by: node.output.arrow.position.utf8Offset).raw
      let retTy = visit(node.output.returnType).rawValue
      return .type(FunctionTypeRepr_create(self.ctx, args, asyncLoc, throwsLoc, arrowLoc, retTy))
    }
  }

  public func visit(_ node: NamedOpaqueReturnTypeSyntax) -> ASTNode {
    let baseTy = visit(node.baseType).rawValue
    return .type(NamedOpaqueReturnTypeRepr_create(self.ctx, baseTy))
  }

  public func visit(_ node: ConstrainedSugarTypeSyntax) -> ASTNode {
    let someOrAnyLoc = self.base.advanced(by: node.someOrAnySpecifier.position.utf8Offset).raw
    let baseTy = visit(node.baseType).rawValue
    if node.someOrAnySpecifier.text == "some" {
      return .type(OpaqueReturnTypeRepr_create(self.ctx, someOrAnyLoc, baseTy))
    } else {
      assert(node.someOrAnySpecifier.text == "any")
      return .type(ExistentialTypeRepr_create(self.ctx, someOrAnyLoc, baseTy))
    }
  }

  public func visit(_ node: AttributedTypeSyntax) -> ASTNode {
    var type = visit(node.baseType)

    // Handle specifiers.
    if let specifier = node.specifier {
      let specifierLoc = self.base.advanced(by: specifier.position.utf8Offset).raw

      let kind: BridgedAttributedTypeSpecifier
      switch specifier.tokenKind {
        case .keyword(.inout): kind = .inOut
        case .keyword(.borrowing): kind = .borrowing
        case .keyword(.consuming): kind = .consuming
        case .keyword(.__shared): kind = .legacyShared
        case .keyword(.__owned): kind = .legacyOwned
        case .keyword(._const): kind = .const
        case .keyword(.isolated): kind = .isolated
        default: fatalError("unhandled specifier \(specifier.debugDescription)")
      }

      type = .type(AttributedTypeSpecifierRepr_create(self.ctx, type.rawValue, kind, specifierLoc))
    }

    // Handle type attributes.
    if let attributes = node.attributes {
      let typeAttributes = BridgedTypeAttributes_create()
      for attributeElt in attributes {
        // FIXME: Ignoring #ifs entirely. We want to provide a filtered view,
        // but we don't have that ability right now.
        guard case let .attribute(attribute) = attributeElt else {
          continue
        }

        // Only handle simple attribute names right now.
        guard let identType = attribute.attributeName.as(SimpleTypeIdentifierSyntax.self) else {
          continue
        }

        let nameSyntax = identType.name
        var name = nameSyntax.text
        let typeAttrKind = name.withUTF8 { buf in
          getBridgedTypeAttrKindFromString(buf.baseAddress, buf.count)
        }
        let atLoc = self.base.advanced(by: attribute.atSignToken.position.utf8Offset).raw
        let attrLoc = self.base.advanced(by: nameSyntax.position.utf8Offset).raw
        switch typeAttrKind {
          // SIL attributes
          // FIXME: Diagnose if not in SIL mode? Or should that move to the
          // type checker?
          case .out, .in, .owned, .unowned_inner_pointer, .guaranteed,
               .autoreleased, .callee_owned, .callee_guaranteed, .objc_metatype,
               .sil_weak, .sil_unowned, .inout, .block_storage, .box,
               .dynamic_self, .sil_unmanaged, .error, .direct, .inout_aliasable,
               .in_guaranteed, .in_constant, .captures_generics, .moveOnly:
            fallthrough

          case .autoclosure, .escaping, .noescape, .noDerivative, .async,
            .sendable, .unchecked, ._local, ._noMetadata, .pack_owned,
            .pack_guaranteed, .pack_inout, .pack_out, .pseudogeneric,
            .yields, .yield_once, .yield_many, .thin, .thick, .count:
            BridgedTypeAttributes_addSimpleAttr(typeAttributes, typeAttrKind, atLoc, attrLoc)

          case .opened, .pack_element, .differentiable, .convention,
            ._opaqueReturnTypeOf:
            // FIXME: These require more complicated checks
            break
        }
      }

      type = .type(AttributedTypeRepr_create(self.ctx, type.rawValue, typeAttributes))
    }

    return type
  }
}

extension ASTGenVisitor {
  private func withBridgedTupleElements<T>(
    _ elementList: TupleTypeElementListSyntax,
    action: (BridgedArrayRef) -> T
  ) -> T {
    var elements = [BridgedTupleTypeElement]()
    for element in elementList {
      var nameText = element.name?.text
      let name = nameText?.withUTF8 { buf in
        return SwiftASTContext_getIdentifier(ctx, buf.baseAddress, buf.count)
      }
      let nameLoc = element.name.map { self.base.advanced(by: $0.position.utf8Offset).raw }
      var secondNameText = element.secondName?.text
      let secondName = secondNameText?.withUTF8 { buf in
        return SwiftASTContext_getIdentifier(ctx, buf.baseAddress, buf.count)
      }
      let secondNameLoc = element.secondName.map {
        self.base.advanced(by: $0.position.utf8Offset).raw
      }
      let colonLoc = element.colon.map { self.base.advanced(by: $0.position.utf8Offset).raw }

      var type = visit(element.type).rawValue
      if let ellipsis = element.ellipsis {
        let ellipsisLoc = self.base.advanced(by: ellipsis.positionAfterSkippingLeadingTrivia.utf8Offset).raw
        type = VarargTypeRepr_create(self.ctx, type, ellipsisLoc)
      }

      let trailingCommaLoc = element.trailingComma.map {
        self.base.advanced(by: $0.position.utf8Offset).raw
      }

      elements.append(
        BridgedTupleTypeElement(
          Name: name,
          NameLoc: nameLoc,
          SecondName: secondName,
          SecondNameLoc: secondNameLoc,
          UnderscoreLoc: nil, /*N.B. Only important for SIL*/
          ColonLoc: colonLoc,
          Type: type,
          TrailingCommaLoc: trailingCommaLoc))
    }
    return elements.withBridgedArrayRef { elements in
      return action(elements)
    }
  }
}

@_cdecl("swift_ASTGen_buildTypeRepr")
@usableFromInline
func buildTypeRepr(
  sourceFilePtr: UnsafeRawPointer,
  typeLocPtr: UnsafePointer<UInt8>,
  dc: UnsafeMutableRawPointer,
  ctx: UnsafeMutableRawPointer,
  endTypeLocPtr: UnsafeMutablePointer<UnsafePointer<UInt8>?>
) -> UnsafeMutableRawPointer? {
  let sourceFile = sourceFilePtr.bindMemory(
    to: ExportedSourceFile.self, capacity: 1
  )

  // Find the type syntax node.
  guard let typeSyntax = findSyntaxNodeInSourceFile(
    sourceFilePtr: sourceFilePtr,
    sourceLocationPtr: typeLocPtr,
    type: TypeSyntax.self,
    wantOutermost: true
  ) else {
    // FIXME: Produce an error
    return nil
  }

  // Fill in the end location.
  endTypeLocPtr.pointee = sourceFile.pointee.buffer.baseAddress!.advanced(by: typeSyntax.endPosition.utf8Offset)

  // Convert the type syntax node.
  let typeReprNode = ASTGenVisitor(ctx: ctx, base: sourceFile.pointee.buffer.baseAddress!, declContext: dc)
    .visit(typeSyntax)

  return typeReprNode.rawValue
}

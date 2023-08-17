import CASTBridging
import SwiftParser

// Needed to use SyntaxTransformVisitor's visit method.
@_spi(SyntaxTransformVisitor)
import SwiftSyntax

extension ASTGenVisitor {
  public func visit(_ node: IdentifierTypeSyntax) -> ASTNode {
    let loc = bridgedSourceLoc(for: node)

    // If this is the bare 'Any' keyword, produce an empty composition type.
    if node.name.tokenKind == .keyword(.Any) && node.genericArgumentClause == nil {
      return .type(EmptyCompositionTypeRepr_create(self.ctx, loc))
    }

    var text = node.name.text
    let id = text.withBridgedString { bridgedText in
      return ASTContext_getIdentifier(ctx, bridgedText)
    }

    guard let generics = node.genericArgumentClause else {
      return .type(SimpleIdentTypeRepr_create(ctx, loc, id))
    }

    let lAngle = bridgedSourceLoc(for: generics.leftAngle)
    let rAngle = bridgedSourceLoc(for: generics.rightAngle)
    return .type(
      generics.arguments.map({
        self.visit($0.argument).rawValue
      }).withBridgedArrayRef {
          genericArgs in
          GenericIdentTypeRepr_create(
            self.ctx, id, loc, genericArgs, lAngle, rAngle)
      })
  }

  public func visit(_ node: MemberTypeSyntax) -> ASTNode {
    // Gather the member components, in decreasing depth order.
    var reverseMemberComponents = [UnsafeMutableRawPointer]()

    var baseType = Syntax(node)
    while let memberType = baseType.as(MemberTypeSyntax.self) {
      let nameToken = memberType.name
      let generics = memberType.genericArgumentClause

      var nameText = nameToken.text
      let name = nameText.withBridgedString { bridgedName in
        return ASTContext_getIdentifier(ctx, bridgedName)
      }
      let nameLoc = bridgedSourceLoc(for: nameToken)

      if let generics = generics {
        let lAngle = bridgedSourceLoc(for: generics.leftAngle)
        let rAngle = bridgedSourceLoc(for: generics.rightAngle)
        reverseMemberComponents.append(
          generics.arguments.map({ self.visit($0.argument).rawValue }).withBridgedArrayRef {
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
    let elementType = visit(node.element).rawValue
    let lSquareLoc = bridgedSourceLoc(for: node.leftSquare)
    let rSquareLoc = bridgedSourceLoc(for: node.rightSquare)
    return .type(ArrayTypeRepr_create(self.ctx, elementType, lSquareLoc, rSquareLoc))
  }

  public func visit(_ node: DictionaryTypeSyntax) -> ASTNode {
    let keyType = visit(node.key).rawValue
    let valueType = visit(node.value).rawValue
    let colonLoc = bridgedSourceLoc(for: node.colon)
    let lSquareLoc = bridgedSourceLoc(for: node.leftSquare)
    let rSquareLoc = bridgedSourceLoc(for: node.rightSquare)
    return .type(
      DictionaryTypeRepr_create(self.ctx, keyType, valueType, colonLoc, lSquareLoc, rSquareLoc))
  }

  public func visit(_ node: MetatypeTypeSyntax) -> ASTNode {
    let baseType = visit(node.baseType).rawValue
    let tyLoc = bridgedSourceLoc(for: node.metatypeSpecifier)
    if node.metatypeSpecifier.text == "Type" {
      return .type(MetatypeTypeRepr_create(self.ctx, baseType, tyLoc))
    } else {
      assert(node.metatypeSpecifier.text == "Protocol")
      return .type(ProtocolTypeRepr_create(self.ctx, baseType, tyLoc))
    }
  }

  public func visit(_ node: ImplicitlyUnwrappedOptionalTypeSyntax) -> ASTNode {
    let base = visit(node.wrappedType).rawValue
    let exclaimLoc = bridgedSourceLoc(for: node.exclamationMark)
    return .type(ImplicitlyUnwrappedOptionalTypeRepr_create(self.ctx, base, exclaimLoc))
  }

  public func visit(_ node: OptionalTypeSyntax) -> ASTNode {
    let base = visit(node.wrappedType).rawValue
    let questionLoc = bridgedSourceLoc(for: node.questionMark)
    return .type(OptionalTypeRepr_create(self.ctx, base, questionLoc))
  }

  public func visit(_ node: PackExpansionTypeSyntax) -> ASTNode {
    let base = visit(node.repetitionPattern).rawValue
    let repeatLoc = bridgedSourceLoc(for: node.repeatKeyword)
    return .type(PackExpansionTypeRepr_create(self.ctx, base, repeatLoc))
  }

  public func visit(_ node: TupleTypeSyntax) -> ASTNode {
    return self.withBridgedTupleElements(node.elements) { elements in
      let lParenLoc = bridgedSourceLoc(for: node.leftParen)
      let rParenLoc = bridgedSourceLoc(for: node.rightParen)
      return .type(TupleTypeRepr_create(self.ctx, elements, lParenLoc, rParenLoc))
    }
  }

  public func visit(_ node: CompositionTypeSyntax) -> ASTNode {
    assert(node.elements.count > 1)
    let types = node.elements.map { visit($0.type) }.map { $0.rawValue }
    let firstTypeLoc = bridgedSourceLoc(for: node.elements.first?.type)
    let firstAmpLoc = bridgedSourceLoc(for: node.elements.first?.ampersand)
    return .type(
      types.withBridgedArrayRef { types in
        return CompositionTypeRepr_create(self.ctx, types, firstTypeLoc, firstAmpLoc)
      })
  }

  public func visit(_ node: FunctionTypeSyntax) -> ASTNode {
    return self.withBridgedTupleElements(node.parameters) { elements in
      let lParenLoc = bridgedSourceLoc(for: node.leftParen)
      let rParenLoc = bridgedSourceLoc(for: node.rightParen)
      let args = TupleTypeRepr_create(self.ctx, elements, lParenLoc, rParenLoc)
      let asyncLoc = bridgedSourceLoc(for: node.effectSpecifiers?.asyncSpecifier)
      let throwsLoc = bridgedSourceLoc(for: node.effectSpecifiers?.throwsSpecifier)
      let arrowLoc = bridgedSourceLoc(for: node.returnClause.arrow)
      let retTy = visit(node.returnClause.type).rawValue
      return .type(FunctionTypeRepr_create(self.ctx, args, asyncLoc, throwsLoc, arrowLoc, retTy))
    }
  }

  public func visit(_ node: NamedOpaqueReturnTypeSyntax) -> ASTNode {
    let baseTy = visit(node.type).rawValue
    return .type(NamedOpaqueReturnTypeRepr_create(self.ctx, baseTy))
  }

  public func visit(_ node: SomeOrAnyTypeSyntax) -> ASTNode {
    let someOrAnyLoc = bridgedSourceLoc(for: node.someOrAnySpecifier)
    let baseTy = visit(node.constraint).rawValue
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
      let specifierLoc = bridgedSourceLoc(for: specifier)

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
    if !node.attributes.isEmpty {
      let typeAttributes = TypeAttributes_create()
      for attributeElt in node.attributes {
        // FIXME: Ignoring #ifs entirely. We want to provide a filtered view,
        // but we don't have that ability right now.
        guard case let .attribute(attribute) = attributeElt else {
          continue
        }

        // Only handle simple attribute names right now.
        guard let identType = attribute.attributeName.as(IdentifierTypeSyntax.self) else {
          continue
        }

        let nameSyntax = identType.name
        var name = nameSyntax.text
        let typeAttrKind = name.withBridgedString { bridgedName in
          TypeAttrKind_fromString(bridgedName)
        }
        let atLoc = bridgedSourceLoc(for: attribute.atSign)
        let attrLoc = bridgedSourceLoc(for: nameSyntax)
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
            .yields, .yield_once, .yield_many, .thin, .thick, .count,
            .unimplementable:
            TypeAttributes_addSimpleAttr(typeAttributes, typeAttrKind, atLoc, attrLoc)

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
      var nameText = element.firstName?.text
      let name = nameText?.withBridgedString { bridgedName in
        return ASTContext_getIdentifier(ctx, bridgedName)
      } ?? nil
      let nameLoc = bridgedSourceLoc(for: element.firstName)
      var secondNameText = element.secondName?.text
      let secondName = secondNameText?.withBridgedString { bridgedName in
        return ASTContext_getIdentifier(ctx, bridgedName)
      } ?? nil
      let secondNameLoc = bridgedSourceLoc(for: element.secondName)
      let colonLoc = bridgedSourceLoc(for: element.colon)
      var type = visit(element.type).rawValue
      if let ellipsis = element.ellipsis {
        let ellipsisLoc = bridgedSourceLoc(at: ellipsis.positionAfterSkippingLeadingTrivia)
        type = VarargTypeRepr_create(self.ctx, type, ellipsisLoc)
      }
      let trailingCommaLoc = bridgedSourceLoc(for: element.trailingComma)

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
  let typeReprNode = ASTGenVisitor(ctx: BridgedASTContext(raw: ctx), base: sourceFile.pointee.buffer, declContext: BridgedDeclContext(raw: dc))
    .visit(typeSyntax)

  return typeReprNode.rawValue
}

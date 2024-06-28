//===--- Stmts.swift ------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022-2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import ASTBridging
import SwiftDiagnostics
@_spi(ExperimentalLanguageFeatures) import SwiftSyntax

extension ASTGenVisitor {
  func generate(stmt node: StmtSyntax) -> BridgedStmt {
    switch node.as(StmtSyntaxEnum.self) {
    case .breakStmt(let node):
      return self.generate(breakStmt: node).asStmt
    case .continueStmt(let node):
      return self.generate(continueStmt: node).asStmt
    case .deferStmt(let node):
      return self.generate(deferStmt: node).asStmt
    case .discardStmt(let node):
      return self.generate(discardStmt: node).asStmt
    case .doStmt(let node):
      return self.generate(doStmt: node)
    case .expressionStmt(let node):
      return self.generate(expressionStmt: node)
    case .fallThroughStmt(let node):
      return self.generate(fallThroughStmt: node).asStmt
    case .forStmt(let node):
      return self.generate(forStmt: node).asStmt
    case .guardStmt(let node):
      return self.generate(guardStmt: node).asStmt
    case .labeledStmt(let node):
      return self.generate(labeledStmt: node)
    case .missingStmt:
      break
    case .repeatStmt(let node):
      return self.generate(repeatStmt: node).asStmt
    case .returnStmt(let node):
      return self.generate(returnStmt: node).asStmt
    case .thenStmt(let node):
      return self.generate(thenStmt: node).asStmt
    case .throwStmt(let node):
      return self.generate(throwStmt: node).asStmt
    case .whileStmt(let node):
      return self.generate(whileStmt: node).asStmt
    case .yieldStmt(let node):
      return self.generate(yieldStmt: node).asStmt
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
    return self.generateWithLegacy(node)
  }

  func generate(codeBlockItem node: CodeBlockItemSyntax) -> ASTNode {
    // TODO: Set semicolon loc.
    switch node.item {
    case .decl(let node):
      return .decl(self.generate(decl: node))
    case .stmt(let node):
      return .stmt(self.generate(stmt: node))
    case .expr(let node):
      return .expr(self.generate(expr: node))
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
  }

  @inline(__always)
  func generate(codeBlockItemList node: CodeBlockItemListSyntax) -> BridgedArrayRef {
    node.lazy.map { self.generate(codeBlockItem: $0).bridged }.bridgedArray(in: self)
  }

  func generate(codeBlock node: CodeBlockSyntax) -> BridgedBraceStmt {
    BridgedBraceStmt.createParsed(
      self.ctx,
      lBraceLoc: self.generateSourceLoc(node.leftBrace),
      elements: self.generate(codeBlockItemList: node.statements),
      rBraceLoc: self.generateSourceLoc(node.rightBrace)
    )
  }

  func generate(conditionElement node: ConditionElementSyntax) -> BridgedStmtConditionElement {
    // FIXME: _hasSymbol is not implemented in SwiftSyntax/SwiftParser.
    switch node.condition {
    case .availability(_):
      fatalError("unimplemented")
      break
    case .expression(let node):
      return .createBoolean(
        expr: self.generate(expr: node)
      )
    case .matchingPattern(let node):
      return .createPatternBinding(
        self.ctx,
        introducerLoc: self.generateSourceLoc(node.caseKeyword),
        pattern: self.generate(pattern: node.pattern, typeAnnotation: node.typeAnnotation),
        initializer: self.generate(expr: node.initializer.value)
      )
    case .optionalBinding(let node):
      var pat = self.generate(pattern: node.pattern)
      let keywordLoc = self.generateSourceLoc(node.bindingSpecifier)
      let isLet = node.bindingSpecifier.keywordKind == .let
      pat =
        BridgedBindingPattern.createParsed(
          self.ctx,
          keywordLoc: keywordLoc,
          isLet: isLet,
          subPattern: pat
        ).asPattern

      // NOTE: (From the comment in libParse) The let/var pattern is part of the
      // statement. But since the statement doesn't have the information. But,
      // I'm not sure this should really be implicit.
      pat.setImplicit()

      let initializer: BridgedExpr
      if let initNode = node.initializer {
        initializer = self.generate(expr: initNode.value)
      } else {
        let identifier = pat.boundName
        if identifier != nil {
          // For `if let foo { }` Create a `foo` expression as the initializer.
          let ref = BridgedDeclNameRef.createParsed(.createIdentifier(identifier))
          let loc = BridgedDeclNameLoc.createParsed(self.generateSourceLoc(node.pattern))
          initializer =
            BridgedUnresolvedDeclRefExpr.createParsed(
              self.ctx,
              name: ref,
              kind: .ordinary,
              loc: loc
            ).asExpr
        } else {
          // FIXME: Implement.
          // For `if let foo.bar {`, diagnose and convert it to `if let _ =  foo.bar`
          // For `if let (a, b) {`, diagnose it and create an error expression.
          fatalError("unimplemented")
        }
      }
      return .createPatternBinding(
        self.ctx,
        introducerLoc: keywordLoc,
        pattern: pat,
        initializer: initializer
      )
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
  }

  func generate(conditionElementList node: ConditionElementListSyntax) -> BridgedArrayRef {
    node.lazy.map(generate(conditionElement:)).bridgedArray(in: self)
  }

  func generate(breakStmt node: BreakStmtSyntax) -> BridgedBreakStmt {
    let (targetName, targetLoc) = self.generateIdentifierAndSourceLoc(node.label)
    return .createParsed(
      self.declContext,
      loc: self.generateSourceLoc(node.breakKeyword),
      targetName: targetName,
      targetLoc: targetLoc
    )
  }

  func generate(continueStmt node: ContinueStmtSyntax) -> BridgedContinueStmt {
    let (targetName, targetLoc) = self.generateIdentifierAndSourceLoc(node.label)
    return .createParsed(
      self.declContext,
      loc: self.generateSourceLoc(node.continueKeyword),
      targetName: targetName,
      targetLoc: targetLoc
    )
  }

  func generate(deferStmt node: DeferStmtSyntax) -> BridgedDeferStmt {
    let deferLoc = self.generateSourceLoc(node.deferKeyword)
    let stmt = BridgedDeferStmt.createParsed(
      self.declContext,
      deferLoc: deferLoc
    )
    self.withDeclContext(stmt.tempDecl.asDeclContext) {
      stmt.tempDecl.setParsedBody(self.generate(codeBlock: node.body))
    }
    return stmt
  }

  func generate(discardStmt node: DiscardStmtSyntax) -> BridgedDiscardStmt {
    return .createParsed(
      self.ctx,
      discardLoc: self.generateSourceLoc(node.discardKeyword),
      subExpr: self.generate(expr: node.expression)
    )
  }

  func generate(catchItem node: CatchItemSyntax) -> BridgedCaseLabelItemInfo {
    let pattern: BridgedPattern
    if let nodePattern = node.pattern {
      pattern = self.generate(pattern: nodePattern)
    } else {
      pattern =
        BridgedBindingPattern.createImplicitCatch(
          self.declContext,
          loc: self.generateSourceLoc(node)
        ).asPattern
    }
    return .init(
      isDefault: false,
      pattern: pattern,
      whereLoc: self.generateSourceLoc(node.whereClause?.whereKeyword),
      guardExpr: self.generate(expr: node.whereClause?.condition)
    )
  }

  func generate(catchItemList node: CatchItemListSyntax) -> BridgedArrayRef {
    if node.isEmpty {
      let item = BridgedCaseLabelItemInfo(
        isDefault: false,
        pattern: BridgedBindingPattern.createImplicitCatch(
          self.declContext,
          loc: self.generateSourceLoc(node)
        ).asPattern,
        whereLoc: nil,
        guardExpr: nil
      )
      return CollectionOfOne(item).bridgedArray(in: self)
    } else {
      return node.lazy.map(self.generate(catchItem:)).bridgedArray(in: self)
    }
  }

  func generate(catchClause node: CatchClauseSyntax) -> BridgedCaseStmt {
    return .createParsedDoCatch(
      self.ctx,
      catchLoc: self.generateSourceLoc(node.catchKeyword),
      caseLabelItems: self.generate(catchItemList: node.catchItems),
      body: self.generate(codeBlock: node.body)
    )
  }

  func generate(catchClauseList node: CatchClauseListSyntax) -> BridgedArrayRef {
    node.lazy.map(self.generate(catchClause:)).bridgedArray(in: self)
  }

  func generate(doStmt node: DoStmtSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedStmt {
    if node.catchClauses.isEmpty {
      // FIXME: Handle
      precondition(node.throwsClause == nil)
      return BridgedDoStmt.createParsed(
        self.ctx,
        labelInfo: labelInfo,
        doLoc: self.generateSourceLoc(node.doKeyword),
        body: self.generate(codeBlock: node.body)
      ).asStmt
    } else {
      return BridgedDoCatchStmt.createParsed(
        self.declContext,
        labelInfo: labelInfo,
        doLoc: self.generateSourceLoc(node.doKeyword),
        throwsLoc: self.generateSourceLoc(node.throwsClause?.throwsSpecifier),
        thrownType: self.generate(type: node.throwsClause?.type),
        body: self.generate(codeBlock: node.body).asStmt,
        catches: self.generate(catchClauseList: node.catchClauses)
      ).asStmt
    }
  }

  func generate(forStmt node: ForStmtSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedForEachStmt {
    return .createParsed(
      self.ctx,
      labelInfo: labelInfo,
      forLoc: self.generateSourceLoc(node.forKeyword),
      tryLoc: self.generateSourceLoc(node.tryKeyword),
      awaitLoc: self.generateSourceLoc(node.awaitKeyword),
      // NOTE: The pattern can be either a refutable pattern after `case` or a
      // normal binding pattern. ASTGen doesn't care because it should be handled
      // by the parser.
      pattern: self.generate(pattern: node.pattern, typeAnnotation: node.typeAnnotation),
      inLoc: self.generateSourceLoc(node.inKeyword),
      sequence: self.generate(expr: node.sequence),
      whereLoc: self.generateSourceLoc(node.whereClause?.whereKeyword),
      whereExpr: self.generate(expr: node.whereClause?.condition),
      body: self.generate(codeBlock: node.body)
    )
  }

  func generate(guardStmt node: GuardStmtSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedGuardStmt {
    return .createParsed(
      self.ctx,
      guardLoc: self.generateSourceLoc(node.guardKeyword),
      conds: self.generate(conditionElementList: node.conditions),
      body: self.generate(codeBlock: node.body)
    )
  }

  func generateIfStmt(ifExpr node: IfExprSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedIfStmt {
    return .createParsed(
      self.ctx,
      labelInfo: labelInfo,
      ifLoc: self.generateSourceLoc(node.ifKeyword),
      conditions: self.generate(conditionElementList: node.conditions),
      then: self.generate(codeBlock: node.body),
      elseLoc: self.generateSourceLoc(node.elseKeyword),
      else: node.elseBody.map {
        switch $0 {
        case .codeBlock(let node):
          return self.generate(codeBlock: node).asStmt
        case .ifExpr(let node):
          return self.generateIfStmt(ifExpr: node).asStmt
#if RESILIENT_SWIFT_SYNTAX
        @unknown default:
          fatalError()
#endif
        }
      }.asNullable
    )
  }

  func generate(expressionStmt node: ExpressionStmtSyntax) -> BridgedStmt {
    switch node.expression.as(ExprSyntaxEnum.self) {
    case .ifExpr(let node):
      return self.generateIfStmt(ifExpr: node).asStmt
    case .switchExpr(let node):
      return self.generateSwitchStmt(switchExpr: node).asStmt
    default:
      fatalError("Unhandled case!")
    }
  }

  func generate(fallThroughStmt node: FallThroughStmtSyntax) -> BridgedFallthroughStmt {
    return .createParsed(
      self.ctx,
      loc: self.generateSourceLoc(node.fallthroughKeyword)
    )
  }

  func generate(labeledStmt node: LabeledStmtSyntax) -> BridgedStmt {
    let (labelName, labelLoc) = self.generateIdentifierAndSourceLoc(node.label)
    let labelInfo = BridgedLabeledStmtInfo(name: labelName, loc: labelLoc)

    switch node.statement.as(StmtSyntaxEnum.self) {
    case .doStmt(let node):
      return self.generate(doStmt: node, labelInfo: labelInfo)
    case .expressionStmt(let node):
      switch node.expression.as(ExprSyntaxEnum.self) {
      case .ifExpr(let node):
        return self.generateIfStmt(ifExpr: node, labelInfo: labelInfo).asStmt
      case .switchExpr(let node):
        return self.generateSwitchStmt(switchExpr: node, labelInfo: labelInfo).asStmt
      default:
        break
      }
    case .forStmt(let node):
      return self.generate(forStmt: node, labelInfo: labelInfo).asStmt
    case .guardStmt(let node):
      return self.generate(guardStmt: node, labelInfo: labelInfo).asStmt
    case .repeatStmt(let node):
      return self.generate(repeatStmt: node, labelInfo: labelInfo).asStmt
    case .whileStmt(let node):
      return self.generate(whileStmt: node, labelInfo: labelInfo).asStmt
    default:
      break
    }

    // Not a labeled statement.
    // Should be unreachable as long as the node is parsed by 'SwiftParser'.
    // FIXME: emit an error diagnostic.
    return self.generate(stmt: node.statement)
  }

  func generate(repeatStmt node: RepeatStmtSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedRepeatWhileStmt {
    return .createParsed(
      self.ctx,
      labelInfo: labelInfo,
      repeatLoc: self.generateSourceLoc(node.repeatKeyword),
      cond: self.generate(expr: node.condition),
      whileLoc: self.generateSourceLoc(node.whileKeyword),
      body: self.generate(codeBlock: node.body).asStmt
    )
  }

  func generate(returnStmt node: ReturnStmtSyntax) -> BridgedReturnStmt {
    return .createParsed(
      self.ctx,
      loc: self.generateSourceLoc(node.returnKeyword),
      expr: self.generate(expr: node.expression)
    )
  }

  func generate(thenStmt node: ThenStmtSyntax) -> BridgedThenStmt {
    return .createParsed(
      self.ctx,
      thenLoc: self.generateSourceLoc(node.thenKeyword),
      result: self.generate(expr: node.expression)
    )
  }

  func generate(switchCaseItem node: SwitchCaseItemSyntax) -> BridgedCaseLabelItemInfo {
    return .init(
      isDefault: false,
      pattern: self.generate(pattern: node.pattern),
      whereLoc: self.generateSourceLoc(node.whereClause?.whereKeyword),
      guardExpr: self.generate(expr: node.whereClause?.condition)
    )
  }

  func generate(switchCaseItemList node: SwitchCaseItemListSyntax) -> BridgedArrayRef {
    return node.lazy.map(self.generate(switchCaseItem:)).bridgedArray(in: self)
  }

  func generate(switchCase node: SwitchCaseSyntax) -> BridgedCaseStmt {
    let unknownAttrLoc = self.generateSourceLoc(node.attribute?.atSign)
    let introducerLoc: BridgedSourceLoc
    let terminatorLoc: BridgedSourceLoc
    let items: BridgedArrayRef
    switch node.label {
    case .case(let node):
      introducerLoc = self.generateSourceLoc(node.caseKeyword)
      terminatorLoc = self.generateSourceLoc(node.colon)
      items = self.generate(switchCaseItemList: node.caseItems)
    case .default(let node):
      introducerLoc = self.generateSourceLoc(node.defaultKeyword)
      let item = BridgedCaseLabelItemInfo(
        isDefault: true,
        pattern: BridgedAnyPattern.createParsed(self.ctx, loc: introducerLoc).asPattern,
        whereLoc: nil,
        guardExpr: nil
      )
      items = CollectionOfOne(item).bridgedArray(in: self)
      terminatorLoc = self.generateSourceLoc(node.colon)
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
    let body = BridgedBraceStmt.createParsed(
      self.ctx,
      lBraceLoc: nil,
      elements: self.generate(codeBlockItemList: node.statements),
      rBraceLoc: nil
    )

    return .createParsedSwitchCase(
      self.ctx,
      introducerLoc: introducerLoc,
      caseLabelItems: items,
      unknownAttrLoc: unknownAttrLoc,
      terminatorLoc: terminatorLoc,
      body: body
    )
  }

  func generate(switchCaseList node: SwitchCaseListSyntax) -> BridgedArrayRef {
    return node.lazy.map({ node -> BridgedASTNode in
      switch node {
      case .switchCase(let node):
        return ASTNode.stmt(self.generate(switchCase: node).asStmt).bridged
      case .ifConfigDecl(_):
        fatalError("unimplemented")
#if RESILIENT_SWIFT_SYNTAX
      @unknown default:
        fatalError()
#endif
      }
    }).bridgedArray(in: self)
  }

  func generateSwitchStmt(switchExpr node: SwitchExprSyntax, labelInfo: BridgedLabeledStmtInfo = nil)
    -> BridgedSwitchStmt
  {
    return .createParsed(
      self.ctx,
      labelInfo: labelInfo,
      switchLoc: self.generateSourceLoc(node.switchKeyword),
      subjectExpr: self.generate(expr: node.subject),
      lBraceLoc: self.generateSourceLoc(node.leftBrace),
      cases: self.generate(switchCaseList: node.cases),
      rBraceLoc: self.generateSourceLoc(node.rightBrace)
    )
  }

  func generate(throwStmt node: ThrowStmtSyntax) -> BridgedThrowStmt {
    return .createParsed(
      self.ctx,
      throwLoc: self.generateSourceLoc(node.throwKeyword),
      subExpr: self.generate(expr: node.expression)
    )
  }

  func generate(whileStmt node: WhileStmtSyntax, labelInfo: BridgedLabeledStmtInfo = nil) -> BridgedWhileStmt {
    return .createParsed(
      self.ctx,
      labelInfo: labelInfo,
      whileLoc: self.generateSourceLoc(node.whileKeyword),
      cond: self.generate(conditionElementList: node.conditions),
      body: self.generate(codeBlock: node.body).asStmt
    )
  }

  func generate(yieldStmt node: YieldStmtSyntax) -> BridgedYieldStmt {
    // FIXME: SwiftParser always parses `yield(...)` as an expression.
    // ASTGen needs to convert the call to an expression (in generate(codeBlockItem:)?)
    let lParenLoc: BridgedSourceLoc
    let rParenLoc: BridgedSourceLoc
    let yields: BridgedArrayRef
    switch node.yieldedExpressions {
    case .multiple(let node):
      lParenLoc = self.generateSourceLoc(node.leftParen)
      rParenLoc = self.generateSourceLoc(node.rightParen)
      yields = node.elements.lazy.map({
        self.generate(expr: $0.expression)
      }).bridgedArray(in: self)
    case .single(let node):
      lParenLoc = nil
      rParenLoc = nil
      yields = CollectionOfOne(self.generate(expr: node)).bridgedArray(in: self)
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
    return .createParsed(
      self.ctx,
      yieldLoc: self.generateSourceLoc(node.yieldKeyword),
      lParenLoc: lParenLoc,
      yields: yields,
      rParenLoc: rParenLoc
    )
  }
}

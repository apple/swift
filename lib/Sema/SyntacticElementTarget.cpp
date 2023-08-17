//===--- SyntacticElementTarget.cpp - Syntactic Element Target ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements SyntacticElementTarget
//
//===----------------------------------------------------------------------===//

#include "swift/Sema/SyntacticElementTarget.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/TypeRepr.h"
#include "TypeChecker.h"

using namespace swift;
using namespace constraints;

#define DEBUG_TYPE "SyntacticElementTarget"

SyntacticElementTarget::SyntacticElementTarget(
    Expr *expr, DeclContext *dc, ContextualTypeInfo contextualInfo,
    bool isDiscarded) {
  auto contextualPurpose = contextualInfo.purpose;

  // Verify that a purpose was specified if a convertType was.  Note that it is
  // ok to have a purpose without a convertType (which is used for call
  // return types).
  assert((!contextualInfo.getType() || contextualPurpose != CTP_Unused) &&
         "Purpose for conversion type was not specified");

  // Take a look at the conversion type to check to make sure it is sensible.
  if (auto type = contextualInfo.getType()) {
    // If we're asked to convert to an UnresolvedType, then ignore the request.
    // This happens when CSDiags nukes a type.
    if (type->is<UnresolvedType>() ||
        (type->is<MetatypeType>() && type->hasUnresolvedType())) {
      contextualInfo.typeLoc = TypeLoc();
      contextualPurpose = CTP_Unused;
    }
  }

  kind = Kind::expression;
  expression.expression = expr;
  expression.dc = dc;
  expression.contextualInfo = contextualInfo;
  expression.pattern = nullptr;
  expression.propertyWrapper.wrappedVar = nullptr;
  expression.propertyWrapper.innermostWrappedValueInit = nullptr;
  expression.propertyWrapper.hasInitialWrappedValue = false;
  expression.isDiscarded = isDiscarded;
  expression.bindPatternVarsOneWay = false;
  expression.initialization.patternBinding = nullptr;
  expression.initialization.patternBindingIndex = 0;
}

void SyntacticElementTarget::maybeApplyPropertyWrapper() {
  assert(getExprContextualTypePurpose() == CTP_Initialization);

  VarDecl *singleVar;
  if (auto *pattern = expression.pattern) {
    singleVar = pattern->getSingleVar();
  } else {
    singleVar = expression.propertyWrapper.wrappedVar;
  }

  if (!singleVar)
    return;

  auto wrapperAttrs = singleVar->getAttachedPropertyWrappers();
  if (wrapperAttrs.empty())
    return;

  // If the outermost property wrapper is directly initialized, form the
  // call.
  auto &ctx = singleVar->getASTContext();
  auto outermostWrapperAttr = wrapperAttrs.front();
  Expr *backingInitializer;
  if (Expr *initializer = expression.expression) {
    if (!isa<PropertyWrapperValuePlaceholderExpr>(initializer)) {
      expression.propertyWrapper.hasInitialWrappedValue = true;
    }
    // Form init(wrappedValue:) call(s).
    Expr *wrappedInitializer = buildPropertyWrapperInitCall(
        singleVar, Type(), initializer, PropertyWrapperInitKind::WrappedValue,
        [&](ApplyExpr *innermostInit) {
          expression.propertyWrapper.innermostWrappedValueInit = innermostInit;
        });
    if (!wrappedInitializer)
      return;

    backingInitializer = wrappedInitializer;
  } else {
    Type outermostWrapperType = singleVar->getAttachedPropertyWrapperType(0);
    if (!outermostWrapperType)
      return;

    bool isImplicit = false;

    // Retrieve the outermost wrapper argument. If there isn't one, we're
    // performing default initialization.
    auto outermostArgs = outermostWrapperAttr->getArgs();
    if (!outermostArgs) {
      SourceLoc fakeParenLoc = outermostWrapperAttr->getRange().End;
      outermostArgs =
          ArgumentList::createImplicit(ctx, fakeParenLoc, {}, fakeParenLoc);
      isImplicit = true;
    }

    SourceLoc typeLoc;
    if (auto *repr = outermostWrapperAttr->getTypeRepr()) {
      typeLoc = repr->getLoc();
    }
    auto typeExpr =
        TypeExpr::createImplicitHack(typeLoc, outermostWrapperType, ctx);
    backingInitializer = CallExpr::create(ctx, typeExpr, outermostArgs,
                                          /*implicit*/ isImplicit);
  }
  wrapperAttrs[0]->setSemanticInit(backingInitializer);

  // Note that we have applied to property wrapper, so we can adjust
  // the initializer type later.
  expression.propertyWrapper.wrappedVar = singleVar;
  expression.expression = backingInitializer;
  expression.contextualInfo.typeLoc = {outermostWrapperAttr->getTypeRepr(),
                                       outermostWrapperAttr->getType()};
}

SyntacticElementTarget
SyntacticElementTarget::forInitialization(Expr *initializer, DeclContext *dc,
                                          Type patternType, Pattern *pattern,
                                          bool bindPatternVarsOneWay) {
  // Determine the contextual type for the initialization.
  TypeLoc contextualType;
  if (!(isa<OptionalSomePattern>(pattern) && !pattern->isImplicit()) &&
      patternType && !patternType->is<UnresolvedType>()) {
    contextualType = TypeLoc::withoutLoc(patternType);

    // Only provide a TypeLoc if it makes sense to allow diagnostics.
    if (auto *typedPattern = dyn_cast<TypedPattern>(pattern)) {
      const Pattern *inner = typedPattern->getSemanticsProvidingPattern();
      if (isa<NamedPattern>(inner) || isa<AnyPattern>(inner)) {
        contextualType = TypeLoc(typedPattern->getTypeRepr());
        if (typedPattern->hasType())
          contextualType.setType(typedPattern->getType());
        else
          contextualType.setType(patternType);
      }
    }
  }

  ContextualTypeInfo contextInfo(contextualType, CTP_Initialization);
  SyntacticElementTarget target(initializer, dc, contextInfo,
                                /*isDiscarded=*/false);
  target.expression.pattern = pattern;
  target.expression.bindPatternVarsOneWay = bindPatternVarsOneWay;
  target.maybeApplyPropertyWrapper();
  return target;
}

SyntacticElementTarget SyntacticElementTarget::forInitialization(
    Expr *initializer, DeclContext *dc, Type patternType,
    PatternBindingDecl *patternBinding, unsigned patternBindingIndex,
    bool bindPatternVarsOneWay) {
  auto result = forInitialization(
      initializer, dc, patternType,
      patternBinding->getPattern(patternBindingIndex), bindPatternVarsOneWay);
  result.expression.initialization.patternBinding = patternBinding;
  result.expression.initialization.patternBindingIndex = patternBindingIndex;
  return result;
}

SyntacticElementTarget
SyntacticElementTarget::forForEachStmt(ForEachStmt *stmt, DeclContext *dc,
                                       bool bindPatternVarsOneWay) {
  SyntacticElementTarget target(
      stmt, dc, bindPatternVarsOneWay || bool(stmt->getWhere()));
  return target;
}

SyntacticElementTarget SyntacticElementTarget::forPropertyWrapperInitializer(
    VarDecl *wrappedVar, DeclContext *dc, Expr *initializer) {
  SyntacticElementTarget target(initializer, dc, CTP_Initialization,
                                wrappedVar->getTypeInContext(),
                                /*isDiscarded=*/false);
  target.expression.propertyWrapper.wrappedVar = wrappedVar;
  if (auto *patternBinding = wrappedVar->getParentPatternBinding()) {
    auto index = patternBinding->getPatternEntryIndexForVarDecl(wrappedVar);
    target.expression.initialization.patternBinding = patternBinding;
    target.expression.initialization.patternBindingIndex = index;
    target.expression.pattern = patternBinding->getPattern(index);
  }
  target.maybeApplyPropertyWrapper();
  return target;
}

SyntacticElementTarget
SyntacticElementTarget::forExprPattern(ExprPattern *pattern) {
  auto *DC = pattern->getDeclContext();
  auto &ctx = DC->getASTContext();

  // Result of ~= operator is always a `Bool`.
  SyntacticElementTarget target(pattern->getMatchExpr(), DC, CTP_ExprPattern,
                                ctx.getBoolType(), /*isDiscarded*/ false);
  target.setPattern(pattern);
  return target;
}

ContextualPattern SyntacticElementTarget::getContextualPattern() const {
  if (kind == Kind::uninitializedVar) {
    assert(patternBinding);
    return ContextualPattern::forPatternBindingDecl(patternBinding,
                                                    uninitializedVar.index);
  }

  if (isForEachStmt()) {
    return ContextualPattern::forRawPattern(forEachStmt.pattern,
                                            forEachStmt.dc);
  }

  auto ctp = getExprContextualTypePurpose();
  assert(ctp == CTP_Initialization);

  if (ctp == CTP_Initialization && expression.initialization.patternBinding) {
    return ContextualPattern::forPatternBindingDecl(
        expression.initialization.patternBinding,
        expression.initialization.patternBindingIndex);
  }

  return ContextualPattern::forRawPattern(expression.pattern, expression.dc);
}

bool SyntacticElementTarget::infersOpaqueReturnType() const {
  switch (getExprContextualTypePurpose()) {
  case CTP_Initialization:
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    if (Type convertType = getExprContextualType())
      return convertType->hasOpaqueArchetype();
    return false;
  default:
    return false;
  }
}

bool SyntacticElementTarget::contextualTypeIsOnlyAHint() const {
  switch (getExprContextualTypePurpose()) {
  case CTP_Initialization:
    return !infersOpaqueReturnType() && !isOptionalSomePatternInit();
  case CTP_ForEachStmt:
  case CTP_ForEachSequence:
    return true;
  case CTP_Unused:
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
  case CTP_YieldByValue:
  case CTP_YieldByReference:
  case CTP_CaseStmt:
  case CTP_ThrowStmt:
  case CTP_DiscardStmt:
  case CTP_EnumCaseRawValue:
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
  case CTP_CalleeResult:
  case CTP_CallArgument:
  case CTP_ClosureResult:
  case CTP_ArrayElement:
  case CTP_DictionaryKey:
  case CTP_DictionaryValue:
  case CTP_CoerceOperand:
  case CTP_AssignSource:
  case CTP_SubscriptAssignSource:
  case CTP_Condition:
  case CTP_WrappedProperty:
  case CTP_ComposedPropertyWrapper:
  case CTP_CannotFail:
  case CTP_ExprPattern:
  case CTP_SingleValueStmtBranch:
    return false;
  }
  llvm_unreachable("invalid contextual type");
}

llvm::Optional<SyntacticElementTarget>
SyntacticElementTarget::walk(ASTWalker &walker) const {
  SyntacticElementTarget result = *this;
  switch (kind) {
  case Kind::expression: {
    if (isForInitialization()) {
      if (auto *newPattern = getInitializationPattern()->walk(walker)) {
        result.setPattern(newPattern);
      } else {
        return llvm::None;
      }
    }
    if (auto *newExpr = getAsExpr()->walk(walker)) {
      result.setExpr(newExpr);
    } else {
      return llvm::None;
    }
    break;
  }
  case Kind::closure: {
    if (auto *newClosure = closure.closure->walk(walker)) {
      result.closure.closure = cast<ClosureExpr>(newClosure);
    } else {
      return llvm::None;
    }
    break;
  }
  case Kind::function: {
    if (auto *newBody = getFunctionBody()->walk(walker)) {
      result.function.body = cast<BraceStmt>(newBody);
    } else {
      return llvm::None;
    }
    break;
  }
  case Kind::stmtCondition: {
    for (auto &condElement : stmtCondition.stmtCondition)
      condElement = *condElement.walk(walker);

    break;
  }
  case Kind::caseLabelItem: {
    auto *item = caseLabelItem.caseLabelItem;
    if (auto *newPattern = item->getPattern()->walk(walker)) {
      item->setPattern(newPattern, item->isPatternResolved());
    } else {
      return llvm::None;
    }
    if (auto guardExpr = item->getGuardExpr()) {
      if (auto newGuardExpr = guardExpr->walk(walker)) {
        item->setGuardExpr(newGuardExpr);
      } else {
        return llvm::None;
      }
    }
    break;
  }
  case Kind::patternBinding: {
    if (getAsPatternBinding()->walk(walker))
      return llvm::None;
    break;
  }
  case Kind::uninitializedVar: {
    if (auto *P = getAsUninitializedVar()->walk(walker)) {
      result.setPattern(P);
    } else {
      return llvm::None;
    }
    break;
  }
  case Kind::forEachStmt: {
    if (auto *newStmt = getAsForEachStmt()->walk(walker)) {
      result.forEachStmt.stmt = cast<ForEachStmt>(newStmt);
    } else {
      return llvm::None;
    }
    break;
  }
  }
  return result;
}

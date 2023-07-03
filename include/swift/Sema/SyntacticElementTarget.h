//===--- SyntacticElementTarget.h - Syntactic Element Target ----*- C++ -*-===//
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
//  This file defines the SyntacticElementTarget class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_SYNTACTIC_ELEMENT_TARGET_H
#define SWIFT_SEMA_SYNTACTIC_ELEMENT_TARGET_H

#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Sema/ConstraintLocator.h"

namespace swift {

namespace constraints {
/// Describes information about a for-each loop that needs to be tracked
/// within the constraint system.
struct ForEachStmtInfo {
  /// The type of the sequence.
  Type sequenceType;

  /// The type of an element in the sequence.
  Type elementType;

  /// The type of the pattern that matches the elements.
  Type initType;

  /// Implicit `$iterator = <sequence>.makeIterator()`
  PatternBindingDecl *makeIteratorVar;

  /// Implicit `$iterator.next()` call.
  Expr *nextCall;
};

/// Describes the target to which a constraint system's solution can be
/// applied.
class SyntacticElementTarget {
public:
  enum class Kind {
    expression,
    closure,
    function,
    stmtCondition,
    caseLabelItem,
    patternBinding,
    uninitializedVar,
    forEachStmt,
  } kind;

private:
  union {
    struct {
      /// The expression being type-checked.
      Expr *expression;

      /// The declaration context in which the expression is being
      /// type-checked.
      DeclContext *dc;

      // TODO: Fold the 3 below fields into ContextualTypeInfo

      /// The purpose of the contextual type.
      ContextualTypePurpose contextualPurpose;

      /// The type to which the expression should be converted.
      TypeLoc convertType;

      /// The locator for the contextual type conversion constraint, or
      /// \c nullptr to use the default locator which is anchored directly on
      /// the expression.
      ConstraintLocator *convertTypeLocator;

      /// When initializing a pattern from the expression, this is the
      /// pattern.
      Pattern *pattern;

      struct {
        /// The variable to which property wrappers have been applied, if
        /// this is an initialization involving a property wrapper.
        VarDecl *wrappedVar;

        /// The innermost call to \c init(wrappedValue:), if this is an
        /// initialization involving a property wrapper.
        ApplyExpr *innermostWrappedValueInit;

        /// Whether this property wrapper has an initial wrapped value specified
        /// via \c = .
        bool hasInitialWrappedValue;
      } propertyWrapper;

      /// Whether the expression result will be discarded at the end.
      bool isDiscarded;

      /// Whether to bind the variables encountered within the pattern to
      /// fresh type variables via one-way constraints.
      bool bindPatternVarsOneWay;

      union {
        struct {
          /// The pattern binding declaration for an initialization, if any.
          PatternBindingDecl *patternBinding;

          /// The index into the pattern binding declaration, if any.
          unsigned patternBindingIndex;
        } initialization;
      };
    } expression;

    struct {
      /// The closure expression being type-checked.
      ClosureExpr *closure;

      /// The type to which the expression should be converted.
      Type convertType;
    } closure;

    struct {
      AnyFunctionRef function;
      BraceStmt *body;
    } function;

    struct {
      StmtCondition stmtCondition;
      DeclContext *dc;
    } stmtCondition;

    struct {
      CaseLabelItem *caseLabelItem;
      DeclContext *dc;
    } caseLabelItem;

    struct {
      PatternBindingDecl *binding;
      /// Index into pattern binding declaration (if any).
      unsigned index;
      PointerUnion<VarDecl *, Pattern *> declaration;
      /// Type associated with the declaration.
      Type type;
    } uninitializedVar;

    struct {
      ForEachStmt *stmt;
      DeclContext *dc;
      Pattern *pattern;
      bool bindPatternVarsOneWay;
      ForEachStmtInfo info;
    } forEachStmt;

    PatternBindingDecl *patternBinding;
  };

  // If the pattern contains a single variable that has an attached
  // property wrapper, set up the initializer expression to initialize
  // the backing storage.
  void maybeApplyPropertyWrapper();

public:
  SyntacticElementTarget(Expr *expr, DeclContext *dc,
                         ContextualTypePurpose contextualPurpose,
                         Type convertType,
                         ConstraintLocator *convertTypeLocator,
                         bool isDiscarded)
      : SyntacticElementTarget(expr, dc, contextualPurpose,
                               TypeLoc::withoutLoc(convertType),
                               convertTypeLocator, isDiscarded) {}

  SyntacticElementTarget(Expr *expr, DeclContext *dc,
                         ContextualTypePurpose contextualPurpose,
                         Type convertType, bool isDiscarded)
      : SyntacticElementTarget(expr, dc, contextualPurpose, convertType,
                               /*convertTypeLocator*/ nullptr, isDiscarded) {}

  SyntacticElementTarget(Expr *expr, DeclContext *dc,
                         ContextualTypePurpose contextualPurpose,
                         TypeLoc convertType,
                         ConstraintLocator *convertTypeLocator,
                         bool isDiscarded);

  SyntacticElementTarget(ClosureExpr *closure, Type convertType) {
    kind = Kind::closure;
    this->closure.closure = closure;
    this->closure.convertType = convertType;
  }

  SyntacticElementTarget(AnyFunctionRef fn)
      : SyntacticElementTarget(fn, fn.getBody()) {}

  SyntacticElementTarget(StmtCondition stmtCondition, DeclContext *dc) {
    kind = Kind::stmtCondition;
    this->stmtCondition.stmtCondition = stmtCondition;
    this->stmtCondition.dc = dc;
  }

  SyntacticElementTarget(AnyFunctionRef fn, BraceStmt *body) {
    kind = Kind::function;
    function.function = fn;
    function.body = body;
  }

  SyntacticElementTarget(CaseLabelItem *caseLabelItem, DeclContext *dc) {
    kind = Kind::caseLabelItem;
    this->caseLabelItem.caseLabelItem = caseLabelItem;
    this->caseLabelItem.dc = dc;
  }

  SyntacticElementTarget(PatternBindingDecl *patternBinding) {
    kind = Kind::patternBinding;
    this->patternBinding = patternBinding;
  }

  SyntacticElementTarget(VarDecl *uninitializedWrappedVar)
      : kind(Kind::uninitializedVar) {
    if (auto *PDB = uninitializedWrappedVar->getParentPatternBinding()) {
      uninitializedVar.binding = PDB;
      uninitializedVar.index =
          PDB->getPatternEntryIndexForVarDecl(uninitializedWrappedVar);
    } else {
      uninitializedVar.binding = nullptr;
      uninitializedVar.index = 0;
    }

    uninitializedVar.declaration = uninitializedWrappedVar;
    uninitializedVar.type = Type();
  }

  SyntacticElementTarget(PatternBindingDecl *binding, unsigned index,
                         Pattern *var, Type patternTy)
      : kind(Kind::uninitializedVar) {
    uninitializedVar.binding = binding;
    uninitializedVar.index = index;
    uninitializedVar.declaration = var;
    uninitializedVar.type = patternTy;
  }

  SyntacticElementTarget(ForEachStmt *stmt, DeclContext *dc,
                         bool bindPatternVarsOneWay)
      : kind(Kind::forEachStmt) {
    forEachStmt.stmt = stmt;
    forEachStmt.dc = dc;
    forEachStmt.bindPatternVarsOneWay = bindPatternVarsOneWay;
  }

  /// Form a target for the initialization of a pattern from an expression.
  static SyntacticElementTarget
  forInitialization(Expr *initializer, DeclContext *dc, Type patternType,
                    Pattern *pattern, bool bindPatternVarsOneWay);

  /// Form a target for the initialization of a pattern binding entry from
  /// an expression.
  static SyntacticElementTarget
  forInitialization(Expr *initializer, DeclContext *dc, Type patternType,
                    PatternBindingDecl *patternBinding,
                    unsigned patternBindingIndex, bool bindPatternVarsOneWay);

  /// Form a target for a for-in loop.
  static SyntacticElementTarget forForEachStmt(ForEachStmt *stmt,
                                               DeclContext *dc,
                                               bool bindPatternVarsOneWay);

  /// Form a target for a property with an attached property wrapper that is
  /// initialized out-of-line.
  static SyntacticElementTarget
  forUninitializedWrappedVar(VarDecl *wrappedVar) {
    return {wrappedVar};
  }

  static SyntacticElementTarget forUninitializedVar(PatternBindingDecl *binding,
                                                    unsigned index,
                                                    Type patternTy) {
    return {binding, index, binding->getPattern(index), patternTy};
  }

  /// Form a target for a synthesized property wrapper initializer.
  static SyntacticElementTarget
  forPropertyWrapperInitializer(VarDecl *wrappedVar, DeclContext *dc,
                                Expr *initializer);

  /// Form a target for the match expression of an ExprPattern.
  static SyntacticElementTarget forExprPattern(ExprPattern *pattern);

  /// This is useful for code completion.
  ASTNode getAsASTNode() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression;

    case Kind::closure:
      return closure.closure;

    case Kind::function: {
      auto ref = *getAsFunction();
      if (auto *func = ref.getAbstractFunctionDecl())
        return func;
      return ref.getAbstractClosureExpr();
    }

    case Kind::forEachStmt:
      return getAsForEachStmt();

    case Kind::stmtCondition:
      return ASTNode();

    case Kind::caseLabelItem:
      return *getAsCaseLabelItem();

    case Kind::patternBinding:
      return getAsPatternBinding();

    case Kind::uninitializedVar:
      return getAsUninitializedVar();
    }
  }

  Expr *getAsExpr() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression;

    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedVar:
    case Kind::forEachStmt:
      return nullptr;
    }
    llvm_unreachable("invalid expression type");
  }

  DeclContext *getDeclContext() const {
    switch (kind) {
    case Kind::expression:
      return expression.dc;

    case Kind::closure:
      return closure.closure;

    case Kind::function:
      return function.function.getAsDeclContext();

    case Kind::stmtCondition:
      return stmtCondition.dc;

    case Kind::caseLabelItem:
      return caseLabelItem.dc;

    case Kind::patternBinding:
      return patternBinding->getDeclContext();

    case Kind::uninitializedVar: {
      if (auto *wrappedVar = uninitializedVar.declaration.dyn_cast<VarDecl *>())
        return wrappedVar->getDeclContext();

      return uninitializedVar.binding->getInitContext(uninitializedVar.index);
    }

    case Kind::forEachStmt:
      return forEachStmt.dc;
    }
    llvm_unreachable("invalid decl context type");
  }

  ContextualTypePurpose getExprContextualTypePurpose() const {
    assert(kind == Kind::expression);
    return expression.contextualPurpose;
  }

  Type getExprContextualType() const {
    return getExprContextualTypeLoc().getType();
  }

  TypeLoc getExprContextualTypeLoc() const {
    assert(kind == Kind::expression);

    // For an @autoclosure parameter, the conversion type is
    // the result of the function type.
    if (FunctionType *autoclosureParamType = getAsAutoclosureParamType()) {
      return TypeLoc(expression.convertType.getTypeRepr(),
                     autoclosureParamType->getResult());
    }

    return expression.convertType;
  }

  /// Retrieve the type to which an expression should be converted, or
  /// a NULL type if no conversion constraint should be generated.
  Type getExprConversionType() const {
    if (contextualTypeIsOnlyAHint())
      return Type();
    return getExprContextualType();
  }

  /// Retrieve the conversion type locator for the expression, or \c nullptr
  /// if it has not been set.
  ConstraintLocator *getExprConvertTypeLocator() const {
    assert(kind == Kind::expression);
    return expression.convertTypeLocator;
  }

  /// Returns the autoclosure parameter type, or \c nullptr if the
  /// expression has a different kind of context.
  FunctionType *getAsAutoclosureParamType() const {
    assert(kind == Kind::expression);
    if (expression.contextualPurpose == CTP_AutoclosureDefaultParameter)
      return expression.convertType.getType()->castTo<FunctionType>();
    return nullptr;
  }

  void setExprConversionType(Type type) {
    assert(kind == Kind::expression);
    expression.convertType = TypeLoc::withoutLoc(type);
  }

  void setExprConversionTypeLoc(TypeLoc type) {
    assert(kind == Kind::expression);
    expression.convertType = type;
  }

  /// Whether this target is for an initialization expression and pattern.
  bool isForInitialization() const {
    return kind == Kind::expression &&
           expression.contextualPurpose == CTP_Initialization;
  }

  /// For a pattern initialization target, retrieve the pattern.
  Pattern *getInitializationPattern() const {
    if (kind == Kind::uninitializedVar)
      return uninitializedVar.declaration.get<Pattern *>();

    assert(isForInitialization());
    return expression.pattern;
  }

  ExprPattern *getExprPattern() const {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_ExprPattern);
    return cast<ExprPattern>(expression.pattern);
  }

  Type getClosureContextualType() const {
    assert(kind == Kind::closure);
    return closure.convertType;
  }

  /// For a pattern initialization target, retrieve the contextual pattern.
  ContextualPattern getContextualPattern() const;

  /// Whether this target is for a for-in statement.
  bool isForEachStmt() const { return kind == Kind::forEachStmt; }

  /// Whether this is an initialization for an Optional.Some pattern.
  bool isOptionalSomePatternInit() const {
    return isForInitialization() &&
           dyn_cast_or_null<OptionalSomePattern>(expression.pattern) &&
           !expression.pattern->isImplicit();
  }

  /// Check whether this is an initialization for `async let` pattern.
  bool isAsyncLetInitializer() const {
    if (!isForInitialization())
      return false;

    if (auto *PBD = getInitializationPatternBindingDecl())
      return PBD->isAsyncLet();
    return false;
  }

  /// Whether to bind the types of any variables within the pattern via
  /// one-way constraints.
  bool shouldBindPatternVarsOneWay() const {
    if (kind == Kind::expression)
      return expression.bindPatternVarsOneWay;

    if (kind == Kind::forEachStmt)
      return forEachStmt.bindPatternVarsOneWay;

    return false;
  }

  /// Whether or not an opaque value placeholder should be injected into the
  /// first \c wrappedValue argument of an apply expression so the initializer
  /// expression can be turned into a property wrapper generator function.
  bool shouldInjectWrappedValuePlaceholder(ApplyExpr *apply) const {
    if (!isForInitialization())
      return false;

    auto *wrappedVar = expression.propertyWrapper.wrappedVar;
    if (!apply || !wrappedVar)
      return false;

    // Don't create property wrapper generator functions for static variables
    // and local variables with initializers.
    bool hasInit = expression.propertyWrapper.hasInitialWrappedValue;
    if (wrappedVar->isStatic() ||
        (hasInit && wrappedVar->getDeclContext()->isLocalContext()))
      return false;

    return expression.propertyWrapper.innermostWrappedValueInit == apply;
  }

  /// Whether this target is for initialization of a property wrapper
  /// with an initial wrapped value specified via \c = .
  bool propertyWrapperHasInitialWrappedValue() const {
    return (kind == Kind::expression &&
            expression.propertyWrapper.hasInitialWrappedValue);
  }

  /// Retrieve the wrapped variable when initializing a pattern with a
  /// property wrapper.
  VarDecl *getInitializationWrappedVar() const {
    assert(isForInitialization());
    return expression.propertyWrapper.wrappedVar;
  }

  PatternBindingDecl *getInitializationPatternBindingDecl() const {
    assert(isForInitialization());
    return expression.initialization.patternBinding;
  }

  unsigned getInitializationPatternBindingIndex() const {
    assert(isForInitialization());
    return expression.initialization.patternBindingIndex;
  }

  const ForEachStmtInfo &getForEachStmtInfo() const {
    assert(isForEachStmt());
    return forEachStmt.info;
  }

  ForEachStmtInfo &getForEachStmtInfo() {
    assert(isForEachStmt());
    return forEachStmt.info;
  }

  /// Whether this context infers an opaque return type.
  bool infersOpaqueReturnType() const;

  /// Whether the contextual type is only a hint, rather than a type
  bool contextualTypeIsOnlyAHint() const;

  bool isDiscardedExpr() const {
    assert(kind == Kind::expression);
    return expression.isDiscarded;
  }

  void setExpr(Expr *expr) {
    assert(kind == Kind::expression);
    expression.expression = expr;
  }

  void setPattern(Pattern *pattern) {
    if (kind == Kind::uninitializedVar) {
      assert(uninitializedVar.declaration.is<Pattern *>());
      uninitializedVar.declaration = pattern;
      return;
    }

    if (kind == Kind::forEachStmt) {
      forEachStmt.pattern = pattern;
      return;
    }

    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization ||
           expression.contextualPurpose == CTP_ForEachStmt ||
           expression.contextualPurpose == CTP_ForEachSequence ||
           expression.contextualPurpose == CTP_ExprPattern);
    expression.pattern = pattern;
  }

  llvm::Optional<AnyFunctionRef> getAsFunction() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedVar:
    case Kind::forEachStmt:
      return llvm::None;

    case Kind::function:
      return function.function;
    }
    llvm_unreachable("invalid function kind");
  }

  llvm::Optional<StmtCondition> getAsStmtCondition() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedVar:
    case Kind::forEachStmt:
      return llvm::None;

    case Kind::stmtCondition:
      return stmtCondition.stmtCondition;
    }
    llvm_unreachable("invalid statement kind");
  }

  llvm::Optional<CaseLabelItem *> getAsCaseLabelItem() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::patternBinding:
    case Kind::uninitializedVar:
    case Kind::forEachStmt:
      return llvm::None;

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem;
    }
    llvm_unreachable("invalid case label type");
  }

  PatternBindingDecl *getAsPatternBinding() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::uninitializedVar:
    case Kind::forEachStmt:
      return nullptr;

    case Kind::patternBinding:
      return patternBinding;
    }
    llvm_unreachable("invalid case label type");
  }

  VarDecl *getAsUninitializedWrappedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::forEachStmt:
      return nullptr;

    case Kind::uninitializedVar:
      return uninitializedVar.declaration.dyn_cast<VarDecl *>();
    }
    llvm_unreachable("invalid case label type");
  }

  Pattern *getAsUninitializedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::forEachStmt:
      return nullptr;

    case Kind::uninitializedVar:
      return uninitializedVar.declaration.dyn_cast<Pattern *>();
    }
    llvm_unreachable("invalid case label type");
  }

  ForEachStmt *getAsForEachStmt() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedVar:
      return nullptr;

    case Kind::forEachStmt:
      return forEachStmt.stmt;
    }
    llvm_unreachable("invalid case label type");
  }

  Type getTypeOfUninitializedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::forEachStmt:
      return nullptr;

    case Kind::uninitializedVar:
      return uninitializedVar.type;
    }
    llvm_unreachable("invalid case label type");
  }

  PatternBindingDecl *getPatternBindingOfUninitializedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::forEachStmt:
      return nullptr;

    case Kind::uninitializedVar:
      return uninitializedVar.binding;
    }
    llvm_unreachable("invalid case label type");
  }

  unsigned getIndexOfUninitializedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::closure:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::forEachStmt:
      return 0;

    case Kind::uninitializedVar:
      return uninitializedVar.index;
    }
    llvm_unreachable("invalid case label type");
  }

  BraceStmt *getFunctionBody() const {
    assert(kind == Kind::function);
    return function.body;
  }

  void setFunctionBody(BraceStmt *stmt) {
    assert(kind == Kind::function);
    function.body = stmt;
  }

  /// Retrieve the source range of the target.
  SourceRange getSourceRange() const {
    switch (kind) {
    case Kind::expression: {
      auto range = expression.expression->getSourceRange();

      // For an initialization, include the pattern in the range too.
      if (isForInitialization()) {
        if (auto *pattern = getInitializationPattern()) {
          if (auto patternRange = pattern->getSourceRange()) {
            if (range.isInvalid()) {
              range = patternRange;
            } else {
              range.widen(patternRange);
            }
          }
        }
      }
      return range;
    }
    case Kind::closure:
      return closure.closure->getSourceRange();

    case Kind::function:
      return function.body->getSourceRange();

    case Kind::stmtCondition:
      return SourceRange(stmtCondition.stmtCondition.front().getStartLoc(),
                         stmtCondition.stmtCondition.back().getEndLoc());

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem->getSourceRange();

    case Kind::patternBinding:
      return patternBinding->getSourceRange();

    case Kind::uninitializedVar: {
      if (auto *wrappedVar =
              uninitializedVar.declaration.dyn_cast<VarDecl *>()) {
        return wrappedVar->getSourceRange();
      }
      return uninitializedVar.declaration.get<Pattern *>()->getSourceRange();
    }

    // For-in statement target doesn't cover the body.
    case Kind::forEachStmt:
      auto *stmt = forEachStmt.stmt;
      SourceLoc startLoc = stmt->getForLoc();
      SourceLoc endLoc = stmt->getParsedSequence()->getEndLoc();

      if (auto *whereExpr = stmt->getWhere()) {
        endLoc = whereExpr->getEndLoc();
      }

      return {startLoc, endLoc};
    }
    llvm_unreachable("invalid target type");
  }

  /// Retrieve the source location for the target.
  SourceLoc getLoc() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression->getLoc();

    case Kind::closure:
      return closure.closure->getLoc();

    case Kind::function:
      return function.function.getLoc();

    case Kind::stmtCondition:
      return stmtCondition.stmtCondition.front().getStartLoc();

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem->getStartLoc();

    case Kind::patternBinding:
      return patternBinding->getLoc();

    case Kind::uninitializedVar: {
      if (auto *wrappedVar =
              uninitializedVar.declaration.dyn_cast<VarDecl *>()) {
        return wrappedVar->getLoc();
      }
      return uninitializedVar.declaration.get<Pattern *>()->getLoc();
    }

    case Kind::forEachStmt:
      return forEachStmt.stmt->getStartLoc();
    }
    llvm_unreachable("invalid target type");
  }

  /// Walk the contents of the application target.
  llvm::Optional<SyntacticElementTarget> walk(ASTWalker &walker) const;
};

} // namespace constraints
} // namespace swift

#endif /* SWIFT_SEMA_SYNTACTIC_ELEMENT_TARGET_H */

//===--- ExprContextAnalysis.h - Expession context analysis ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_EXPRCONTEXTANALYSIS_H
#define SWIFT_IDE_EXPRCONTEXTANALYSIS_H

#include "swift/AST/Type.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/IDE/PossibleParamInfo.h"

namespace swift {
class DeclContext;
class Expr;
class ValueDecl;

namespace ide {
enum class SemanticContextKind : uint8_t;

/// Type check parent contexts of the given decl context, and the body of the
/// given context until \c Loc if the context is a function body.
void typeCheckContextAt(TypeCheckASTNodeAtLocContext TypeCheckCtx,
                        SourceLoc Loc);

/// From \p DC, find and returns the outer most expression which source range is
/// exact the same as \p TargetRange. Returns \c nullptr if not found.
Expr *findParsedExpr(const DeclContext *DC, SourceRange TargetRange);

/// Remove \c CodeCompletionExpr from \p expr . Returns \c true if it actually
/// mutated the expression.
///
/// NOTE: Currently, this only removes CodeCompletionExpr at call argument
///       position.
bool removeCodeCompletionExpr(ASTContext &Ctx, Expr *&expr);

/// Collects possible expected return types of the given decl context.
/// \p DC should be an \c AbstractFunctionDecl or an \c AbstractClosureExpr.
void collectPossibleReturnTypesFromContext(DeclContext *DC,
                                           SmallVectorImpl<Type> &candidates);

struct FunctionTypeAndDecl {
  AnyFunctionType *Type;
  ValueDecl *Decl;
  llvm::Optional<SemanticContextKind> SemanticContext;

  FunctionTypeAndDecl(AnyFunctionType *Type, ValueDecl *Decl)
      : Type(Type), Decl(Decl) {}
  FunctionTypeAndDecl(AnyFunctionType *Type, ValueDecl *Decl,
                      SemanticContextKind SemanticContext)
      : Type(Type), Decl(Decl), SemanticContext(SemanticContext) {}
};

/// Given an expression and its decl context, the analyzer tries to figure out
/// the expected type of the expression by analyzing its context.
class ExprContextInfo {
  SmallVector<Type, 2> PossibleTypes;
  SmallVector<PossibleParamInfo, 2> PossibleParams;
  SmallVector<FunctionTypeAndDecl, 2> PossibleCallees;
  Expr *AnalyzedExpr = nullptr;
  bool implicitSingleExpressionReturn = false;

public:
  ExprContextInfo(DeclContext *DC, Expr *TargetExpr);

  // Returns a list of possible context types.
  ArrayRef<Type> getPossibleTypes() const { return PossibleTypes; }

  /// Whether the type context comes from a single-expression body without
  /// and explicit return e.g. `foo({ <here> })` and not foo({ return <here>}).
  ///
  /// Since the input may be incomplete, such as in code-completion, take into
  /// account that the types returned by `getPossibleTypes()` are only a hint.
  bool isImplicitSingleExpressionReturn() const {
    return implicitSingleExpressionReturn;
  }

  // Returns a list of possible argument label names.
  // Valid only if \c getKind() is \c CallArgument.
  ArrayRef<PossibleParamInfo> getPossibleParams() const {
    return PossibleParams;
  }

  // Returns a list of possible callee
  // Valid only if \c getKind() is \c CallArgument.
  ArrayRef<FunctionTypeAndDecl> getPossibleCallees() const {
    return PossibleCallees;
  }

  Expr *getAnalyzedExpr() const {
    return AnalyzedExpr;
  }
};

} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_EXPRCONTEXTANALYSIS_H

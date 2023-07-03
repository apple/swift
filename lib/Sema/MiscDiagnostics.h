//===--- MiscDiagnostics.h - AST-Level Diagnostics --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_MISC_DIAGNOSTICS_H
#define SWIFT_SEMA_MISC_DIAGNOSTICS_H

#include "swift/AST/ASTWalker.h"
#include "swift/AST/AttrKind.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Identifier.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"

namespace swift {
  class AbstractFunctionDecl;
  class ApplyExpr;
  class CallExpr;
  class ClosureExpr;
  class DeclContext;
  class Decl;
  class Expr;
  class InFlightDiagnostic;
  class Stmt;
  class TopLevelCodeDecl;
  class ValueDecl;
  class ForEachStmt;

/// Emit diagnostics for syntactic restrictions on a given expression.
void performSyntacticExprDiagnostics(
    const Expr *E, const DeclContext *DC,
    bool isExprStmt, bool disableExprAvailabilityChecking = false);

/// Emit diagnostics for a given statement.
void performStmtDiagnostics(const Stmt *S, DeclContext *DC);

void performAbstractFuncDeclDiagnostics(AbstractFunctionDecl *AFD);

/// Perform diagnostics on the top level code declaration.
void performTopLevelDeclDiagnostics(TopLevelCodeDecl *TLCD);
  
/// Emit a fix-it to set the access of \p VD to \p desiredAccess.
///
/// This actually updates \p VD as well.
void fixItAccess(InFlightDiagnostic &diag,
                 ValueDecl *VD,
                 AccessLevel desiredAccess,
                 bool isForSetter = false,
                 bool shouldUseDefaultAccess = false);

/// Describes the context of a parameter, for use in diagnosing argument
/// label problems.
enum class ParameterContext: unsigned {
  Call = 0,
  Subscript = 1,
  MacroExpansion = 2
};

/// Emit fix-its to correct the argument labels in \p argList.
///
/// If \p existingDiag is null, the fix-its will be attached to an appropriate
/// error diagnostic.
///
/// \returns true if the issue was diagnosed
bool diagnoseArgumentLabelError(ASTContext &ctx,
                                const ArgumentList *argList,
                                ArrayRef<Identifier> newNames,
                                ParameterContext paramContext,
                                InFlightDiagnostic *existingDiag = nullptr);

/// If \p assignExpr has a destination expression that refers to a declaration
/// with a non-owning attribute, such as 'weak' or 'unowned' and the initializer
/// expression refers to a class constructor, emit a warning that the assigned
/// instance will be immediately deallocated.
void diagnoseUnownedImmediateDeallocation(ASTContext &ctx,
                                          const AssignExpr *assignExpr);

/// If \p pattern binds to a declaration with a non-owning attribute, such as
/// 'weak' or 'unowned' and \p initializer refers to a class constructor,
/// emit a warning that the bound instance will be immediately deallocated.
void diagnoseUnownedImmediateDeallocation(ASTContext &ctx,
                                          const Pattern *pattern,
                                          SourceLoc equalLoc,
                                          const Expr *initializer);

/// If \p expr is a call to a known function with a requirement that some
/// arguments must be constants, whether those arguments are passed only
/// constants. Otherwise, diagnose and emit errors.
void diagnoseConstantArgumentRequirement(const Expr *expr,
                                         const DeclContext *declContext);

/// Attempt to fix the type of \p decl so that it's a valid override for
/// \p base...but only if we're highly confident that we know what the user
/// should have written.
///
/// The \p diag closure allows the caller to control the diagnostic that is
/// emitted. It is passed true if the diagnostic will be emitted with fixits
/// attached, and false otherwise. If None is returned, no diagnostics are
/// emitted.  Else the fixits are attached to the returned diagnostic.
///
/// \returns true iff any fix-its were attached to \p diag.
bool computeFixitsForOverriddenDeclaration(
    ValueDecl *decl, const ValueDecl *base,
    llvm::function_ref<llvm::Optional<InFlightDiagnostic>(bool)> diag);

/// Emit fix-its to enclose trailing closure in argument parens.
void fixItEncloseTrailingClosure(ASTContext &ctx,
                                 InFlightDiagnostic &diag,
                                 const CallExpr *call,
                                 Identifier closureLabel);

/// Check that we use the async version of a function where available
///
/// If a completion-handler function is called from an async context and it has
/// a '@available' attribute with renamed field pointing to an async function,
/// we emit a diagnostic suggesting the async call.
void checkFunctionAsyncUsage(AbstractFunctionDecl *decl);
void checkPatternBindingDeclAsyncUsage(PatternBindingDecl *decl);

/// Detect and diagnose a missing `try` in `for-in` loop sequence
/// expression in async context (denoted with `await` keyword).
bool diagnoseUnhandledThrowsInAsyncContext(DeclContext *dc,
                                           ForEachStmt *forEach);

class BaseDiagnosticWalker : public ASTWalker {
  PreWalkAction walkToDeclPre(Decl *D) override;

  bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
    return false;
  }

  // Only emit diagnostics in the expansion of macros.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

private:
  static bool shouldWalkIntoDeclInClosureContext(Decl *D);
};

void diagnoseCopyableTypeContainingMoveOnlyType(
    NominalTypeDecl *copyableNominalType);

} // namespace swift

#endif // SWIFT_SEMA_MISC_DIAGNOSTICS_H


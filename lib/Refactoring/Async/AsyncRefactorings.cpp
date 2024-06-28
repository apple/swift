//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "AsyncRefactoring.h"
#include "RefactoringActions.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

/// Find the outermost call of the given location
static CallExpr *findOuterCall(ResolvedCursorInfoPtr CursorInfo) {
  auto IncludeInContext = [](ASTNode N) {
    if (auto *E = N.dyn_cast<Expr *>())
      return !E->isImplicit();
    return false;
  };

  // TODO: Bit pointless using the "ContextFinder" here. Ideally we would have
  //       already generated a slice of the AST for anything that contains
  //       the cursor location
  ContextFinder Finder(*CursorInfo->getSourceFile(), CursorInfo->getLoc(),
                       IncludeInContext);
  Finder.resolve();
  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return nullptr;

  CallExpr *CE = dyn_cast<CallExpr>(Contexts[0].get<Expr *>());
  if (!CE)
    return nullptr;

  SourceManager &SM = CursorInfo->getSourceFile()->getASTContext().SourceMgr;
  if (!SM.rangeContains(CE->getFn()->getSourceRange(), CursorInfo->getLoc()))
    return nullptr;
  return CE;
}

/// Find the function matching the given location if it is not an accessor and
/// either has a body or is a member of a protocol
static FuncDecl *findFunction(ResolvedCursorInfoPtr CursorInfo) {
  auto IncludeInContext = [](ASTNode N) {
    if (auto *D = N.dyn_cast<Decl *>())
      return !D->isImplicit();
    return false;
  };

  ContextFinder Finder(*CursorInfo->getSourceFile(), CursorInfo->getLoc(),
                       IncludeInContext);
  Finder.resolve();

  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return nullptr;

  if (Contexts.back().isDecl(DeclKind::Param))
    Contexts = Contexts.drop_back();

  auto *FD = dyn_cast_or_null<FuncDecl>(Contexts.back().get<Decl *>());
  if (!FD || isa<AccessorDecl>(FD))
    return nullptr;

  auto *Body = FD->getBody();
  if (!Body && !isa<ProtocolDecl>(FD->getDeclContext()))
    return nullptr;

  SourceManager &SM = CursorInfo->getSourceFile()->getASTContext().SourceMgr;
  SourceLoc DeclEnd = Body ? Body->getLBraceLoc() : FD->getEndLoc();
  if (!SM.rangeContains(SourceRange(FD->getStartLoc(), DeclEnd),
                        CursorInfo->getLoc()))
    return nullptr;

  return FD;
}

bool RefactoringActionConvertCallToAsyncAlternative::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  // Currently doesn't check that the call is in an async context. This seems
  // possibly useful in some situations, so we'll see what the feedback is.
  // May need to change in the future
  auto *CE = findOuterCall(CursorInfo);
  if (!CE)
    return false;

  auto HandlerDesc = AsyncHandlerParamDesc::find(
      getUnderlyingFunc(CE->getFn()), /*RequireAttributeOrName=*/false);
  return HandlerDesc.isValid();
}

/// Converts a call of a function with a possible async alternative, to use it
/// instead. Currently this is any function that
///   1. has a void return type,
///   2. has a void returning closure as its last parameter, and
///   3. is not already async
///
/// For now the call need not be in an async context, though this may change
/// depending on feedback.
bool RefactoringActionConvertCallToAsyncAlternative::performChange() {
  using namespace asyncrefactorings;

  auto *CE = findOuterCall(CursorInfo);
  assert(CE &&
         "Should not run performChange when refactoring is not applicable");

  // Find the scope this call is in
  ContextFinder Finder(
      *CursorInfo->getSourceFile(), CursorInfo->getLoc(),
      [](ASTNode N) { return N.isStmt(StmtKind::Brace) && !N.isImplicit(); });
  Finder.resolve();
  auto Scopes = Finder.getContexts();
  BraceStmt *Scope = nullptr;
  if (!Scopes.empty())
    Scope = cast<BraceStmt>(Scopes.back().get<Stmt *>());

  AsyncConverter Converter(TheFile, SM, DiagEngine, CE, Scope);
  if (!Converter.convert())
    return true;

  Converter.replace(CE, EditConsumer);
  return false;
}

bool RefactoringActionConvertToAsync::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  // As with the call refactoring, should possibly only apply if there's
  // actually calls to async alternatives. At the moment this will just add
  // `async` if there are no calls, which is probably fine.
  return findFunction(CursorInfo);
}

/// Converts a whole function to async, converting any calls to functions with
/// async alternatives as above.
bool RefactoringActionConvertToAsync::performChange() {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  assert(FD &&
         "Should not run performChange when refactoring is not applicable");

  auto HandlerDesc =
      AsyncHandlerParamDesc::find(FD, /*RequireAttributeOrName=*/false);
  AsyncConverter Converter(TheFile, SM, DiagEngine, FD, HandlerDesc);
  if (!Converter.convert())
    return true;

  Converter.replace(FD, EditConsumer, FD->getSourceRangeIncludingAttrs().Start);
  return false;
}

bool RefactoringActionAddAsyncAlternative::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  if (!FD)
    return false;

  auto HandlerDesc =
      AsyncHandlerParamDesc::find(FD, /*RequireAttributeOrName=*/false);
  return HandlerDesc.isValid();
}

/// Adds an async alternative and marks the current function as deprecated.
/// Equivalent to the conversion but
///   1. only works on functions that themselves are a possible async
///      alternative, and
///   2. has extra handling to convert the completion/handler/callback closure
///      parameter to either `return`/`throws`
bool RefactoringActionAddAsyncAlternative::performChange() {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  assert(FD &&
         "Should not run performChange when refactoring is not applicable");

  auto HandlerDesc =
      AsyncHandlerParamDesc::find(FD, /*RequireAttributeOrName=*/false);
  assert(HandlerDesc.isValid() &&
         "Should not run performChange when refactoring is not applicable");

  AsyncConverter Converter(TheFile, SM, DiagEngine, FD, HandlerDesc);
  if (!Converter.convert())
    return true;

  // Add a reference to the async function so that warnings appear when the
  // synchronous function is used in an async context
  SmallString<128> AvailabilityAttr = HandlerDesc.buildRenamedAttribute();
  EditConsumer.accept(SM, FD->getAttributeInsertionLoc(false),
                      AvailabilityAttr);

  AsyncConverter LegacyBodyCreator(TheFile, SM, DiagEngine, FD, HandlerDesc);
  if (LegacyBodyCreator.createLegacyBody()) {
    LegacyBodyCreator.replace(FD->getBody(), EditConsumer);
  }

  // Add the async alternative
  Converter.insertAfter(FD, EditConsumer);

  return false;
}

bool RefactoringActionAddAsyncWrapper::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  if (!FD)
    return false;

  auto HandlerDesc =
      AsyncHandlerParamDesc::find(FD, /*RequireAttributeOrName=*/false);
  return HandlerDesc.isValid();
}

bool RefactoringActionAddAsyncWrapper::performChange() {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  assert(FD &&
         "Should not run performChange when refactoring is not applicable");

  auto HandlerDesc =
      AsyncHandlerParamDesc::find(FD, /*RequireAttributeOrName=*/false);
  assert(HandlerDesc.isValid() &&
         "Should not run performChange when refactoring is not applicable");

  AsyncConverter Converter(TheFile, SM, DiagEngine, FD, HandlerDesc);
  if (!Converter.createAsyncWrapper())
    return true;

  // Add a reference to the async function so that warnings appear when the
  // synchronous function is used in an async context
  SmallString<128> AvailabilityAttr = HandlerDesc.buildRenamedAttribute();
  EditConsumer.accept(SM, FD->getAttributeInsertionLoc(false),
                      AvailabilityAttr);

  // Add the async wrapper.
  Converter.insertAfter(FD, EditConsumer);

  return false;
}

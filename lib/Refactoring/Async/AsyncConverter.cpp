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
#include "Utils.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace swift::refactoring::asyncrefactorings;


/// Base name of a decl if it has one, an empty \c DeclBaseName otherwise.
static DeclBaseName getDeclName(const Decl *D) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    if (VD->hasName())
      return VD->getBaseName();
  }
  return DeclBaseName();
}

bool AsyncConverter::convert() {
  assert(Buffer.empty() && "AsyncConverter can only be used once");

  if (auto *FD = dyn_cast_or_null<FuncDecl>(StartNode.dyn_cast<Decl *>())) {
    addFuncDecl(FD);
    if (FD->getBody()) {
      convertNode(FD->getBody());
    }
  } else {
    convertNode(StartNode, /*StartOverride=*/{}, /*ConvertCalls=*/true,
                /*IncludeComments=*/false);
  }
  return !DiagEngine.hadAnyError();
}

bool AsyncConverter::createLegacyBody() {
  assert(Buffer.empty() && "AsyncConverter can only be used once");

  if (!canCreateLegacyBody())
    return false;

  FuncDecl *FD = cast<FuncDecl>(StartNode.get<Decl *>());
  OS << tok::l_brace << "\n"; // start function body
  OS << "Task " << tok::l_brace << "\n";
  addHoistedNamedCallback(FD, TopHandler, TopHandler.getNameStr(), [&]() {
    if (TopHandler.HasError) {
      OS << tok::kw_try << " ";
    }
    OS << "await ";

    // Since we're *creating* the async alternative here, there shouldn't
    // already be one. Thus, just assume that the call to the alternative is
    // the same as the call to the old completion handler function, minus the
    // completion handler arg.
    addForwardingCallTo(FD, /*HandlerReplacement=*/"");
  });
  OS << "\n";
  OS << tok::r_brace << "\n"; // end 'Task'
  OS << tok::r_brace << "\n"; // end function body
  return true;
}

bool AsyncConverter::createAsyncWrapper() {
  assert(Buffer.empty() && "AsyncConverter can only be used once");
  auto *FD = cast<FuncDecl>(StartNode.get<Decl *>());

  // First add the new async function declaration.
  addFuncDecl(FD);
  OS << tok::l_brace << "\n";

  // Then add the body.
  OS << tok::kw_return << " ";
  if (TopHandler.HasError)
    OS << tok::kw_try << " ";

  OS << "await ";

  // withChecked[Throwing]Continuation { continuation in
  if (TopHandler.HasError) {
    OS << "withCheckedThrowingContinuation";
  } else {
    OS << "withCheckedContinuation";
  }
  OS << " " << tok::l_brace << " continuation " << tok::kw_in << "\n";

  // fnWithHandler(args...) { ... }
  auto ClosureStr =
      getAsyncWrapperCompletionClosure("continuation", TopHandler);
  addForwardingCallTo(FD, /*HandlerReplacement=*/ClosureStr);

  OS << "\n";
  OS << tok::r_brace << "\n"; // end continuation closure
  OS << tok::r_brace << "\n"; // end function body
  return true;
}

void AsyncConverter::replace(ASTNode Node, SourceEditConsumer &EditConsumer,
                             SourceLoc StartOverride) {
  SourceRange Range = Node.getSourceRange();
  if (StartOverride.isValid()) {
    Range = SourceRange(StartOverride, Range.End);
  }
  CharSourceRange CharRange =
      Lexer::getCharSourceRangeFromSourceRange(SM, Range);
  EditConsumer.accept(SM, CharRange, Buffer.str());
  Buffer.clear();
}

void AsyncConverter::insertAfter(ASTNode Node,
                                 SourceEditConsumer &EditConsumer) {
  EditConsumer.insertAfter(SM, Node.getEndLoc(), "\n\n");
  EditConsumer.insertAfter(SM, Node.getEndLoc(), Buffer.str());
  Buffer.clear();
}

bool AsyncConverter::canCreateLegacyBody() {
  FuncDecl *FD = dyn_cast<FuncDecl>(StartNode.dyn_cast<Decl *>());
  if (!FD) {
    return false;
  }
  if (FD == nullptr || FD->getBody() == nullptr) {
    return false;
  }
  if (FD->hasThrows()) {
    assert(!TopHandler.isValid() && "We shouldn't have found a handler desc "
                                    "if the original function throws");
    return false;
  }
  return TopHandler.isValid();
}

std::string AsyncConverter::getAsyncWrapperCompletionClosure(
    StringRef ContName, const AsyncHandlerParamDesc &HandlerDesc) {
  std::string OutputStr;
  llvm::raw_string_ostream OS(OutputStr);

  OS << tok::l_brace; // start closure

  // Prepare parameter names for the closure.
  auto SuccessParams = HandlerDesc.getSuccessParams();
  SmallVector<SmallString<4>, 2> SuccessParamNames;
  for (auto idx : indices(SuccessParams)) {
    SuccessParamNames.emplace_back("result");

    // If we have multiple success params, number them e.g res1, res2...
    if (SuccessParams.size() > 1)
      SuccessParamNames.back().append(std::to_string(idx + 1));
  }
  std::optional<SmallString<4>> ErrName;
  if (HandlerDesc.getErrorParam())
    ErrName.emplace("error");

  auto HasAnyParams = !SuccessParamNames.empty() || ErrName;
  if (HasAnyParams)
    OS << " ";

  // res1, res2
  llvm::interleave(
      SuccessParamNames, [&](auto Name) { OS << Name; },
      [&]() { OS << tok::comma << " "; });

  // , err
  if (ErrName) {
    if (!SuccessParamNames.empty())
      OS << tok::comma << " ";

    OS << *ErrName;
  }
  if (HasAnyParams)
    OS << " " << tok::kw_in;

  OS << "\n";

  // The closure body.
  switch (HandlerDesc.Type) {
  case HandlerType::PARAMS: {
    // For a (Success?, Error?) -> Void handler, we do an if let on the error.
    if (ErrName) {
      // if let err = err {
      OS << tok::kw_if << " " << tok::kw_let << " ";
      OS << *ErrName << " " << tok::equal << " " << *ErrName << " ";
      OS << tok::l_brace << "\n";
      for (auto Idx : indices(SuccessParamNames)) {
        auto ParamTy = SuccessParams[Idx].getParameterType();
        if (!HandlerDesc.shouldUnwrap(ParamTy))
          continue;
      }

      // continuation.resume(throwing: err)
      OS << ContName << tok::period << "resume" << tok::l_paren;
      OS << "throwing" << tok::colon << " " << *ErrName;
      OS << tok::r_paren << "\n";

      // return }
      OS << tok::kw_return << "\n";
      OS << tok::r_brace << "\n";
    }

    // If we have any success params that we need to unwrap, insert a guard.
    for (auto Idx : indices(SuccessParamNames)) {
      auto &Name = SuccessParamNames[Idx];
      auto ParamTy = SuccessParams[Idx].getParameterType();
      if (!HandlerDesc.shouldUnwrap(ParamTy))
        continue;

      // guard let res = res else {
      OS << tok::kw_guard << " " << tok::kw_let << " ";
      OS << Name << " " << tok::equal << " " << Name << " " << tok::kw_else;
      OS << " " << tok::l_brace << "\n";

      // fatalError(...)
      OS << "fatalError" << tok::l_paren;
      OS << "\"Expected non-nil result '" << Name << "' for nil error\"";
      OS << tok::r_paren << "\n";

      // End guard.
      OS << tok::r_brace << "\n";
    }

    // continuation.resume(returning: (res1, res2, ...))
    OS << ContName << tok::period << "resume" << tok::l_paren;
    OS << "returning" << tok::colon << " ";
    addTupleOf(SuccessParamNames, OS, [&](auto Ref) { OS << Ref; });
    OS << tok::r_paren << "\n";
    break;
  }
  case HandlerType::RESULT: {
    // continuation.resume(with: res)
    assert(SuccessParamNames.size() == 1);
    OS << ContName << tok::period << "resume" << tok::l_paren;
    OS << "with" << tok::colon << " " << SuccessParamNames[0];
    OS << tok::r_paren << "\n";
    break;
  }
  case HandlerType::INVALID:
    llvm_unreachable("Should not have an invalid handler here");
  }

  OS << tok::r_brace; // end closure
  return OutputStr;
}

CharSourceRange AsyncConverter::getPrecedingCommentRange(SourceLoc Loc) {
  auto Tokens = SF->getAllTokens();
  auto TokenIter = token_lower_bound(Tokens, Loc);
  if (TokenIter == Tokens.end() || !TokenIter->hasComment())
    return CharSourceRange();
  return TokenIter->getCommentRange();
}

SourceLoc AsyncConverter::getLocIncludingPrecedingComment(SourceLoc Loc) {
  auto CommentRange = getPrecedingCommentRange(Loc);
  if (CommentRange.isInvalid())
    return Loc;
  return CommentRange.getStart();
}

void AsyncConverter::printCommentIfNeeded(SourceLoc Loc) {
  auto CommentRange = getPrecedingCommentRange(Loc);
  if (CommentRange.isValid())
    OS << "\n" << CommentRange.str();
}

void AsyncConverter::convertNodes(const NodesToPrint &ToPrint) {
  // Sort the possible comment locs in reverse order so we can pop them as we
  // go.
  SmallVector<SourceLoc, 2> CommentLocs;
  CommentLocs.append(ToPrint.getPossibleCommentLocs().begin(),
                     ToPrint.getPossibleCommentLocs().end());
  llvm::sort(CommentLocs.begin(), CommentLocs.end(), [](auto lhs, auto rhs) {
    return lhs.getOpaquePointerValue() > rhs.getOpaquePointerValue();
  });

  // First print the nodes we've been asked to print.
  for (auto Node : ToPrint.getNodes()) {
    // If we need to print comments, do so now.
    while (!CommentLocs.empty()) {
      auto CommentLoc = CommentLocs.back().getOpaquePointerValue();
      auto NodeLoc = Node.getStartLoc().getOpaquePointerValue();
      assert(CommentLoc != NodeLoc &&
             "Added node to both comment locs and nodes to print?");

      // If the comment occurs after the node, don't print now. Wait until
      // the right node comes along.
      if (CommentLoc > NodeLoc)
        break;

      printCommentIfNeeded(CommentLocs.pop_back_val());
    }
    OS << "\n";
    convertNode(Node);
  }

  // We're done printing nodes. Make sure to output the remaining comments.
  while (!CommentLocs.empty())
    printCommentIfNeeded(CommentLocs.pop_back_val());
}

void AsyncConverter::convertNode(ASTNode Node, SourceLoc StartOverride,
                                 bool ConvertCalls,
                                 bool IncludePrecedingComment) {
  if (!StartOverride.isValid())
    StartOverride = Node.getStartLoc();

  // Make sure to include any preceding comments attached to the loc
  if (IncludePrecedingComment)
    StartOverride = getLocIncludingPrecedingComment(StartOverride);

  llvm::SaveAndRestore<SourceLoc> RestoreLoc(LastAddedLoc, StartOverride);
  llvm::SaveAndRestore<int> RestoreCount(NestedExprCount, ConvertCalls ? 0 : 1);

  walk(Node);
  addRange(LastAddedLoc, Node.getEndLoc(), /*ToEndOfToken=*/true);
}

void AsyncConverter::convertPattern(const Pattern *P) {
  // Only print semantic patterns. This cleans up the output of the transform
  // and works around some bogus source locs that can appear with typed
  // patterns in if let statements.
  P = P->getSemanticsProvidingPattern();

  // Set up the start of the pattern as the last loc printed to make sure we
  // accurately fill in the gaps as we customize the printing of sub-patterns.
  llvm::SaveAndRestore<SourceLoc> RestoreLoc(LastAddedLoc, P->getStartLoc());
  llvm::SaveAndRestore<bool> RestoreFlag(ConvertingPattern, true);

  walk(const_cast<Pattern *>(P));
  addRange(LastAddedLoc, P->getEndLoc(), /*ToEndOfToken*/ true);
}

void AsyncConverter::wrapScopeInContinationIfNecessary(ASTNode Node) {
  if (NestedExprCount != 0) {
    // We can't start a continuation in the middle of an expression
    return;
  }
  if (Scopes.back().isWrappedInContination()) {
    // We are already in a continuation. No need to add another one.
    return;
  }
  if (!DeclReferenceFinder::containsReference(Node, TopHandler.getHandler())) {
    // The node doesn't have a reference to the function's completion handler.
    // It can stay a call with a completion handler, because we don't need to
    // promote a completion handler call to a 'return'.
    return;
  }

  // Wrap the current call in a continuation

  Identifier contName = createUniqueName("continuation");
  Scopes.back().Names.insert(contName);
  Scopes.back().ContinuationName = contName;

  insertCustom(Node.getStartLoc(), [&]() {
    OS << tok::kw_return << ' ';
    if (TopHandler.HasError) {
      OS << tok::kw_try << ' ';
    }
    OS << "await ";
    if (TopHandler.HasError) {
      OS << "withCheckedThrowingContinuation ";
    } else {
      OS << "withCheckedContinuation ";
    }
    OS << tok::l_brace << ' ' << contName << ' ' << tok::kw_in << '\n';
  });
}

bool AsyncConverter::walkToPatternPre(Pattern *P) {
  // If we're not converting a pattern, there's nothing extra to do.
  if (!ConvertingPattern)
    return true;

  // When converting a pattern, don't print the 'let' or 'var' of binding
  // subpatterns, as they're illegal when nested in PBDs, and we print a
  // top-level one.
  if (auto *BP = dyn_cast<BindingPattern>(P)) {
    return addCustom(BP->getSourceRange(),
                     [&]() { convertPattern(BP->getSubPattern()); });
  }
  return true;
}

bool AsyncConverter::walkToDeclPre(Decl *D, CharSourceRange Range) {
  if (isa<PatternBindingDecl>(D)) {
    // We can't hoist a closure inside a PatternBindingDecl. If it contains
    // a call to the completion handler, wrap it in a continuation.
    wrapScopeInContinationIfNecessary(D);
    NestedExprCount++;
    return true;
  }

  // Functions and types already have their names in \c Scopes.Names, only
  // variables should need to be renamed.
  if (isa<VarDecl>(D)) {
    // If we don't already have a name for the var, assign it one. Note that
    // vars in binding patterns may already have assigned names here.
    if (Names.find(D) == Names.end()) {
      auto Ident = assignUniqueName(D, StringRef());
      Scopes.back().Names.insert(Ident);
    }
    addCustom(D->getSourceRange(), [&]() { OS << newNameFor(D); });
  }

  // Note we don't walk into any nested local function decls. If we start
  // doing so in the future, be sure to update the logic that deals with
  // converting unhandled returns into placeholders in walkToStmtPre.
  return false;
}

bool AsyncConverter::walkToDeclPost(Decl *D) {
  NestedExprCount--;
  return true;
}

#define PLACEHOLDER_START "<#"
#define PLACEHOLDER_END "#>"
bool AsyncConverter::walkToExprPre(Expr *E) {
  // TODO: Handle Result.get as well
  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *D = DRE->getDecl()) {
      // Look through to the parent var decl if we have one. This ensures we
      // look at the var in a case stmt's pattern rather than the var that's
      // implicitly declared in the body.
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (auto *Parent = VD->getParentVarDecl())
          D = Parent;
      }

      bool AddPlaceholder = Placeholders.count(D);
      StringRef Name = newNameFor(D, false);
      if (AddPlaceholder || !Name.empty())
        return addCustom(DRE->getSourceRange(), [&]() {
          if (AddPlaceholder)
            OS << PLACEHOLDER_START;
          if (!Name.empty())
            OS << Name;
          else
            D->getName().print(OS);
          if (AddPlaceholder)
            OS << PLACEHOLDER_END;
        });
    }
  } else if (isa<ForceValueExpr>(E) || isa<BindOptionalExpr>(E)) {
    // Remove a force unwrap or optional chain of a returned success value,
    // as it will no longer be optional. For force unwraps, this is always a
    // valid transform. For optional chains, it is a locally valid transform
    // within the optional chain e.g foo?.x -> foo.x, but may change the type
    // of the overall chain, which could cause errors elsewhere in the code.
    // However this is generally more useful to the user than just leaving
    // 'foo' as a placeholder. Note this is only the case when no other
    // optionals are involved in the chain, e.g foo?.x?.y -> foo.x?.y is
    // completely valid.
    if (auto *D = E->getReferencedDecl().getDecl()) {
      if (Unwraps.count(D))
        return addCustom(E->getSourceRange(),
                         [&]() { OS << newNameFor(D, true); });
    }
  } else if (CallExpr *CE = TopHandler.getAsHandlerCall(E)) {
    if (Scopes.back().isWrappedInContination()) {
      return addCustom(E->getSourceRange(),
                       [&]() { convertHandlerToContinuationResume(CE); });
    } else if (NestedExprCount == 0) {
      return addCustom(E->getSourceRange(),
                       [&]() { convertHandlerToReturnOrThrows(CE); });
    }
  } else if (auto *CE = dyn_cast<CallExpr>(E)) {
    // Try and hoist a call's completion handler. Don't do so if
    //  - the current expression is nested (we can't start hoisting in the
    //    middle of an expression)
    //  - the current scope is wrapped in a continuation (we can't have await
    //    calls in the continuation block)
    if (NestedExprCount == 0 && !Scopes.back().isWrappedInContination()) {
      // If the refactoring is on the call itself, do not require the callee
      // to have the @available attribute or a completion-like name.
      auto HandlerDesc = AsyncHandlerParamDesc::find(
          getUnderlyingFunc(CE->getFn()),
          /*RequireAttributeOrName=*/StartNode.dyn_cast<Expr *>() != CE);
      if (HandlerDesc.isValid()) {
        return addCustom(CE->getSourceRange(),
                         [&]() { addHoistedCallback(CE, HandlerDesc); });
      }
    }
  }

  // A void SingleValueStmtExpr is semantically more like a statement than
  // an expression, so recurse without bumping the expr depth or wrapping in
  // continuation.
  if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E)) {
    auto ty = SVE->getType();
    if (!ty || ty->isVoid())
      return true;
  }

  // We didn't do any special conversion for this expression. If needed, wrap
  // it in a continuation.
  wrapScopeInContinationIfNecessary(E);

  NestedExprCount++;
  return true;
}

bool AsyncConverter::replaceRangeWithPlaceholder(SourceRange range) {
  return addCustom(range, [&]() {
    OS << PLACEHOLDER_START;
    addRange(range, /*toEndOfToken*/ true);
    OS << PLACEHOLDER_END;
  });
}

bool AsyncConverter::walkToExprPost(Expr *E) {
  if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E)) {
    auto ty = SVE->getType();
    if (!ty || ty->isVoid())
      return true;
  }
  NestedExprCount--;
  return true;
}

#undef PLACEHOLDER_START
#undef PLACEHOLDER_END

bool AsyncConverter::walkToStmtPre(Stmt *S) {
  // CaseStmt has an implicit BraceStmt inside it, which *should* start a new
  // scope, so don't check isImplicit here.
  if (startsNewScope(S)) {
    // Add all names of decls referenced within this statement that aren't
    // also declared first, plus any contexts. Note that \c getReferencedDecl
    // will only return a value for a \c BraceStmt. This means that \c IfStmt
    // (and other statements with conditions) will have their own empty scope,
    // which is fine for our purposes - their existing names are always valid.
    // The body of those statements will include the decls if they've been
    // referenced, so shadowing is still avoided there.
    if (auto *ReferencedDecls = ScopedDecls.getReferencedDecls(S)) {
      llvm::DenseSet<const Decl *> Decls;
      for (auto DeclAndNumRefs : *ReferencedDecls)
        Decls.insert(DeclAndNumRefs.first);
      addNewScope(Decls);
    } else {
      addNewScope({});
    }
  } else if (Hoisting && !S->isImplicit()) {
    // Some break and return statements need to be turned into placeholders,
    // as they may no longer perform the control flow that the user is
    // expecting.
    if (auto *BS = dyn_cast<BreakStmt>(S)) {
      // For a break, if it's jumping out of a switch statement that we've
      // re-written as a part of the transform, turn it into a placeholder, as
      // it would have been lifted out of the switch statement.
      if (auto *SS = dyn_cast<SwitchStmt>(BS->getTarget())) {
        if (HandledSwitches.contains(SS))
          return replaceRangeWithPlaceholder(S->getSourceRange());
      }
    } else if (isa<ReturnStmt>(S) && NestedExprCount == 0) {
      // For a return, if it's not nested inside another closure or function,
      // turn it into a placeholder, as it will be lifted out of the callback.
      // Note that we only turn the 'return' token into a placeholder as we
      // still want to be able to apply transforms to the argument.
      replaceRangeWithPlaceholder(S->getStartLoc());
    }
  }
  return true;
}

bool AsyncConverter::walkToStmtPost(Stmt *S) {
  if (startsNewScope(S)) {
    bool ClosedScopeWasWrappedInContinuation =
        Scopes.back().isWrappedInContination();
    Scopes.pop_back();
    if (ClosedScopeWasWrappedInContinuation &&
        !Scopes.back().isWrappedInContination()) {
      // The nested scope was wrapped in a continuation but the current one
      // isn't anymore. Add the '}' that corresponds to the call to
      // withChecked(Throwing)Continuation.
      insertCustom(S->getEndLoc(), [&]() { OS << tok::r_brace << '\n'; });
    }
  }
  return true;
}

bool AsyncConverter::addCustom(SourceRange Range,
                               llvm::function_ref<void()> Custom) {
  addRange(LastAddedLoc, Range.Start);
  Custom();
  LastAddedLoc = Lexer::getLocForEndOfToken(SM, Range.End);
  return false;
}

bool AsyncConverter::insertCustom(SourceLoc Loc,
                                  llvm::function_ref<void()> Custom) {
  addRange(LastAddedLoc, Loc);
  Custom();
  LastAddedLoc = Loc;
  return false;
}

void AsyncConverter::addRange(SourceLoc Start, SourceLoc End,
                              bool ToEndOfToken) {
  if (ToEndOfToken) {
    OS << Lexer::getCharSourceRangeFromSourceRange(SM, SourceRange(Start, End))
              .str();
  } else {
    OS << CharSourceRange(SM, Start, End).str();
  }
}

void AsyncConverter::addRange(SourceRange Range, bool ToEndOfToken) {
  addRange(Range.Start, Range.End, ToEndOfToken);
}

void AsyncConverter::addFuncDecl(const FuncDecl *FD) {
  auto *Params = FD->getParameters();
  auto *HandlerParam = TopHandler.getHandlerParam();
  auto ParamPos = TopHandler.handlerParamPosition();

  // If the completion handler parameter has a default argument, the async
  // version is effectively @discardableResult, as not all the callers care
  // about receiving the completion call.
  if (HandlerParam && HandlerParam->isDefaultArgument())
    OS << tok::at_sign << "discardableResult"
       << "\n";

  // First chunk: start -> the parameter to remove (if any)
  SourceLoc LeftEndLoc;
  switch (ParamPos) {
  case AsyncHandlerParamDesc::Position::None:
  case AsyncHandlerParamDesc::Position::Only:
  case AsyncHandlerParamDesc::Position::First:
    // Handler is the first param (or there is none), so only include the (
    LeftEndLoc = Params->getLParenLoc().getAdvancedLoc(1);
    break;
  case AsyncHandlerParamDesc::Position::Middle:
    // Handler is somewhere in the middle of the params, so we need to
    // include any comments and comma up until the handler
    LeftEndLoc = Params->get(TopHandler.Index)->getStartLoc();
    LeftEndLoc = getLocIncludingPrecedingComment(LeftEndLoc);
    break;
  case AsyncHandlerParamDesc::Position::Last:
    // Handler is the last param, which means we don't want the comma. This
    // is a little annoying since we *do* want the comments past for the
    // last parameter
    LeftEndLoc = Lexer::getLocForEndOfToken(
        SM, Params->get(TopHandler.Index - 1)->getEndLoc());
    // Skip to the end of any comments
    Token Next =
        Lexer::getTokenAtLocation(SM, LeftEndLoc, CommentRetentionMode::None);
    if (Next.getKind() != tok::NUM_TOKENS)
      LeftEndLoc = Next.getLoc();
    break;
  }
  addRange(FD->getSourceRangeIncludingAttrs().Start, LeftEndLoc);

  // Second chunk: end of the parameter to remove -> right parenthesis
  SourceLoc MidStartLoc;
  SourceLoc MidEndLoc = Params->getRParenLoc().getAdvancedLoc(1);
  switch (ParamPos) {
  case AsyncHandlerParamDesc::Position::None:
    // No handler param, so make sure to include them all
    MidStartLoc = LeftEndLoc;
    break;
  case AsyncHandlerParamDesc::Position::First:
  case AsyncHandlerParamDesc::Position::Middle:
    // Handler param is either the first or one of the middle params. Skip
    // past it but make sure to include comments preceding the param after
    // the handler
    MidStartLoc = Params->get(TopHandler.Index + 1)->getStartLoc();
    MidStartLoc = getLocIncludingPrecedingComment(MidStartLoc);
    break;
  case AsyncHandlerParamDesc::Position::Only:
  case AsyncHandlerParamDesc::Position::Last:
    // Handler param is last, this is easy since there's no other params
    // to copy over
    MidStartLoc = Params->getRParenLoc();
    break;
  }
  addRange(MidStartLoc, MidEndLoc);

  // Third chunk: add in async and throws if necessary
  if (!FD->hasAsync())
    OS << " async";
  if (FD->hasThrows() || TopHandler.HasError)
    // TODO: Add throws if converting a function and it has a converted call
    //       without a do/catch
    OS << " " << tok::kw_throws;

  // Fourth chunk: if no parent handler (ie. not adding an async
  // alternative), the rest of the decl. Otherwise, add in the new return
  // type
  if (!TopHandler.isValid()) {
    SourceLoc RightStartLoc = MidEndLoc;
    if (FD->hasThrows()) {
      RightStartLoc = Lexer::getLocForEndOfToken(SM, FD->getThrowsLoc());
    }
    SourceLoc RightEndLoc =
        FD->getBody() ? FD->getBody()->getLBraceLoc() : RightStartLoc;
    addRange(RightStartLoc, RightEndLoc);
    return;
  }

  SmallVector<LabeledReturnType, 2> Scratch;
  auto ReturnTypes = TopHandler.getAsyncReturnTypes(Scratch);
  if (ReturnTypes.empty()) {
    OS << " ";
    return;
  }

  // Print the function result type, making sure to omit a '-> Void' return.
  if (!TopHandler.willAsyncReturnVoid()) {
    OS << " -> ";
    addAsyncFuncReturnType(TopHandler);
  }

  if (FD->hasBody())
    OS << " ";

  // TODO: Should remove the generic param and where clause for the error
  //       param if it exists (and no other parameter uses that type)
  TrailingWhereClause *TWC = FD->getTrailingWhereClause();
  if (TWC && TWC->getWhereLoc().isValid()) {
    auto Range = TWC->getSourceRange();
    OS << Lexer::getCharSourceRangeFromSourceRange(SM, Range).str();
    if (FD->hasBody())
      OS << " ";
  }
}

void AsyncConverter::addFallbackVars(ArrayRef<const ParamDecl *> FallbackParams,
                                     const ClosureCallbackParams &AllParams) {
  for (auto *Param : FallbackParams) {
    auto Ty = Param->getTypeInContext();
    auto ParamName = newNameFor(Param);

    // If this is the known bool success param, we can use 'let' and type it
    // as non-optional, as it gets bound in both blocks.
    if (AllParams.isKnownBoolFlagParam(Param)) {
      OS << tok::kw_let << " " << ParamName << ": ";
      Ty->print(OS);
      OS << "\n";
      continue;
    }

    OS << tok::kw_var << " " << ParamName << ": ";
    Ty->print(OS);
    if (!Ty->getOptionalObjectType())
      OS << "?";

    OS << " = " << tok::kw_nil << "\n";
  }
}

void AsyncConverter::addDo() {
  OS << tok::kw_do << " " << tok::l_brace << "\n";
}

bool AsyncConverter::isErrorAlreadyHandled(HandlerResult Result) {
  assert(Result.isError());
  assert(Result.args().size() == 1 &&
         "There should only be one error parameter");
  // We assume that the error has already been handled if its variable
  // declaration doesn't exist anymore, which is the case if it's in
  // Placeholders but not in Unwraps (if it's in Placeholders and Unwraps
  // an optional Error has simply been promoted to a non-optional Error).
  if (auto *DRE = dyn_cast<DeclRefExpr>(Result.args().back().getExpr())) {
    if (Placeholders.count(DRE->getDecl()) && !Unwraps.count(DRE->getDecl())) {
      return true;
    }
  }
  return false;
}

bool AsyncConverter::isExpressionOptional(Expr *E) {
  if (isa<InjectIntoOptionalExpr>(E)) {
    // E is downgrading a non-Optional result to an Optional. Its source
    // representation isn't Optional.
    return false;
  }
  if (auto DRE = dyn_cast<DeclRefExpr>(E)) {
    if (Unwraps.count(DRE->getDecl())) {
      // E has been promoted to a non-Optional value. It can't be used as an
      // Optional anymore.
      return false;
    }
  }
  if (!E->getType().isNull() && E->getType()->isOptional()) {
    return true;
  }
  // We couldn't determine the type. Assume non-Optional.
  return false;
}

void AsyncConverter::convertHandlerCall(
    const CallExpr *CE,
    llvm::function_ref<void(HandlerResult)> AddConvertedHandlerCall,
    llvm::function_ref<void(StringRef)> AddConvertedErrorCall) {
  auto Result =
      TopHandler.extractResultArgs(CE, /*ReturnErrorArgsIfAmbiguous=*/true);
  if (!TopHandler.isAmbiguousCallToParamHandler(CE)) {
    if (Result.isError()) {
      if (!isErrorAlreadyHandled(Result)) {
        // If the error has already been handled, we don't need to add another
        // throwing call.
        AddConvertedHandlerCall(Result);
      }
    } else {
      AddConvertedHandlerCall(Result);
    }
  } else {
    assert(Result.isError() && "If the call was ambiguous, we should have "
                               "retrieved its error representation");
    assert(Result.args().size() == 1 &&
           "There should only be one error parameter");
    Expr *ErrorExpr = Result.args().back().getExpr();
    if (isErrorAlreadyHandled(Result)) {
      // The error has already been handled, interpret the call as a success
      // call.
      auto SuccessExprs = TopHandler.extractResultArgs(
          CE, /*ReturnErrorArgsIfAmbiguous=*/false);
      AddConvertedHandlerCall(SuccessExprs);
    } else if (!isExpressionOptional(ErrorExpr)) {
      // The error is never nil. No matter what the success param is, we
      // interpret it as an error call.
      AddConvertedHandlerCall(Result);
    } else {
      // The call was truly ambiguous. Add an
      // if let error = <convert error arg> {
      //   throw error // or equivalent
      // } else {
      //   <interpret call as success call>
      // }
      auto SuccessExprs = TopHandler.extractResultArgs(
          CE, /*ReturnErrorArgsIfAmbiguous=*/false);

      // The variable 'error' is only available in the 'if let' scope, so we
      // don't need to create a new unique one.
      StringRef ErrorName = "error";
      OS << tok::kw_if << ' ' << tok::kw_let << ' ' << ErrorName << ' '
         << tok::equal << ' ';
      convertNode(ErrorExpr, /*StartOverride=*/{}, /*ConvertCalls=*/false);
      OS << ' ' << tok::l_brace << '\n';
      AddConvertedErrorCall(ErrorName);
      OS << tok::r_brace << ' ' << tok::kw_else << ' ' << tok::l_brace << '\n';
      AddConvertedHandlerCall(SuccessExprs);
      OS << '\n' << tok::r_brace;
    }
  }
}

void AsyncConverter::convertHandlerToReturnOrThrows(const CallExpr *CE) {
  return convertHandlerCall(
      CE,
      [&](HandlerResult Exprs) {
        convertHandlerToReturnOrThrowsImpl(CE, Exprs);
      },
      [&](StringRef ErrorName) {
        OS << tok::kw_throw << ' ' << ErrorName << '\n';
      });
}

void AsyncConverter::convertHandlerToReturnOrThrowsImpl(const CallExpr *CE,
                                                        HandlerResult Result) {
  bool AddedReturnOrThrow = true;
  if (!Result.isError()) {
    // It's possible the user has already written an explicit return statement
    // for the completion handler call, e.g 'return completion(args...)'. In
    // that case, be sure not to add another return.
    auto *parent = getWalker().Parent.getAsStmt();
    if (isa_and_nonnull<ReturnStmt>(parent) &&
        !cast<ReturnStmt>(parent)->isImplicit()) {
      // The statement already has a return keyword. Don't add another one.
      AddedReturnOrThrow = false;
    } else {
      OS << tok::kw_return;
    }
  } else {
    OS << tok::kw_throw;
  }

  auto Args = Result.args();
  if (!Args.empty()) {
    if (AddedReturnOrThrow)
      OS << ' ';

    addTupleOf(Args, OS, [&](Argument Arg) {
      // Special case: If the completion handler is a params handler that
      // takes an error, we could pass arguments to it without unwrapping
      // them. E.g.
      //   simpleWithError { (res: String?, error: Error?) in
      //     completion(res, nil)
      //   }
      // But after refactoring `simpleWithError` to an async function we have
      //   let res: String = await simple()
      // and `res` is no longer an `Optional`. Thus it's in `Placeholders` and
      // `Unwraps` and any reference to it will be replaced by a placeholder
      // unless it is wrapped in an unwrapping expression. This would cause us
      // to create `return <#res# >`.
      // Under our assumption that either the error or the result parameter
      // are non-nil, the above call to the completion handler is equivalent
      // to
      //   completion(res!, nil)
      // which correctly yields
      //   return res
      // Synthesize the force unwrap so that we get the expected results.
      auto *E = Arg.getExpr();
      if (TopHandler.getHandlerType() == HandlerType::PARAMS &&
          TopHandler.HasError) {
        if (auto DRE = dyn_cast<DeclRefExpr>(E->getSemanticsProvidingExpr())) {
          auto D = DRE->getDecl();
          if (Unwraps.count(D)) {
            E = new (getASTContext()) ForceValueExpr(E, SourceLoc());
          }
        }
      }
      // Can't just add the range as we need to perform replacements
      convertNode(E, /*StartOverride=*/Arg.getLabelLoc(),
                  /*ConvertCalls=*/false);
    });
  }
}

void AsyncConverter::convertHandlerToContinuationResume(const CallExpr *CE) {
  return convertHandlerCall(
      CE,
      [&](HandlerResult Exprs) {
        convertHandlerToContinuationResumeImpl(CE, Exprs);
      },
      [&](StringRef ErrorName) {
        Identifier ContinuationName = Scopes.back().ContinuationName;
        OS << ContinuationName << tok::period << "resume" << tok::l_paren
           << "throwing" << tok::colon << ' ' << ErrorName;
        OS << tok::r_paren << '\n';
      });
}

void AsyncConverter::convertHandlerToContinuationResumeImpl(
    const CallExpr *CE, HandlerResult Result) {
  assert(Scopes.back().isWrappedInContination());

  std::vector<Argument> Args;
  StringRef ResumeArgumentLabel;
  switch (TopHandler.getHandlerType()) {
  case HandlerType::PARAMS: {
    Args = Result.args();
    if (!Result.isError()) {
      ResumeArgumentLabel = "returning";
    } else {
      ResumeArgumentLabel = "throwing";
    }
    break;
  }
  case HandlerType::RESULT: {
    Args = {CE->getArgs()->begin(), CE->getArgs()->end()};
    ResumeArgumentLabel = "with";
    break;
  }
  case HandlerType::INVALID:
    llvm_unreachable("Invalid top handler");
  }

  // A vector in which each argument of Result has an entry. If the entry is
  // not empty then that argument has been unwrapped using 'guard let' into
  // a variable with that name.
  SmallVector<Identifier, 4> ArgNames;
  ArgNames.reserve(Args.size());

  /// When unwrapping a result argument \p Arg into a variable using
  /// 'guard let' return a suitable name for the unwrapped variable.
  /// \p ArgIndex is the index of \p Arg in the results passed to the
  /// completion handler.
  auto GetSuitableNameForGuardUnwrap = [&](Expr *Arg,
                                           unsigned ArgIndex) -> Identifier {
    // If Arg is a DeclRef, use its name for the guard unwrap.
    // guard let myVar1 = myVar.
    if (auto DRE = dyn_cast<DeclRefExpr>(Arg)) {
      return createUniqueName(DRE->getDecl()->getBaseIdentifier().str());
    } else if (auto IIOE = dyn_cast<InjectIntoOptionalExpr>(Arg)) {
      if (auto DRE = dyn_cast<DeclRefExpr>(IIOE->getSubExpr())) {
        return createUniqueName(DRE->getDecl()->getBaseIdentifier().str());
      }
    }
    if (Args.size() == 1) {
      // We only have a single result. 'result' seems a resonable name.
      return createUniqueName("result");
    } else {
      // We are returning a tuple. Name the result elements 'result' +
      // index in tuple.
      return createUniqueName("result" + std::to_string(ArgIndex));
    }
  };

  unsigned ArgIndex = 0;
  for (auto Arg : Args) {
    auto *ArgExpr = Arg.getExpr();
    Identifier ArgName;
    if (isExpressionOptional(ArgExpr) && TopHandler.HasError) {
      ArgName = GetSuitableNameForGuardUnwrap(ArgExpr, ArgIndex);
      Scopes.back().Names.insert(ArgName);
      OS << tok::kw_guard << ' ' << tok::kw_let << ' ' << ArgName << ' '
         << tok::equal << ' ';

      // If the argument is a call with a trailing closure, the generated
      // guard statement will not compile.
      // e.g. 'guard let result1 = value.map { $0 + 1 } else { ... }'
      // doesn't compile. Adding parentheses makes the code compile.
      auto HasTrailingClosure = false;
      if (auto *CE = dyn_cast<CallExpr>(ArgExpr)) {
        if (CE->getArgs()->hasAnyTrailingClosures())
          HasTrailingClosure = true;
      }

      if (HasTrailingClosure)
        OS << tok::l_paren;

      convertNode(ArgExpr, /*StartOverride=*/Arg.getLabelLoc(),
                  /*ConvertCalls=*/false);

      if (HasTrailingClosure)
        OS << tok::r_paren;

      OS << ' ' << tok::kw_else << ' ' << tok::l_brace << '\n';
      OS << "fatalError" << tok::l_paren;
      OS << "\"Expected non-nil result ";
      if (ArgName.str() != "result") {
        OS << "'" << ArgName << "' ";
      }
      OS << "in the non-error case\"";
      OS << tok::r_paren << '\n';
      OS << tok::r_brace << '\n';
    }
    ArgNames.push_back(ArgName);
    ArgIndex++;
  }

  Identifier ContName = Scopes.back().ContinuationName;
  OS << ContName << tok::period << "resume" << tok::l_paren
     << ResumeArgumentLabel << tok::colon << ' ';

  ArgIndex = 0;
  addTupleOf(Args, OS, [&](Argument Arg) {
    Identifier ArgName = ArgNames[ArgIndex];
    if (!ArgName.empty()) {
      OS << ArgName;
    } else {
      // Can't just add the range as we need to perform replacements
      convertNode(Arg.getExpr(), /*StartOverride=*/Arg.getLabelLoc(),
                  /*ConvertCalls=*/false);
    }
    ArgIndex++;
  });
  OS << tok::r_paren;
}

ClosureExpr *AsyncConverter::extractCallback(Expr *E) {
  E = lookThroughFunctionConversionExpr(E);
  if (auto Closure = dyn_cast<ClosureExpr>(E)) {
    return Closure;
  } else if (auto CaptureList = dyn_cast<CaptureListExpr>(E)) {
    return dyn_cast<ClosureExpr>(CaptureList->getClosureBody());
  } else {
    return nullptr;
  }
}

Expr *AsyncConverter::lookThroughFunctionConversionExpr(Expr *E) {
  if (auto FunctionConversion = dyn_cast<FunctionConversionExpr>(E)) {
    return lookThroughFunctionConversionExpr(FunctionConversion->getSubExpr());
  } else {
    return E;
  }
}

void AsyncConverter::addHoistedCallback(
    const CallExpr *CE, const AsyncHandlerParamDesc &HandlerDesc) {
  llvm::SaveAndRestore<bool> RestoreHoisting(Hoisting, true);

  auto *ArgList = CE->getArgs();
  if (HandlerDesc.Index >= ArgList->size()) {
    DiagEngine.diagnose(CE->getStartLoc(), diag::missing_callback_arg);
    return;
  }

  Expr *CallbackArg =
      lookThroughFunctionConversionExpr(ArgList->getExpr(HandlerDesc.Index));
  if (ClosureExpr *Callback = extractCallback(CallbackArg)) {
    // The user is using a closure for the completion handler
    addHoistedClosureCallback(CE, HandlerDesc, Callback);
    return;
  }
  if (auto CallbackDecl =
          getReferencedDeclLookingThroughAutoclosures(CallbackArg)) {
    if (CallbackDecl == TopHandler.getHandler()) {
      // We are refactoring the function that declared the completion handler
      // that would be called here. We can't call the completion handler
      // anymore because it will be removed. But since the function that
      // declared it is being refactored to async, we can just return the
      // values.
      if (!HandlerDesc.willAsyncReturnVoid()) {
        OS << tok::kw_return << " ";
      }
      InlinePatternsToPrint InlinePatterns;
      addAwaitCall(CE, ClassifiedBlock(), {}, InlinePatterns, HandlerDesc,
                   /*AddDeclarations*/ false);
      return;
    }
    // We are not removing the completion handler, so we can call it once the
    // async function returns.

    // The completion handler that is called as part of the \p CE call.
    // This will be called once the async function returns.
    auto CompletionHandler =
        AsyncHandlerDesc::get(CallbackDecl, /*RequireAttributeOrName=*/false);
    if (CompletionHandler.isValid()) {
      if (auto CalledFunc = getUnderlyingFunc(CE->getFn())) {
        StringRef HandlerName = Lexer::getCharSourceRangeFromSourceRange(
                                    SM, CallbackArg->getSourceRange())
                                    .str();
        addHoistedNamedCallback(
            CalledFunc, CompletionHandler, HandlerName, [&] {
              InlinePatternsToPrint InlinePatterns;
              addAwaitCall(CE, ClassifiedBlock(), {}, InlinePatterns,
                           HandlerDesc, /*AddDeclarations*/ false);
            });
        return;
      }
    }
  }
  DiagEngine.diagnose(CE->getStartLoc(), diag::missing_callback_arg);
}

void AsyncConverter::addBoolFlagParamBindingIfNeeded(
    std::optional<KnownBoolFlagParam> Flag, BlockKind Block) {
  if (!Flag)
    return;
  // Figure out the polarity of the binding based on the block we're in and
  // whether the flag indicates success.
  auto Polarity = true;
  switch (Block) {
  case BlockKind::SUCCESS:
    break;
  case BlockKind::ERROR:
    Polarity = !Polarity;
    break;
  case BlockKind::FALLBACK:
    llvm_unreachable("Not a valid place to bind");
  }
  if (!Flag->IsSuccessFlag)
    Polarity = !Polarity;

  OS << newNameFor(Flag->Param) << " " << tok::equal << " ";
  OS << (Polarity ? tok::kw_true : tok::kw_false) << "\n";
}

void AsyncConverter::addHoistedClosureCallback(
    const CallExpr *CE, const AsyncHandlerParamDesc &HandlerDesc,
    const ClosureExpr *Callback) {
  if (HandlerDesc.params().size() != Callback->getParameters()->size()) {
    DiagEngine.diagnose(CE->getStartLoc(), diag::mismatched_callback_args);
    return;
  }
  ClosureCallbackParams CallbackParams(HandlerDesc, Callback);
  ClassifiedBlocks Blocks;
  auto *CallbackBody = Callback->getBody();
  if (!HandlerDesc.HasError) {
    Blocks.SuccessBlock.addNodesInBraceStmt(CallbackBody);
  } else if (!CallbackBody->getElements().empty()) {
    CallbackClassifier::classifyInto(Blocks, CallbackParams, HandledSwitches,
                                     DiagEngine, CallbackBody);
  }

  auto SuccessBindings = CallbackParams.getParamsToBind(BlockKind::SUCCESS);
  auto *ErrParam = CallbackParams.getErrParam();
  if (DiagEngine.hadAnyError()) {
    // For now, only fallback when the results are params with an error param,
    // in which case only the names are used (defaulted to the names of the
    // params if none).
    if (HandlerDesc.Type != HandlerType::PARAMS || !HandlerDesc.HasError)
      return;
    DiagEngine.resetHadAnyError();

    // Note that we don't print any inline patterns here as we just want
    // assignments to the names in the outer scope.
    InlinePatternsToPrint InlinePatterns;

    auto AllBindings = CallbackParams.getParamsToBind(BlockKind::FALLBACK);

    prepareNames(ClassifiedBlock(), AllBindings, InlinePatterns);
    preparePlaceholdersAndUnwraps(HandlerDesc, CallbackParams,
                                  BlockKind::FALLBACK);
    addFallbackVars(AllBindings, CallbackParams);
    addDo();
    addAwaitCall(CE, Blocks.SuccessBlock, SuccessBindings, InlinePatterns,
                 HandlerDesc, /*AddDeclarations*/ false);
    OS << "\n";

    // If we have a known Bool success param, we need to bind it.
    addBoolFlagParamBindingIfNeeded(CallbackParams.getKnownBoolFlagParam(),
                                    BlockKind::SUCCESS);
    addFallbackCatch(CallbackParams);
    OS << "\n";
    convertNodes(NodesToPrint::inBraceStmt(CallbackBody));

    clearNames(AllBindings);
    return;
  }

  auto *ErrOrResultParam = ErrParam;
  if (auto *ResultParam = CallbackParams.getResultParam())
    ErrOrResultParam = ResultParam;

  auto ErrorNodes = Blocks.ErrorBlock.nodesToPrint().getNodes();
  bool RequireDo = !ErrorNodes.empty();
  // Check if we *actually* need a do/catch (see class comment)
  if (ErrorNodes.size() == 1) {
    auto Node = ErrorNodes[0];
    if (auto *HandlerCall = TopHandler.getAsHandlerCall(Node)) {
      auto Res = TopHandler.extractResultArgs(
          HandlerCall, /*ReturnErrorArgsIfAmbiguous=*/true);
      if (Res.args().size() == 1) {
        // Skip if we have the param itself or the name it's bound to
        auto *ArgExpr = Res.args()[0].getExpr();
        auto *SingleDecl = ArgExpr->getReferencedDecl().getDecl();
        auto ErrName = Blocks.ErrorBlock.boundName(ErrOrResultParam);
        RequireDo = SingleDecl != ErrOrResultParam &&
                    !(Res.isError() && SingleDecl &&
                      SingleDecl->getName().isSimpleName(ErrName));
      }
    }
  }

  // If we're not requiring a 'do', we'll be dropping the error block. But
  // let's make sure we at least preserve the comments in the error block by
  // transplanting them into the success block. This should make sure they
  // maintain a sensible ordering.
  if (!RequireDo) {
    auto ErrorNodes = Blocks.ErrorBlock.nodesToPrint();
    for (auto CommentLoc : ErrorNodes.getPossibleCommentLocs())
      Blocks.SuccessBlock.addPossibleCommentLoc(CommentLoc);
  }

  if (RequireDo) {
    addDo();
  }

  auto InlinePatterns =
      getInlinePatternsToPrint(Blocks.SuccessBlock, SuccessBindings, Callback);

  prepareNames(Blocks.SuccessBlock, SuccessBindings, InlinePatterns);
  preparePlaceholdersAndUnwraps(HandlerDesc, CallbackParams,
                                BlockKind::SUCCESS);

  addAwaitCall(CE, Blocks.SuccessBlock, SuccessBindings, InlinePatterns,
               HandlerDesc, /*AddDeclarations=*/true);
  printOutOfLineBindingPatterns(Blocks.SuccessBlock, InlinePatterns);
  convertNodes(Blocks.SuccessBlock.nodesToPrint());
  clearNames(SuccessBindings);

  if (RequireDo) {
    // We don't use inline patterns for the error path.
    InlinePatternsToPrint ErrInlinePatterns;

    // Always use the ErrParam name if none is bound.
    prepareNames(Blocks.ErrorBlock, llvm::ArrayRef(ErrOrResultParam),
                 ErrInlinePatterns,
                 /*AddIfMissing=*/HandlerDesc.Type != HandlerType::RESULT);
    preparePlaceholdersAndUnwraps(HandlerDesc, CallbackParams,
                                  BlockKind::ERROR);

    addCatch(ErrOrResultParam);
    convertNodes(Blocks.ErrorBlock.nodesToPrint());
    OS << "\n" << tok::r_brace;
    clearNames(llvm::ArrayRef(ErrOrResultParam));
  }
}

void AsyncConverter::addHoistedNamedCallback(
    const FuncDecl *FD, const AsyncHandlerDesc &HandlerDesc,
    StringRef HandlerName, std::function<void(void)> AddAwaitCall) {
  if (HandlerDesc.HasError) {
    // "result" and "error" always okay to use here since they're added
    // in their own scope, which only contains new code.
    addDo();
    if (!HandlerDesc.willAsyncReturnVoid()) {
      OS << tok::kw_let << " result";
      addResultTypeAnnotationIfNecessary(FD, HandlerDesc);
      OS << " " << tok::equal << " ";
    }
    AddAwaitCall();
    OS << "\n";
    addCallToCompletionHandler("result", HandlerDesc, HandlerName);
    OS << "\n";
    OS << tok::r_brace << " " << tok::kw_catch << " " << tok::l_brace << "\n";
    addCallToCompletionHandler(StringRef(), HandlerDesc, HandlerName);
    OS << "\n" << tok::r_brace; // end catch
  } else {
    // This code may be placed into an existing scope, in that case create
    // a unique "result" name so that it doesn't cause shadowing or redecls.
    StringRef ResultName;
    if (!HandlerDesc.willAsyncReturnVoid()) {
      Identifier Unique = createUniqueName("result");
      Scopes.back().Names.insert(Unique);
      ResultName = Unique.str();

      OS << tok::kw_let << " " << ResultName;
      addResultTypeAnnotationIfNecessary(FD, HandlerDesc);
      OS << " " << tok::equal << " ";
    } else {
      // The name won't end up being used, just give it a bogus one so that
      // the result path is taken (versus the error path).
      ResultName = "result";
    }
    AddAwaitCall();
    OS << "\n";
    addCallToCompletionHandler(ResultName, HandlerDesc, HandlerName);
  }
}

const Pattern *AsyncConverter::bindingPatternToPrintInline(
    const Decl *D, const ClassifiedBlock &Block,
    const ClosureExpr *CallbackClosure) {
  // Only currently done for callback closures.
  if (!CallbackClosure)
    return nullptr;

  // If we can reduce the pattern bindings down to a single pattern, we may
  // be able to print it inline.
  auto *P = Block.getSinglePatternFor(D);
  if (!P)
    return nullptr;

  // Patterns that bind a single var are always printed inline.
  if (P->getSingleVar())
    return P;

  // If we have a multi-var binding, and the decl being bound is referenced
  // elsewhere in the block, we cannot print the pattern immediately in the
  // await call. Instead, we'll print it out of line.
  auto *Decls = ScopedDecls.getReferencedDecls(CallbackClosure->getBody());
  assert(Decls);
  auto NumRefs = Decls->lookup(D);
  return NumRefs == 1 ? P : nullptr;
}

AsyncConverter::InlinePatternsToPrint
AsyncConverter::getInlinePatternsToPrint(const ClassifiedBlock &Block,
                                         ArrayRef<const ParamDecl *> Params,
                                         const ClosureExpr *CallbackClosure) {
  InlinePatternsToPrint Patterns;
  for (auto *Param : Params) {
    if (auto *P = bindingPatternToPrintInline(Param, Block, CallbackClosure))
      Patterns[Param] = P;
  }
  return Patterns;
}

void AsyncConverter::printOutOfLineBindingPatterns(
    const ClassifiedBlock &Block, const InlinePatternsToPrint &InlinePatterns) {
  for (auto &Entry : Block.paramPatternBindings()) {
    auto *D = Entry.first;
    auto Aliases = Block.getAliasesFor(D);

    for (auto *P : Entry.second) {
      // If we already printed this as an inline pattern, there's nothing else
      // to do.
      if (InlinePatterns.lookup(D) == P)
        continue;

      // If this is an alias binding, it can be elided.
      if (auto *SingleVar = P->getSingleVar()) {
        if (Aliases.contains(SingleVar))
          continue;
      }

      auto HasMutable = P->hasAnyMutableBindings();
      OS << "\n" << (HasMutable ? tok::kw_var : tok::kw_let) << " ";
      convertPattern(P);
      OS << " = ";
      OS << newNameFor(D);
    }
  }
}

void AsyncConverter::addAwaitCall(const CallExpr *CE,
                                  const ClassifiedBlock &SuccessBlock,
                                  ArrayRef<const ParamDecl *> SuccessParams,
                                  const InlinePatternsToPrint &InlinePatterns,
                                  const AsyncHandlerParamDesc &HandlerDesc,
                                  bool AddDeclarations) {
  auto *Args = CE->getArgs();

  // Print the bindings to match the completion handler success parameters,
  // making sure to omit in the case of a Void return.
  if (!SuccessParams.empty() && !HandlerDesc.willAsyncReturnVoid()) {
    auto AllLet = true;

    // Gather the items to print for the variable bindings. This can either be
    // a param decl, or a pattern that binds it.
    using DeclOrPattern = llvm::PointerUnion<const Decl *, const Pattern *>;
    SmallVector<DeclOrPattern, 4> ToPrint;
    for (auto *Param : SuccessParams) {
      // Check if we have an inline pattern to print.
      if (auto *P = InlinePatterns.lookup(Param)) {
        if (P->hasAnyMutableBindings())
          AllLet = false;
        ToPrint.push_back(P);
        continue;
      }
      ToPrint.push_back(Param);
    }

    if (AddDeclarations) {
      if (AllLet) {
        OS << tok::kw_let;
      } else {
        OS << tok::kw_var;
      }
      OS << " ";
    }
    // 'res =' or '(res1, res2, ...) ='
    addTupleOf(ToPrint, OS, [&](DeclOrPattern Elt) {
      if (auto *P = Elt.dyn_cast<const Pattern *>()) {
        convertPattern(P);
        return;
      }
      OS << newNameFor(Elt.get<const Decl *>());
    });
    OS << " " << tok::equal << " ";
  }

  if (HandlerDesc.HasError) {
    OS << tok::kw_try << " ";
  }
  OS << "await ";

  // Try to replace the name with that of the alternative. Use the existing
  // name if for some reason that's not possible.
  bool NameAdded = false;
  if (HandlerDesc.Alternative) {
    const ValueDecl *Named = HandlerDesc.Alternative;
    if (auto *Accessor = dyn_cast<AccessorDecl>(HandlerDesc.Alternative))
      Named = Accessor->getStorage();
    if (!Named->getBaseName().isSpecial()) {
      Names.try_emplace(HandlerDesc.Func, Named->getBaseName().getIdentifier());
      convertNode(CE->getFn(), /*StartOverride=*/{}, /*ConvertCalls=*/false,
                  /*IncludeComments=*/false);
      NameAdded = true;
    }
  }
  if (!NameAdded) {
    addRange(CE->getStartLoc(), CE->getFn()->getEndLoc(),
             /*ToEndOfToken=*/true);
  }

  if (!HandlerDesc.alternativeIsAccessor())
    OS << tok::l_paren;

  size_t ConvertedArgIndex = 0;
  ArrayRef<ParamDecl *> AlternativeParams;
  if (HandlerDesc.Alternative)
    AlternativeParams = HandlerDesc.Alternative->getParameters()->getArray();

  for (auto I : indices(*Args)) {
    auto Arg = Args->get(I);
    auto *ArgExpr = Arg.getExpr();
    if (I == HandlerDesc.Index || isa<DefaultArgumentExpr>(ArgExpr))
      continue;

    if (ConvertedArgIndex > 0)
      OS << tok::comma << " ";

    if (HandlerDesc.Alternative) {
      // Skip argument if it's defaulted and has a different name
      while (ConvertedArgIndex < AlternativeParams.size() &&
             AlternativeParams[ConvertedArgIndex]->isDefaultArgument() &&
             AlternativeParams[ConvertedArgIndex]->getArgumentName() !=
                 Arg.getLabel()) {
        ConvertedArgIndex++;
      }

      if (ConvertedArgIndex < AlternativeParams.size()) {
        // Could have a different argument label (or none), so add it instead
        auto Name = AlternativeParams[ConvertedArgIndex]->getArgumentName();
        if (!Name.empty())
          OS << Name << ": ";
        convertNode(ArgExpr, /*StartOverride=*/{}, /*ConvertCalls=*/false);

        ConvertedArgIndex++;
        continue;
      }

      // Fallthrough if arguments don't match up for some reason
    }

    // Can't just add the range as we need to perform replacements. Also
    // make sure to include the argument label (if any)
    convertNode(ArgExpr, /*StartOverride=*/Arg.getLabelLoc(),
                /*ConvertCalls=*/false);
    ConvertedArgIndex++;
  }

  if (!HandlerDesc.alternativeIsAccessor())
    OS << tok::r_paren;
}

void AsyncConverter::addFallbackCatch(const ClosureCallbackParams &Params) {
  auto *ErrParam = Params.getErrParam();
  assert(ErrParam);
  auto ErrName = newNameFor(ErrParam);
  OS << tok::r_brace << " " << tok::kw_catch << " " << tok::l_brace << "\n"
     << ErrName << " = error\n";

  // If we have a known Bool success param, we need to bind it.
  addBoolFlagParamBindingIfNeeded(Params.getKnownBoolFlagParam(),
                                  BlockKind::ERROR);
  OS << tok::r_brace;
}

void AsyncConverter::addCatch(const ParamDecl *ErrParam) {
  OS << "\n" << tok::r_brace << " " << tok::kw_catch << " ";
  auto ErrName = newNameFor(ErrParam, false);
  if (!ErrName.empty() && ErrName != "_") {
    OS << tok::kw_let << " " << ErrName << " ";
  }
  OS << tok::l_brace;
}

void AsyncConverter::preparePlaceholdersAndUnwraps(
    AsyncHandlerDesc HandlerDesc, const ClosureCallbackParams &Params,
    BlockKind Block) {
  // Params that have been dropped always need placeholdering.
  for (auto *Param : Params.getAllParams()) {
    if (!Params.hasBinding(Param, Block))
      Placeholders.insert(Param);
  }
  // For the fallback case, no other params need placeholdering, as they are
  // all freely accessible in the fallback case.
  if (Block == BlockKind::FALLBACK)
    return;

  switch (HandlerDesc.Type) {
  case HandlerType::PARAMS: {
    auto *ErrParam = Params.getErrParam();
    auto SuccessParams = Params.getSuccessParams();
    switch (Block) {
    case BlockKind::FALLBACK:
      llvm_unreachable("Already handled");
    case BlockKind::ERROR:
      if (ErrParam) {
        if (HandlerDesc.shouldUnwrap(ErrParam->getTypeInContext())) {
          Placeholders.insert(ErrParam);
          Unwraps.insert(ErrParam);
        }
        // Can't use success params in the error body
        Placeholders.insert(SuccessParams.begin(), SuccessParams.end());
      }
      break;
    case BlockKind::SUCCESS:
      for (auto *SuccessParam : SuccessParams) {
        auto Ty = SuccessParam->getTypeInContext();
        if (HandlerDesc.shouldUnwrap(Ty)) {
          // Either unwrap or replace with a placeholder if there's some other
          // reference
          Unwraps.insert(SuccessParam);
          Placeholders.insert(SuccessParam);
        }

        // Void parameters get omitted where possible, so turn any reference
        // into a placeholder, as its usage is unlikely what the user wants.
        if (HandlerDesc.getSuccessParamAsyncReturnType(Ty)->isVoid())
          Placeholders.insert(SuccessParam);
      }
      // Can't use the error param in the success body
      if (ErrParam)
        Placeholders.insert(ErrParam);
      break;
    }
    break;
  }
  case HandlerType::RESULT: {
    // Any uses of the result parameter in the current body (that aren't
    // replaced) are invalid, so replace them with a placeholder.
    auto *ResultParam = Params.getResultParam();
    assert(ResultParam);
    Placeholders.insert(ResultParam);
    break;
  }
  default:
    llvm_unreachable("Unhandled handler type");
  }
}

void AsyncConverter::prepareNames(const ClassifiedBlock &Block,
                                  ArrayRef<const ParamDecl *> Params,
                                  const InlinePatternsToPrint &InlinePatterns,
                                  bool AddIfMissing) {
  for (auto *PD : Params) {
    // If this param is to be replaced by a pattern that binds multiple
    // separate vars, it's not actually going to be added to the scope, and
    // therefore doesn't need naming. This avoids needing to rename a var with
    // the same name later on in the scope, as it's not actually clashing.
    if (auto *P = InlinePatterns.lookup(PD)) {
      if (!P->getSingleVar())
        continue;
    }
    auto Name = Block.boundName(PD);
    if (Name.empty() && !AddIfMissing)
      continue;

    auto Ident = assignUniqueName(PD, Name);

    // Also propagate the name to any aliases.
    for (auto *Alias : Block.getAliasesFor(PD))
      Names[Alias] = Ident;
  }
}

Identifier AsyncConverter::createUniqueName(StringRef Name) {
  Identifier Ident = getASTContext().getIdentifier(Name);
  if (Name == "_")
    return Ident;

  auto &CurrentNames = Scopes.back().Names;
  if (CurrentNames.count(Ident)) {
    // Add a number to the end of the name until it's unique given the current
    // names in scope.
    llvm::SmallString<32> UniquedName;
    unsigned UniqueId = 1;
    do {
      UniquedName = Name;
      UniquedName.append(std::to_string(UniqueId));
      Ident = getASTContext().getIdentifier(UniquedName);
      UniqueId++;
    } while (CurrentNames.count(Ident));
  }
  return Ident;
}

Identifier AsyncConverter::assignUniqueName(const Decl *D,
                                            StringRef BoundName) {
  Identifier Ident;
  if (BoundName.empty()) {
    BoundName = getDeclName(D).userFacingName();
    if (BoundName.empty())
      return Ident;
  }

  if (BoundName.starts_with("$")) {
    llvm::SmallString<8> NewName;
    NewName.append("val");
    NewName.append(BoundName.drop_front());
    Ident = createUniqueName(NewName);
  } else {
    Ident = createUniqueName(BoundName);
  }

  Names.try_emplace(D, Ident);
  Scopes.back().Names.insert(Ident);
  return Ident;
}

StringRef AsyncConverter::newNameFor(const Decl *D, bool Required) {
  auto Res = Names.find(D);
  if (Res == Names.end()) {
    assert(!Required && "Missing name for decl when one was required");
    return StringRef();
  }
  return Res->second.str();
}

void AsyncConverter::addNewScope(const llvm::DenseSet<const Decl *> &Decls) {
  if (Scopes.empty()) {
    Scopes.emplace_back(/*ContinuationName=*/Identifier());
  } else {
    // If the parent scope is nested in a continuation, the new one is also.
    // Carry over the continuation name.
    Identifier PreviousContinuationName = Scopes.back().ContinuationName;
    Scopes.emplace_back(PreviousContinuationName);
  }
  for (auto D : Decls) {
    auto Name = getDeclName(D);
    if (!Name.empty())
      Scopes.back().Names.insert(Name);
  }
}

void AsyncConverter::clearNames(ArrayRef<const ParamDecl *> Params) {
  for (auto *Param : Params) {
    Unwraps.erase(Param);
    Placeholders.erase(Param);
    Names.erase(Param);
  }
}

void AsyncConverter::addForwardingCallTo(const FuncDecl *FD,
                                         StringRef HandlerReplacement) {
  OS << FD->getBaseName() << tok::l_paren;

  auto *Params = FD->getParameters();
  size_t ConvertedArgsIndex = 0;
  for (size_t I = 0, E = Params->size(); I < E; ++I) {
    if (I == TopHandler.Index) {
      /// If we're not replacing the handler with anything, drop it.
      if (HandlerReplacement.empty())
        continue;

      // Use a trailing closure if the handler is the last param
      if (I == E - 1) {
        OS << tok::r_paren << " ";
        OS << HandlerReplacement;
        return;
      }

      // Otherwise fall through to do the replacement.
    }

    if (ConvertedArgsIndex > 0)
      OS << tok::comma << " ";

    const auto *Param = Params->get(I);
    if (!Param->getArgumentName().empty())
      OS << Param->getArgumentName() << tok::colon << " ";

    if (I == TopHandler.Index) {
      OS << HandlerReplacement;
    } else {
      OS << Param->getParameterName();
    }

    ConvertedArgsIndex++;
  }
  OS << tok::r_paren;
}

void AsyncConverter::addForwardedErrorArgument(
    StringRef ErrorName, const AsyncHandlerDesc &HandlerDesc) {
  // If the error type is already Error, we can pass it as-is.
  auto ErrorType = *HandlerDesc.getErrorType();
  if (ErrorType->getCanonicalType() ==
      getASTContext().getErrorExistentialType()) {
    OS << ErrorName;
    return;
  }

  // Otherwise we need to add a force cast to the destination custom error
  // type. If this is for an Error? parameter, we'll need to add parens around
  // the cast to silence a compiler warning about force casting never
  // producing nil.
  auto RequiresParens = HandlerDesc.getErrorParam().has_value();
  if (RequiresParens)
    OS << tok::l_paren;

  OS << ErrorName << " " << tok::kw_as << tok::exclaim_postfix << " ";
  ErrorType->lookThroughSingleOptionalType()->print(OS);

  if (RequiresParens)
    OS << tok::r_paren;
}

void AsyncConverter::addDefaultValueOrPlaceholder(Type T) {
  if (T->isOptional()) {
    OS << tok::kw_nil;
  } else if (T->isVoid()) {
    OS << "()";
  } else {
    OS << "<#";
    T.print(OS);
    OS << "#>";
  }
}

void AsyncConverter::addCompletionHandlerArgument(
    size_t Index, StringRef ResultName, const AsyncHandlerDesc &HandlerDesc) {
  if (HandlerDesc.HasError && Index == HandlerDesc.params().size() - 1) {
    // The error parameter is the last argument of the completion handler.
    if (ResultName.empty()) {
      addForwardedErrorArgument("error", HandlerDesc);
    } else {
      addDefaultValueOrPlaceholder(HandlerDesc.params()[Index].getPlainType());
    }
  } else {
    if (ResultName.empty()) {
      addDefaultValueOrPlaceholder(HandlerDesc.params()[Index].getPlainType());
    } else if (HandlerDesc
                   .getSuccessParamAsyncReturnType(
                       HandlerDesc.params()[Index].getPlainType())
                   ->isVoid()) {
      // Void return types are not returned by the async function, synthesize
      // a Void instance.
      OS << tok::l_paren << tok::r_paren;
    } else if (HandlerDesc.getSuccessParams().size() > 1) {
      // If the async method returns a tuple, we need to pass its elements to
      // the completion handler separately. For example:
      //
      // func foo() async -> (String, Int) {}
      //
      // causes the following legacy body to be created:
      //
      // func foo(completion: (String, Int) -> Void) {
      //   Task {
      //     let result = await foo()
      //     completion(result.0, result.1)
      //   }
      // }
      OS << ResultName << tok::period;

      auto Label = HandlerDesc.getAsyncReturnTypeLabel(Index);
      if (!Label.empty()) {
        OS << Label;
      } else {
        OS << Index;
      }
    } else {
      OS << ResultName;
    }
  }
}

void AsyncConverter::addCallToCompletionHandler(
    StringRef ResultName, const AsyncHandlerDesc &HandlerDesc,
    StringRef HandlerName) {
  OS << HandlerName << tok::l_paren;

  // Construct arguments to pass to the completion handler
  switch (HandlerDesc.Type) {
  case HandlerType::INVALID:
    llvm_unreachable("Cannot be rewritten");
    break;
  case HandlerType::PARAMS: {
    for (size_t I = 0; I < HandlerDesc.params().size(); ++I) {
      if (I > 0) {
        OS << tok::comma << " ";
      }
      addCompletionHandlerArgument(I, ResultName, HandlerDesc);
    }
    break;
  }
  case HandlerType::RESULT: {
    if (!ResultName.empty()) {
      OS << tok::period_prefix << "success" << tok::l_paren;
      if (!HandlerDesc.willAsyncReturnVoid()) {
        OS << ResultName;
      } else {
        OS << tok::l_paren << tok::r_paren;
      }
      OS << tok::r_paren;
    } else {
      OS << tok::period_prefix << "failure" << tok::l_paren;
      addForwardedErrorArgument("error", HandlerDesc);
      OS << tok::r_paren;
    }
    break;
  }
  }
  OS << tok::r_paren; // Close the call to the completion handler
}

void AsyncConverter::addAsyncFuncReturnType(
    const AsyncHandlerDesc &HandlerDesc) {
  // Type or (Type1, Type2, ...)
  SmallVector<LabeledReturnType, 2> Scratch;
  auto ReturnTypes = HandlerDesc.getAsyncReturnTypes(Scratch);
  if (ReturnTypes.empty()) {
    OS << "Void";
  } else {
    addTupleOf(ReturnTypes, OS, [&](LabeledReturnType LabelAndType) {
      if (!LabelAndType.Label.empty()) {
        OS << LabelAndType.Label << tok::colon << " ";
      }
      LabelAndType.Ty->print(OS);
    });
  }
}

void AsyncConverter::addResultTypeAnnotationIfNecessary(
    const FuncDecl *FD, const AsyncHandlerDesc &HandlerDesc) {
  if (FD->isGeneric()) {
    OS << tok::colon << " ";
    addAsyncFuncReturnType(HandlerDesc);
  }
}

//===--- CodeCompletion.cpp - Code completion implementation --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CodeCompletion.h"
#include "CodeCompletionDiagnostics.h"
#include "CodeCompletionResultBuilder.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Comment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/IDE/AfterPoundExprCompletion.h"
#include "swift/IDE/ArgumentCompletion.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/CodeCompletionConsumer.h"
#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/IDE/CodeCompletionStringPrinter.h"
#include "swift/IDE/CompletionLookup.h"
#include "swift/IDE/CompletionOverrideLookup.h"
#include "swift/IDE/ExprCompletion.h"
#include "swift/IDE/KeyPathCompletion.h"
#include "swift/IDE/PostfixCompletion.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"
#include "swift/IDE/UnresolvedMemberCompletion.h"
#include "swift/IDE/Utils.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Comment.h"
#include "clang/AST/CommentVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <string>

using namespace swift;
using namespace ide;

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        ++Ptr;
      } while (Ptr < End && *Ptr != '#');
      if (Ptr == End)
        break;
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

namespace {

class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks,
                                    public DoneParsingCallback {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;
  CodeCompletionExpr *CodeCompleteTokenExpr = nullptr;
  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  SourceLoc DotLoc;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  CustomSyntaxAttributeKind AttrKind;

  /// When the code completion token occurs in a custom attribute, the attribute
  /// it occurs in. Used so we can complete inside the attribute even if it's
  /// not attached to the AST, e.g. because there is no var decl it could be
  /// attached to.
  CustomAttr *AttrWithCompletion = nullptr;

  /// In situations when \c SyntaxKind hints or determines
  /// completions, i.e. a precedence group attribute, this
  /// can be set and used to control the code completion scenario.
  CodeCompletionCallbacks::PrecedenceGroupCompletionKind SyntxKind;

  int AttrParamIndex;
  bool AttrParamHasLabel;
  bool IsInSil = false;
  bool HasSpace = false;
  bool ShouldCompleteCallPatternAfterParen = true;
  bool PreferFunctionReferencesToCalls = false;
  bool AttTargetIsIndependent = false;
  bool IsAtStartOfLine = false;
  llvm::Optional<DeclKind> AttTargetDK;
  llvm::Optional<StmtKind> ParentStmtKind;

  SmallVector<StringRef, 3> ParsedKeywords;
  SourceLoc introducerLoc;

  std::vector<std::pair<std::string, bool>> SubModuleNameVisibilityPairs;

  llvm::Optional<std::pair<Type, ConcreteDeclRef>> typeCheckParsedExpr() {
    assert(ParsedExpr && "should have an expression");

    // Figure out the kind of type-check we'll be performing.
    auto CheckKind = CompletionTypeCheckKind::Normal;
    if (Kind == CompletionKind::KeyPathExprObjC)
      CheckKind = CompletionTypeCheckKind::KeyPath;

    // If we've already successfully type-checked the expression for some
    // reason, just return the type.
    // FIXME: if it's ErrorType but we've already typechecked we shouldn't
    // typecheck again. rdar://21466394
    if (CheckKind == CompletionTypeCheckKind::Normal &&
        ParsedExpr->getType() && !ParsedExpr->getType()->is<ErrorType>()) {
      return getReferencedDecl(ParsedExpr);
    }

    ConcreteDeclRef ReferencedDecl = nullptr;
    Expr *ModifiedExpr = ParsedExpr;
    if (auto T = getTypeOfCompletionContextExpr(P.Context, CurDeclContext,
                                                CheckKind, ModifiedExpr,
                                                ReferencedDecl)) {
      // FIXME: even though we don't apply the solution, the type checker may
      // modify the original expression. We should understand what effect that
      // may have on code completion.
      ParsedExpr = ModifiedExpr;

      return std::make_pair(*T, ReferencedDecl);
    }
    return llvm::None;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    // If the type appeared inside an extension, make sure that extension has
    // been bound.
    auto SF = CurDeclContext->getParentSourceFile();
    auto visitTopLevelDecl = [&](Decl *D) {
      if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        if (ED->getSourceRange().contains(ParsedTypeLoc.getLoc())) {
          ED->computeExtendedNominal();
        }
      }
    };
    for (auto item : SF->getTopLevelItems()) {
      if (auto D = item.dyn_cast<Decl *>()) {
        visitTopLevelDecl(D);
      }
    }
    for (auto *D : SF->getHoistedDecls()) {
      visitTopLevelDecl(D);
    }

    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    if (ParsedTypeLoc.wasValidated() && !ParsedTypeLoc.isError()) {
      return true;
    }

    const auto ty = swift::performTypeResolution(
        ParsedTypeLoc.getTypeRepr(), P.Context,
        CurDeclContext->getGenericSignatureOfContext(),
        /*SILContext=*/nullptr,
        CurDeclContext,
        /*ProduceDiagnostics=*/false);
    if (!ty->hasError()) {
      ParsedTypeLoc.setType(CurDeclContext->mapTypeIntoContext(ty));
      return true;
    }

    ParsedTypeLoc.setType(ty);

    // It doesn't type check as a type, so see if it's a qualifying module name.
    if (auto *declRefTR =
            dyn_cast<DeclRefTypeRepr>(ParsedTypeLoc.getTypeRepr())) {
      // If it has more than one component, it can't be a module name.
      if (isa<MemberTypeRepr>(declRefTR))
        return false;

      const auto *identTR = cast<IdentTypeRepr>(declRefTR);
      ImportPath::Module::Builder builder(
          identTR->getNameRef().getBaseIdentifier(), identTR->getLoc());

      if (auto Module = Context.getLoadedModule(builder.get()))
        ParsedTypeLoc.setType(ModuleType::get(Module));
      return true;
    }
    return false;
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), DoneParsingCallback(),
        CompletionContext(CompletionContext), Consumer(Consumer) {}

  void setAttrTargetDeclKind(llvm::Optional<DeclKind> DK) override {
    if (DK == DeclKind::PatternBinding)
      DK = DeclKind::Var;
    else if (DK == DeclKind::Param)
      // For params, consider the attribute is always for the decl.
      AttTargetIsIndependent = false;

    if (!AttTargetIsIndependent)
      AttTargetDK = DK;
  }

  void setCompletingInAttribute(CustomAttr *Attr) override {
    AttrWithCompletion = Attr;
    CurDeclContext = P.CurDeclContext;
  }

  void completeDotExpr(CodeCompletionExpr *E, SourceLoc DotLoc) override;
  void completeStmtOrExpr(CodeCompletionExpr *E) override;
  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completeForEachSequenceBeginning(CodeCompletionExpr *E) override;
  void completeForEachInKeyword() override;
  void completePostfixExpr(Expr *E, bool hasSpace) override;
  void completePostfixExprParen(Expr *E, Expr *CodeCompletionE) override;
  void completeExprKeyPath(KeyPathExpr *KPE, SourceLoc DotLoc) override;

  void completeTypeDeclResultBeginning() override;
  void completeTypeSimpleBeginning() override;
  void completeTypeSimpleWithDot(TypeRepr *TR) override;
  void completeTypeSimpleWithoutDot(TypeRepr *TR) override;

  void completeCaseStmtKeyword() override;
  void completeCaseStmtBeginning(CodeCompletionExpr *E) override;
  void completeDeclAttrBeginning(bool Sil, bool isIndependent) override;
  void completeDeclAttrParam(CustomSyntaxAttributeKind DK, int Index,
                             bool HasLabel) override;
  void completeEffectsSpecifier(bool hasAsync, bool hasThrows) override;
  void completeInPrecedenceGroup(
      CodeCompletionCallbacks::PrecedenceGroupCompletionKind SK) override;
  void completeNominalMemberBeginning(
      SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) override;
  void completeAccessorBeginning(CodeCompletionExpr *E) override;

  void completePoundAvailablePlatform() override;
  void completeImportDecl(ImportPath::Builder &Path) override;
  void completeUnresolvedMember(CodeCompletionExpr *E,
                                SourceLoc DotLoc) override;
  void completeCallArg(CodeCompletionExpr *E, bool isFirst) override;
  void completeLabeledTrailingClosure(CodeCompletionExpr *E,
                                      bool isAtStartOfLine) override;

  bool canPerformCompleteLabeledTrailingClosure() const override {
    return true;
  }

  void completeReturnStmt(CodeCompletionExpr *E) override;
  void completeYieldStmt(CodeCompletionExpr *E,
                         llvm::Optional<unsigned> yieldIndex) override;
  void completeAfterPoundExpr(CodeCompletionExpr *E,
                              llvm::Optional<StmtKind> ParentKind) override;
  void completeAfterPoundDirective() override;
  void completePlatformCondition() override;
  void completeGenericRequirement() override;
  void completeAfterIfStmtElse() override;
  void completeStmtLabel(StmtKind ParentKind) override;
  void completeForEachPatternBeginning(bool hasTry, bool hasAwait) override;
  void completeTypeAttrBeginning() override;
  void completeOptionalBinding() override;
  void completeWithoutConstraintType() override;

  void doneParsing(SourceFile *SrcFile) override;

private:
  void addKeywords(CodeCompletionResultSink &Sink, bool MaybeFuncBody);
  bool trySolverCompletion(bool MaybeFuncBody);
};
} // end anonymous namespace

static void addSelectorModifierKeywords(CodeCompletionResultSink &sink) {
  auto addKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    CodeCompletionResultBuilder Builder(sink, CodeCompletionResultKind::Keyword,
                                        SemanticContextKind::None);
    Builder.setKeywordKind(Kind);
    Builder.addTextChunk(Name);
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("@objc property", /*IsVarArg=*/false);
  };

  addKeyword("getter", CodeCompletionKeywordKind::None);
  addKeyword("setter", CodeCompletionKeywordKind::None);
}

void CodeCompletionCallbacksImpl::completeDotExpr(CodeCompletionExpr *E,
                                                  SourceLoc DotLoc) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E->getBase();
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtOrExpr(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::StmtOrExpr;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
    if (CompleteExprSelectorContext == ObjCSelectorContext::MethodSelector) {
      addSelectorModifierKeywords(CompletionContext.getResultSink());
    }
  }


  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeForEachSequenceBeginning(
    CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::ForEachSequence;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeForEachInKeyword() {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::ForEachInKw;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E, bool hasSpace) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  HasSpace = hasSpace;
  Kind = CompletionKind::PostfixExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprParen(Expr *E,
                                                           Expr *CodeCompletionE) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprParen;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = static_cast<CodeCompletionExpr*>(CodeCompletionE);

  ShouldCompleteCallPatternAfterParen = true;
  if (CompletionContext.getCallPatternHeuristics()) {
    // Lookahead one token to decide what kind of call completions to provide.
    // When it appears that there is already code for the call present, just
    // complete values and/or argument labels.  Otherwise give the entire call
    // pattern.
    Token next = P.peekToken();
    if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
      ShouldCompleteCallPatternAfterParen = false;
    }
  }
}

void CodeCompletionCallbacksImpl::completeExprKeyPath(KeyPathExpr *KPE,
                                                      SourceLoc DotLoc) {
  Kind = (!KPE || KPE->isObjC()) ? CompletionKind::KeyPathExprObjC
                                 : CompletionKind::KeyPathExprSwift;
  ParsedExpr = KPE;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePoundAvailablePlatform() {
  Kind = CompletionKind::PoundAvailablePlatform;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeDeclResultBeginning() {
  Kind = CompletionKind::TypeDeclResultBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrParam(
    CustomSyntaxAttributeKind DK, int Index, bool HasLabel) {
  Kind = CompletionKind::AttributeDeclParen;
  AttrKind = DK;
  AttrParamIndex = Index;
  AttrParamHasLabel = HasLabel;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeEffectsSpecifier(bool hasAsync,
                                                           bool hasThrows) {
  Kind = CompletionKind::EffectsSpecifier;
  CurDeclContext = P.CurDeclContext;
  ParsedKeywords.clear();
  if (hasAsync)
    ParsedKeywords.emplace_back("async");
  if (hasThrows)
    ParsedKeywords.emplace_back("throws");
}

void CodeCompletionCallbacksImpl::completeDeclAttrBeginning(
    bool Sil, bool isIndependent) {
  Kind = CompletionKind::AttributeBegin;
  IsInSil = Sil;
  CurDeclContext = P.CurDeclContext;
  AttTargetIsIndependent = isIndependent;
}

void CodeCompletionCallbacksImpl::completeInPrecedenceGroup(
    CodeCompletionCallbacks::PrecedenceGroupCompletionKind SK) {
  assert(P.Tok.is(tok::code_complete));

  SyntxKind = SK;
  Kind = CompletionKind::PrecedenceGroup;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleWithDot(TypeRepr *TR) {
  assert(TR);
  Kind = CompletionKind::TypeSimpleWithDot;
  ParsedTypeLoc = TypeLoc(TR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleWithoutDot(TypeRepr *TR) {
  assert(TR);
  Kind = CompletionKind::TypeSimpleWithoutDot;
  ParsedTypeLoc = TypeLoc(TR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtKeyword() {
  Kind = CompletionKind::CaseStmtKeyword;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtBeginning(CodeCompletionExpr *E) {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeImportDecl(
    ImportPath::Builder &Path) {
  Kind = CompletionKind::Import;
  CurDeclContext = P.CurDeclContext;
  DotLoc = Path.empty() ? SourceLoc() : Path.back().Loc;
  if (DotLoc.isInvalid())
    return;
  auto Importer = static_cast<ClangImporter *>(CurDeclContext->getASTContext().
                                               getClangModuleLoader());
  std::vector<std::string> SubNames;
  Importer->collectSubModuleNames(Path.get().getModulePath(false), SubNames);
  ASTContext &Ctx = CurDeclContext->getASTContext();
  Path.push_back(Identifier());
  for (StringRef Sub : SubNames) {
    Path.back().Item = Ctx.getIdentifier(Sub);
    SubModuleNameVisibilityPairs.push_back(
      std::make_pair(Sub.str(),
                     Ctx.getLoadedModule(Path.get().getModulePath(false))));
  }
  Path.pop_back();
}

void CodeCompletionCallbacksImpl::completeUnresolvedMember(CodeCompletionExpr *E,
    SourceLoc DotLoc) {
  Kind = CompletionKind::UnresolvedMember;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  this->DotLoc = DotLoc;
}

void CodeCompletionCallbacksImpl::completeCallArg(CodeCompletionExpr *E,
                                                  bool isFirst) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::CallArg;

  ShouldCompleteCallPatternAfterParen = false;
  if (isFirst) {
    ShouldCompleteCallPatternAfterParen = true;
    if (CompletionContext.getCallPatternHeuristics()) {
      // Lookahead one token to decide what kind of call completions to provide.
      // When it appears that there is already code for the call present, just
      // complete values and/or argument labels.  Otherwise give the entire call
      // pattern.
      Token next = P.peekToken();
      if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
        ShouldCompleteCallPatternAfterParen = false;
      }
    }
  }
}

void CodeCompletionCallbacksImpl::completeLabeledTrailingClosure(
    CodeCompletionExpr *E, bool isAtStartOfLine) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::LabeledTrailingClosure;
  IsAtStartOfLine = isAtStartOfLine;
}

void CodeCompletionCallbacksImpl::completeReturnStmt(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::ReturnStmtExpr;
}

void CodeCompletionCallbacksImpl::completeYieldStmt(
    CodeCompletionExpr *E, llvm::Optional<unsigned> index) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  // TODO: use a different completion kind when completing without an index
  // in a multiple-value context.
  Kind = CompletionKind::YieldStmtExpr;
}

void CodeCompletionCallbacksImpl::completeAfterPoundExpr(
    CodeCompletionExpr *E, llvm::Optional<StmtKind> ParentKind) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::AfterPoundExpr;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeAfterPoundDirective() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AfterPoundDirective;
}

void CodeCompletionCallbacksImpl::completePlatformCondition() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::PlatformConditon;
}

void CodeCompletionCallbacksImpl::completeAfterIfStmtElse() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AfterIfStmtElse;
}

void CodeCompletionCallbacksImpl::completeGenericRequirement() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::GenericRequirement;
}

void CodeCompletionCallbacksImpl::completeNominalMemberBeginning(
    SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) {
  assert(!InEnumElementRawValue);
  this->introducerLoc = introducerLoc;
  ParsedKeywords.clear();
  ParsedKeywords.append(Keywords.begin(), Keywords.end());
  Kind = CompletionKind::NominalMemberBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeAccessorBeginning(
    CodeCompletionExpr *E) {
  Kind = CompletionKind::AccessorBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtLabel(StmtKind ParentKind) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::StmtLabel;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeForEachPatternBeginning(
    bool hasTry, bool hasAwait) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::ForEachPatternBeginning;
  ParsedKeywords.clear();
  if (hasTry)
    ParsedKeywords.emplace_back("try");
  if (hasAwait)
    ParsedKeywords.emplace_back("await");
}

void CodeCompletionCallbacksImpl::completeOptionalBinding() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::OptionalBinding;
}

void CodeCompletionCallbacksImpl::completeWithoutConstraintType() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::WithoutConstraintType;
}

void CodeCompletionCallbacksImpl::completeTypeAttrBeginning() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::TypeAttrBeginning;
}

bool swift::ide::isDynamicLookup(Type T) {
  return T->getRValueType()->isAnyObject();
}

static bool isClangSubModule(ModuleDecl *TheModule) {
  if (auto ClangMod = TheModule->findUnderlyingClangModule())
    return ClangMod->isSubModule();
  return false;
}

static void addKeyword(CodeCompletionResultSink &Sink, StringRef Name,
                       CodeCompletionKeywordKind Kind,
                       StringRef TypeAnnotation = "",
                       CodeCompletionFlair Flair = {}) {
  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                      SemanticContextKind::None);
  Builder.setKeywordKind(Kind);
  Builder.addKeyword(Name);
  Builder.addFlair(Flair);
  if (!TypeAnnotation.empty())
    Builder.addTypeAnnotation(TypeAnnotation);
  Builder.setResultTypeNotApplicable();
}

static void addDeclKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool IsConcurrencyEnabled) {
  auto isTypeDeclIntroducer = [](CodeCompletionKeywordKind Kind,
                                 llvm::Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_class:
    case CodeCompletionKeywordKind::kw_struct:
    case CodeCompletionKeywordKind::kw_enum:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    default:
      break;
    }
    return false;
  };
  auto isTopLevelOnlyDeclIntroducer =
      [](CodeCompletionKeywordKind Kind,
         llvm::Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_operator:
    case CodeCompletionKeywordKind::kw_precedencegroup:
    case CodeCompletionKeywordKind::kw_import:
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    default:
      return false;
    }
  };

  auto getFlair = [&](CodeCompletionKeywordKind Kind,
                      llvm::Optional<DeclAttrKind> DAK) -> CodeCompletionFlair {
    if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      // Type decls are common in library file top-level.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::CommonKeywordAtCurrentPosition;
    }
    if (isa<ProtocolDecl>(DC)) {
      // Protocols cannot have nested type decls (other than 'typealias').
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (DC->isTypeContext()) {
      // Top-level only decls are invalid in type context.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (isCompletionDeclContextLocalContext(DC)) {
      // Local type decl are valid, but not common.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // Top-level only decls are invalid in function body.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // 'init', 'deinit' and 'subscript' are invalid in function body.
      // Access control modifiers are invalid in function body.
      switch (Kind) {
      case CodeCompletionKeywordKind::kw_init:
      case CodeCompletionKeywordKind::kw_deinit:
      case CodeCompletionKeywordKind::kw_subscript:
      case CodeCompletionKeywordKind::kw_private:
      case CodeCompletionKeywordKind::kw_fileprivate:
      case CodeCompletionKeywordKind::kw_internal:
      case CodeCompletionKeywordKind::kw_public:
      case CodeCompletionKeywordKind::kw_static:
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      default:
        break;
      }

      // These modifiers are invalid for decls in function body.
      if (DAK) {
        switch (*DAK) {
        case DeclAttrKind::DAK_Lazy:
        case DeclAttrKind::DAK_Final:
        case DeclAttrKind::DAK_Infix:
        case DeclAttrKind::DAK_Frozen:
        case DeclAttrKind::DAK_Prefix:
        case DeclAttrKind::DAK_Postfix:
        case DeclAttrKind::DAK_Dynamic:
        case DeclAttrKind::DAK_Override:
        case DeclAttrKind::DAK_Optional:
        case DeclAttrKind::DAK_Required:
        case DeclAttrKind::DAK_Convenience:
        case DeclAttrKind::DAK_AccessControl:
        case DeclAttrKind::DAK_Nonisolated:
          return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

        default:
          break;
        }
      }
    }
    return llvm::None;
  };

  auto AddDeclKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind,
                            llvm::Optional<DeclAttrKind> DAK) {
    if (Name == "let" || Name == "var") {
      // Treat keywords that could be the start of a pattern specially.
      return;
    }

    // FIXME: This should use canUseAttributeOnDecl.

    // Remove user inaccessible keywords.
    if (DAK.has_value() && DeclAttribute::isUserInaccessible(*DAK))
      return;

    // Remove keywords only available when concurrency is enabled.
    if (DAK.has_value() && !IsConcurrencyEnabled &&
        DeclAttribute::isConcurrencyOnly(*DAK))
      return;

    CodeCompletionFlair flair = getFlair(Kind, DAK);

    // Special case for 'actor'. Get the same flair with 'kw_class'.
    if (Kind == CodeCompletionKeywordKind::None && Name == "actor")
      flair = getFlair(CodeCompletionKeywordKind::kw_class, llvm::None);

    addKeyword(Sink, Name, Kind, /*TypeAnnotation=*/"", flair);
  };

#define DECL_KEYWORD(kw)                                                       \
  AddDeclKeyword(#kw, CodeCompletionKeywordKind::kw_##kw, llvm::None);
#include "swift/AST/TokenKinds.def"
  // Manually add "actor" because it's a contextual keyword.
  AddDeclKeyword("actor", CodeCompletionKeywordKind::None, llvm::None);

  // Context-sensitive keywords.
  auto AddCSKeyword = [&](StringRef Name, DeclAttrKind Kind) {
    AddDeclKeyword(Name, CodeCompletionKeywordKind::None, Kind);
  };

#define CONTEXTUAL_CASE(KW, CLASS) AddCSKeyword(#KW, DAK_##CLASS);
#define CONTEXTUAL_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, CLASS) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#include <swift/AST/Attr.def>
#undef CONTEXTUAL_CASE
}

static void addStmtKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool MaybeFuncBody) {
  CodeCompletionFlair flair;
  // Starting a statement at top-level in non-script files is invalid.
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  auto AddStmtKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    if (!MaybeFuncBody && Kind == CodeCompletionKeywordKind::kw_return)
      return;

    // 'in' keyword is added in 'addClosureSignatureKeywordsIfApplicable' if
    // needed.
    if (Kind == CodeCompletionKeywordKind::kw_in)
      return;

    addKeyword(Sink, Name, Kind, "", flair);
  };
#define STMT_KEYWORD(kw) AddStmtKeyword(#kw, CodeCompletionKeywordKind::kw_##kw);
#include "swift/AST/TokenKinds.def"
}

static void addCaseStmtKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
  addKeyword(Sink, "default", CodeCompletionKeywordKind::kw_default);
}

static void addLetVarKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "let", CodeCompletionKeywordKind::kw_let);
  addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
}

static void addAccessorKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "get", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "set", CodeCompletionKeywordKind::None);
}

static void addObserverKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "willSet", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "didSet", CodeCompletionKeywordKind::None);
}

void swift::ide::addExprKeywords(CodeCompletionResultSink &Sink,
                                 DeclContext *DC) {
  // Expression is invalid at top-level of non-script files.
  CodeCompletionFlair flair;
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  // Expr keywords.
  addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "try!", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "try?", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "await", CodeCompletionKeywordKind::None, "", flair);
}

void swift::ide::addSuperKeyword(CodeCompletionResultSink &Sink,
                                 DeclContext *DC) {
  if (!DC)
    return;
  auto *TC = DC->getInnermostTypeContext();
  if (!TC)
    return;
  auto *CD = TC->getSelfClassDecl();
  if (!CD)
    return;
  Type ST = CD->getSuperclass();
  if (ST.isNull() || ST->is<ErrorType>())
    return;

  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                      SemanticContextKind::CurrentNominal);
  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (AFD->getOverriddenDecl() != nullptr) {
      Builder.addFlair(CodeCompletionFlairBit::CommonKeywordAtCurrentPosition);
    }
  }

  Builder.setKeywordKind(CodeCompletionKeywordKind::kw_super);
  Builder.addKeyword("super");
  Builder.addTypeAnnotation(ST, PrintOptions());
}

static void addOpaqueTypeKeyword(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "some", CodeCompletionKeywordKind::None, "some");
}

static void addAnyTypeKeyword(CodeCompletionResultSink &Sink, Type T) {
  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                      SemanticContextKind::None);
  Builder.setKeywordKind(CodeCompletionKeywordKind::None);
  Builder.addKeyword("Any");
  Builder.addTypeAnnotation(T, PrintOptions());
}

static void
addClosureSignatureKeywordsIfApplicable(CodeCompletionResultSink &Sink,
                                        DeclContext *DC) {
  ClosureExpr *closure = dyn_cast<ClosureExpr>(DC);
  if (!closure)
    return;
  if (closure->getInLoc().isValid())
    return;

  addKeyword(Sink, "in", CodeCompletionKeywordKind::kw_in);
}

void CodeCompletionCallbacksImpl::addKeywords(CodeCompletionResultSink &Sink,
                                              bool MaybeFuncBody) {
  auto addEffectsSpecifierKeywords = [&] {
    if (!llvm::is_contained(ParsedKeywords, "async"))
      addKeyword(Sink, "async", CodeCompletionKeywordKind::None);
    if (!llvm::is_contained(ParsedKeywords, "throws"))
      addKeyword(Sink, "throws", CodeCompletionKeywordKind::kw_throws);
  };

  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::AttributeDeclParen:
  case CompletionKind::AttributeBegin:
  case CompletionKind::PoundAvailablePlatform:
  case CompletionKind::Import:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::LabeledTrailingClosure:
  case CompletionKind::AfterPoundExpr:
  case CompletionKind::AfterPoundDirective:
  case CompletionKind::PlatformConditon:
  case CompletionKind::GenericRequirement:
  case CompletionKind::KeyPathExprObjC:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::PrecedenceGroup:
  case CompletionKind::StmtLabel:
  case CompletionKind::TypeAttrBeginning:
  case CompletionKind::OptionalBinding:
  case CompletionKind::WithoutConstraintType:
    break;

  case CompletionKind::EffectsSpecifier:
    addEffectsSpecifierKeywords();
    break;

  case CompletionKind::AccessorBeginning: {
    // TODO: Omit already declared or mutally exclusive accessors.
    //       E.g. If 'get' is already declared, emit 'set' only.
    addAccessorKeywords(Sink);

    // Only 'var' for non-protocol context can have 'willSet' and 'didSet'.
    assert(ParsedDecl);
    VarDecl *var = dyn_cast<VarDecl>(ParsedDecl);
    if (auto accessor = dyn_cast<AccessorDecl>(ParsedDecl))
      var = dyn_cast<VarDecl>(accessor->getStorage());
    if (var && !var->getDeclContext()->getSelfProtocolDecl())
      addObserverKeywords(Sink);

    if (!isa<AccessorDecl>(ParsedDecl))
      break;

    MaybeFuncBody = true;
    LLVM_FALLTHROUGH;
  }
  case CompletionKind::StmtOrExpr:
    addDeclKeywords(Sink, CurDeclContext,
                    Context.LangOpts.EnableExperimentalConcurrency);
    addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
    addClosureSignatureKeywordsIfApplicable(Sink, CurDeclContext);

    LLVM_FALLTHROUGH;
  case CompletionKind::ReturnStmtExpr:
  case CompletionKind::YieldStmtExpr:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::ForEachSequence:
    addSuperKeyword(Sink, CurDeclContext);
    addLetVarKeywords(Sink);
    addExprKeywords(Sink, CurDeclContext);
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::CallArg:
  case CompletionKind::PostfixExprParen:
    // Note that we don't add keywords here as the completion might be for
    // an argument list pattern. We instead add keywords later in
    // CodeCompletionCallbacksImpl::doneParsing when we know we're not
    // completing for a argument list pattern.
    break;

  case CompletionKind::CaseStmtKeyword:
    addCaseStmtKeywords(Sink);
    break;

  case CompletionKind::PostfixExpr:
    // Suggest 'in' for '{ value <HERE>'.
    if (HasSpace)
      addClosureSignatureKeywordsIfApplicable(Sink, CurDeclContext);

    break;
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::TypeSimpleWithDot:
    break;

  case CompletionKind::TypeSimpleWithoutDot:
    // Suggest effects specifiers after a tuple type because it may be
    // intended as a parameter list.
    if (isa_and_nonnull<TupleTypeRepr>(ParsedTypeLoc.getTypeRepr())) {
      addEffectsSpecifierKeywords();
    }
    break;

  case CompletionKind::TypeDeclResultBeginning: {
    auto DC = CurDeclContext;
    if (ParsedDecl && ParsedDecl == CurDeclContext->getAsDecl())
      DC = ParsedDecl->getDeclContext();
    if (!isa<ProtocolDecl>(DC))
      if (DC->isTypeContext() || isa_and_nonnull<FuncDecl>(ParsedDecl))
        addOpaqueTypeKeyword(Sink);

    LLVM_FALLTHROUGH;
  }
  case CompletionKind::TypeSimpleBeginning:
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::NominalMemberBeginning: {
    bool HasDeclIntroducer = llvm::find_if(ParsedKeywords,
                                           [this](const StringRef kw) {
      return llvm::StringSwitch<bool>(kw)
        .Case("associatedtype", true)
        .Case("class", !CurDeclContext || !isa<ClassDecl>(CurDeclContext))
        .Case("deinit", true)
        .Case("enum", true)
        .Case("extension", true)
        .Case("func", true)
        .Case("import", true)
        .Case("init", true)
        .Case("let", true)
        .Case("operator", true)
        .Case("precedencegroup", true)
        .Case("protocol", true)
        .Case("struct", true)
        .Case("subscript", true)
        .Case("typealias", true)
        .Case("var", true)
        .Default(false);
    }) != ParsedKeywords.end();
    if (!HasDeclIntroducer) {
      addDeclKeywords(Sink, CurDeclContext,
                      Context.LangOpts.EnableExperimentalConcurrency);
      addLetVarKeywords(Sink);
    }
    break;
  }

  case CompletionKind::AfterIfStmtElse:
    addKeyword(Sink, "if", CodeCompletionKeywordKind::kw_if);
    break;
  case CompletionKind::ForEachPatternBeginning:
    if (!llvm::is_contained(ParsedKeywords, "try"))
      addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try);
    if (!llvm::is_contained(ParsedKeywords, "await"))
      addKeyword(Sink, "await", CodeCompletionKeywordKind::None);
    addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
    addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
    break;
  case CompletionKind::ForEachInKw:
    addKeyword(Sink, "in", CodeCompletionKeywordKind::kw_in);
  }
}

static void addPoundDirectives(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef name, CodeCompletionKeywordKind K,
          llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer =
              nullptr) {
        CodeCompletionResultBuilder Builder(Sink,
                                            CodeCompletionResultKind::Keyword,
                                            SemanticContextKind::None);
        Builder.addBaseName(name);
        Builder.setKeywordKind(K);
        if (consumer)
          consumer(Builder);
      };

  addWithName("sourceLocation", CodeCompletionKeywordKind::pound_sourceLocation,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("file");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("String");
    Builder.addComma();
    Builder.addTextChunk("line");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("Int");
    Builder.addRightParen();
  });

#ifndef SWIFT_SWIFT_PARSER
  addWithName("warning", CodeCompletionKeywordKind::pound_warning,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });
  addWithName("error", CodeCompletionKeywordKind::pound_error,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });
#endif

  addWithName("if ", CodeCompletionKeywordKind::pound_if,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });

  // FIXME: These directives are only valid in conditional completion block.
  addWithName("elseif ", CodeCompletionKeywordKind::pound_elseif,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });
  addWithName("else", CodeCompletionKeywordKind::pound_else);
  addWithName("endif", CodeCompletionKeywordKind::pound_endif);
}

/// Add platform conditions used in '#if' and '#elseif' directives.
static void addPlatformConditions(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef Name,
          llvm::function_ref<void(CodeCompletionResultBuilder & Builder)>
              consumer) {
        CodeCompletionResultBuilder Builder(
            Sink, CodeCompletionResultKind::Pattern,
            // FIXME: SemanticContextKind::CurrentModule is not correct.
            // Use 'None' (and fix prioritization) or introduce a new context.
            SemanticContextKind::CurrentModule);
        Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
        Builder.addBaseName(Name);
        Builder.addLeftParen();
        consumer(Builder);
        Builder.addRightParen();
      };

  addWithName("os", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("arch", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("canImport", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("module");
  });
  addWithName("targetEnvironment", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("simulator");
  });
  addWithName("targetEnvironment", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("macCatalyst");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });

  addKeyword(Sink, "true", CodeCompletionKeywordKind::kw_true, "Bool");
  addKeyword(Sink, "false", CodeCompletionKeywordKind::kw_false, "Bool");
}

/// Add flags specified by '-D' to completion results.
static void addConditionalCompilationFlags(ASTContext &Ctx,
                                           CodeCompletionResultSink &Sink) {
  for (auto Flag : Ctx.LangOpts.getCustomConditionalCompilationFlags()) {
    // TODO: Should we filter out some flags?
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResultKind::Keyword,
        // FIXME: SemanticContextKind::CurrentModule is not correct.
        // Use 'None' (and fix prioritization) or introduce a new context.
        SemanticContextKind::CurrentModule);
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
    Builder.addTextChunk(Flag);
  }
}

/// Add flairs to the each item in \p results .
///
/// If \p Sink is passed, the pointer of the each result may be replaced with a
/// pointer to the new item allocated in \p Sink.
/// If \p Sink is nullptr, the pointee of each result may be modified in place.
void swift::ide::postProcessCompletionResults(
    MutableArrayRef<CodeCompletionResult *> results, CompletionKind Kind,
    const DeclContext *DC, CodeCompletionResultSink *Sink) {
  for (CodeCompletionResult *&result : results) {
    bool modified = false;
    auto flair = result->getFlair();

    // Starting a statement with a protocol name is not common. So protocol
    // names at non-type name position are "rare".
    if (result->getKind() == CodeCompletionResultKind::Declaration &&
        result->getAssociatedDeclKind() == CodeCompletionDeclKind::Protocol &&
        Kind != CompletionKind::TypeSimpleBeginning &&
        Kind != CompletionKind::TypeSimpleWithoutDot &&
        Kind != CompletionKind::TypeSimpleWithDot &&
        Kind != CompletionKind::TypeDeclResultBeginning &&
        Kind != CompletionKind::GenericRequirement) {
      flair |= CodeCompletionFlairBit::RareTypeAtCurrentPosition;
      modified = true;
    }

    // Starting a statement at top-level in non-script files is invalid.
    if (Kind == CompletionKind::StmtOrExpr &&
        result->getKind() == CodeCompletionResultKind::Declaration &&
        isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
      modified = true;
    }

    if (!modified)
      continue;

    if (Sink) {
      // Replace the result with a new result with the flair.
      result = result->withFlair(flair, *Sink);
    } else {
      // 'Sink' == nullptr means the result is modifiable in place.
      result->setFlair(flair);
    }
  }
}

void swift::ide::deliverCompletionResults(
    CodeCompletionContext &CompletionContext, CompletionLookup &Lookup,
    DeclContext *DC, CodeCompletionConsumer &Consumer) {
  auto &SF = *DC->getParentSourceFile();
  llvm::SmallPtrSet<Identifier, 8> seenModuleNames;
  std::vector<RequestedCachedModule> RequestedModules;

  SmallPtrSet<ModuleDecl *, 4> explictlyImportedModules;
  {
    // Collect modules directly imported in this SourceFile.
    SmallVector<ImportedModule, 4> directImport;
    SF.getImportedModules(directImport, ModuleDecl::getImportFilterLocal());
    for (auto import : directImport)
      explictlyImportedModules.insert(import.importedModule);

    // Exclude modules implicitly imported in the current module.
    auto implicitImports = SF.getParentModule()->getImplicitImports();
    for (auto import : implicitImports.imports)
      explictlyImportedModules.erase(import.module.importedModule);

    // Consider the current module "explicit".
    explictlyImportedModules.insert(SF.getParentModule());
  }

  for (auto &Request: Lookup.RequestedCachedResults) {
    llvm::DenseSet<CodeCompletionCache::Key> ImportsSeen;
    auto handleImport = [&](ImportedModule Import) {
      ModuleDecl *TheModule = Import.importedModule;
      ImportPath::Access Path = Import.accessPath;
      if (TheModule->getFiles().empty())
        return;

      // Clang submodules are ignored and there's no lookup cost involved,
      // so just ignore them and don't put the empty results in the cache
      // because putting a lot of objects in the cache will push out
      // other lookups.
      if (isClangSubModule(TheModule))
        return;

      std::vector<std::string> AccessPath;
      for (auto Piece : Path) {
        AccessPath.push_back(std::string(Piece.Item));
      }

      StringRef ModuleFilename = TheModule->getModuleFilename();
      // ModuleFilename can be empty if something strange happened during
      // module loading, for example, the module file is corrupted.
      if (!ModuleFilename.empty()) {
        llvm::SmallVector<std::string, 2> spiGroups;
        for (auto Import : SF.getImports()) {
          if (Import.module.importedModule == TheModule) {
            for (auto SpiGroup : Import.spiGroups) {
              spiGroups.push_back(SpiGroup.str().str());
            }
            break;
          }
        }
        llvm::sort(spiGroups);
        CodeCompletionCache::Key K{
            ModuleFilename.str(),
            std::string(TheModule->getName()),
            AccessPath,
            Request.NeedLeadingDot,
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::TestableOnly),
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::PrivateOnly),
            spiGroups,
            CompletionContext.getAddInitsToTopLevel(),
            CompletionContext.addCallWithNoDefaultArgs(),
            CompletionContext.getAnnotateResult()};

        using PairType = llvm::DenseSet<swift::ide::CodeCompletionCache::Key,
            llvm::DenseMapInfo<CodeCompletionCache::Key>>::iterator;
        std::pair<PairType, bool> Result = ImportsSeen.insert(K);
        if (!Result.second)
          return; // already handled.
        RequestedModules.push_back({std::move(K), TheModule, Request.Filter});

        auto TheModuleName = TheModule->getName();
        if (Request.Filter.contains(CodeCompletionFilterFlag::Module) &&
            (!Lookup.isHiddenModuleName(TheModuleName) ||
             explictlyImportedModules.contains(TheModule)) &&
            seenModuleNames.insert(TheModuleName).second)
          Lookup.addModuleName(TheModule);
      }
    };

    if (Request.TheModule) {
      // FIXME: actually check imports.
      for (auto Import : namelookup::getAllImports(Request.TheModule)) {
        handleImport(Import);
      }
    } else {
      // Add results from current module.
      Lookup.getToplevelCompletions(Request.Filter);

      // Add the qualifying module name
      auto curModule = SF.getParentModule();
      if (Request.Filter.contains(CodeCompletionFilterFlag::Module) &&
          seenModuleNames.insert(curModule->getName()).second)
        Lookup.addModuleName(curModule);

      // Add results for all imported modules.
      SmallVector<ImportedModule, 4> Imports;
      SF.getImportedModules(Imports, ModuleDecl::getImportFilterLocal());

      for (auto Imported : Imports) {
        for (auto Import : namelookup::getAllImports(Imported.importedModule))
          handleImport(Import);
      }
    }
  }
  Lookup.RequestedCachedResults.clear();
  CompletionContext.typeContextKind = Lookup.typeContextKind();

  postProcessCompletionResults(CompletionContext.getResultSink().Results,
                               CompletionContext.CodeCompletionKind, DC,
                               /*Sink=*/nullptr);

  Consumer.handleResultsAndModules(CompletionContext, RequestedModules,
                                   Lookup.getExpectedTypeContext(), DC,
                                   Lookup.canCurrDeclContextHandleAsync());
}

bool CodeCompletionCallbacksImpl::trySolverCompletion(bool MaybeFuncBody) {
  assert(ParsedExpr || CurDeclContext);

  SourceLoc CompletionLoc = ParsedExpr
                                ? ParsedExpr->getLoc()
                                : CurDeclContext->getASTContext()
                                      .SourceMgr.getIDEInspectionTargetLoc();

  auto typeCheckWithLookup = [this, &CompletionLoc](
                                 TypeCheckCompletionCallback &Lookup) {
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    if (AttrWithCompletion) {
      /// The attribute might not be attached to the AST if there is no var
      /// decl it could be attached to. Type check it standalone.

      // First try to check it as an attached macro.
      auto resolvedMacro = evaluateOrDefault(
          CurDeclContext->getASTContext().evaluator,
          ResolveMacroRequest{AttrWithCompletion, CurDeclContext},
          ConcreteDeclRef());

      // If that fails, type check as a call to the attribute's type. This is
      // how, e.g., property wrappers are modelled.
      if (!resolvedMacro) {
        ASTNode Call = CallExpr::create(
            CurDeclContext->getASTContext(), AttrWithCompletion->getTypeExpr(),
            AttrWithCompletion->getArgs(), /*implicit=*/true);
        typeCheckContextAt(
            TypeCheckASTNodeAtLocContext::node(CurDeclContext, Call),
            CompletionLoc);
      }
    } else {
      typeCheckContextAt(
          TypeCheckASTNodeAtLocContext::declContext(CurDeclContext),
          CompletionLoc);
    }

    // This (hopefully) only happens in cases where the expression isn't
    // typechecked during normal compilation either (e.g. member completion in a
    // switch case where there control expression is invalid). Having normal
    // typechecking still resolve even these cases would be beneficial for
    // tooling in general though.
    if (!Lookup.gotCallback()) {
      if (Context.TypeCheckerOpts.DebugConstraintSolver) {
        llvm::errs() << "--- Fallback typecheck for code completion ---\n";
      }
      Lookup.fallbackTypeCheck(CurDeclContext);
    }
  };

  switch (Kind) {
  case CompletionKind::DotExpr: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    PostfixCompletionCallback Lookup(CodeCompleteTokenExpr, CurDeclContext);
    typeCheckWithLookup(Lookup);

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

    Expr *CheckedBase = CodeCompleteTokenExpr->getBase();
    Lookup.deliverResults(CheckedBase, CurDeclContext, DotLoc,
                          isInsideObjCSelector(), CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::UnresolvedMember: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    UnresolvedMemberTypeCheckCompletionCallback Lookup(CodeCompleteTokenExpr,
                                                       CurDeclContext);
    typeCheckWithLookup(Lookup);

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);
    Lookup.deliverResults(CurDeclContext, DotLoc, CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::KeyPathExprSwift: {
    assert(CurDeclContext);

    // CodeCompletionCallbacks::completeExprKeyPath takes a \c KeyPathExpr,
    // so we can safely cast the \c ParsedExpr back to a \c KeyPathExpr.
    auto KeyPath = cast<KeyPathExpr>(ParsedExpr);
    KeyPathTypeCheckCompletionCallback Lookup(KeyPath);
    typeCheckWithLookup(Lookup);

    Lookup.deliverResults(CurDeclContext, DotLoc, CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::CallArg: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);
    ArgumentTypeCheckCompletionCallback Lookup(CodeCompleteTokenExpr,
                                               CurDeclContext);
    typeCheckWithLookup(Lookup);

    Lookup.deliverResults(ShouldCompleteCallPatternAfterParen, CompletionLoc,
                          CurDeclContext, CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::AccessorBeginning:
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::ForEachSequence:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::StmtOrExpr: {
    assert(CurDeclContext);

    bool AddUnresolvedMemberCompletions =
        (Kind == CompletionKind::CaseStmtBeginning);
    ExprTypeCheckCompletionCallback Lookup(
        CodeCompleteTokenExpr, CurDeclContext, AddUnresolvedMemberCompletions);
    if (CodeCompleteTokenExpr) {
      // 'CodeCompletionTokenExpr == nullptr' happens when completing e.g.
      //   var x: Int {
      //     get { ... }
      //     #^COMPLETE^#
      //   }
      // In this case we don't want to provide any expression results. We still
      // need to have a TypeCheckCompletionCallback so we can call
      // deliverResults on it to deliver the keyword results from the completion
      // context's result sink to the consumer.
      typeCheckWithLookup(Lookup);
    }

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

    SourceLoc CCLoc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.deliverResults(CCLoc, CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::AfterPoundExpr: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    AfterPoundExprCompletion Lookup(CodeCompleteTokenExpr, CurDeclContext,
                                    ParentStmtKind);
    typeCheckWithLookup(Lookup);

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

    Lookup.deliverResults(CompletionContext, Consumer);
    return true;
  }
  default:
    return false;
  }
}

// Undoes the single-expression closure/function body transformation on the
// given DeclContext and its parent contexts if they have a single expression
// body that contains the code completion location.
//
// FIXME: Remove this once all expression position completions are migrated
// to work via TypeCheckCompletionCallback.
static void undoSingleExpressionReturn(DeclContext *DC) {
  auto updateBody = [](BraceStmt *BS, ASTContext &Ctx) -> bool {
    ASTNode LastElem = BS->getLastElement();
    auto *RS = dyn_cast_or_null<ReturnStmt>(LastElem.dyn_cast<Stmt*>());

    if (!RS || !RS->isImplicit())
      return false;

    BS->setLastElement(RS->getResult());
    return true;
  };

  while (ClosureExpr *CE = dyn_cast_or_null<ClosureExpr>(DC)) {
    if (CE->hasSingleExpressionBody()) {
      if (updateBody(CE->getBody(), CE->getASTContext()))
        CE->setBody(CE->getBody(), false);
    }
    DC = DC->getParent();
  }
  if (FuncDecl *FD = dyn_cast_or_null<FuncDecl>(DC)) {
    if (FD->hasSingleExpressionBody()) {
      if (updateBody(FD->getBody(), FD->getASTContext()))
        FD->setHasSingleExpressionBody(false);
    }
  }
}

void CodeCompletionCallbacksImpl::doneParsing(SourceFile *SrcFile) {
  CompletionContext.CodeCompletionKind = Kind;

  if (Kind == CompletionKind::None) {
    return;
  }

  bool MaybeFuncBody = true;
  if (CurDeclContext) {
    auto *CD = CurDeclContext->getLocalContext();
    if (!CD || CD->getContextKind() == DeclContextKind::Initializer ||
        CD->getContextKind() == DeclContextKind::TopLevelCodeDecl)
      MaybeFuncBody = false;
  }

  if (auto *DC = dyn_cast_or_null<DeclContext>(ParsedDecl)) {
    if (DC->isChildContextOf(CurDeclContext))
      CurDeclContext = DC;
  }

  if (trySolverCompletion(MaybeFuncBody))
    return;

  undoSingleExpressionReturn(CurDeclContext);
  if (Kind != CompletionKind::TypeSimpleWithDot) {
    // Type member completion does not need a type-checked AST.
    typeCheckContextAt(
        TypeCheckASTNodeAtLocContext::declContext(CurDeclContext),
        ParsedExpr ? ParsedExpr->getLoc()
                   : CurDeclContext->getASTContext()
                         .SourceMgr.getIDEInspectionTargetLoc());
  }

  // Add keywords even if type checking fails completely.
  addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

  llvm::Optional<Type> ExprType;
  ConcreteDeclRef ReferencedDecl = nullptr;
  if (ParsedExpr) {
    if (auto *checkedExpr = findParsedExpr(CurDeclContext,
                                           ParsedExpr->getSourceRange())) {
      ParsedExpr = checkedExpr;
    }

    if (auto typechecked = typeCheckParsedExpr()) {
      ExprType = typechecked->first;
      ReferencedDecl = typechecked->second;
      ParsedExpr->setType(*ExprType);
    }

    if (!ExprType && Kind != CompletionKind::PostfixExprParen &&
        Kind != CompletionKind::CallArg &&
        Kind != CompletionKind::KeyPathExprObjC)
      return;
  }

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext.getResultSink(), P.Context,
                          CurDeclContext, &CompletionContext);
  if (ExprType) {
    Lookup.setIsStaticMetatype(ParsedExpr->isStaticallyDerivedMetatype());
  }
  if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(ParsedExpr)) {
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Context.Id_self);
  } else if (isa_and_nonnull<SuperRefExpr>(ParsedExpr)) {
    Lookup.setIsSuperRefExpr();
  }

  if (isInsideObjCSelector())
    Lookup.includeInstanceMembers();
  if (PreferFunctionReferencesToCalls)
    Lookup.setPreferFunctionReferencesToCalls();

  auto DoPostfixExprBeginning = [&] (){
    SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(Loc, /*isForDeclResult=*/false);
  };

  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::CallArg:
  case CompletionKind::StmtOrExpr:
  case CompletionKind::ForEachSequence:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::AfterPoundExpr:
  case CompletionKind::AccessorBeginning:
  case CompletionKind::CaseStmtBeginning:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::PostfixExpr: {
    Lookup.setHaveLeadingSpace(HasSpace);
    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    /// We set the type of ParsedExpr explicitly above. But we don't want an
    /// unresolved type in our AST when we type check again for operator
    /// completions. Remove the type of the ParsedExpr and see if we can come up
    /// with something more useful based on the the full sequence expression.
    if (ParsedExpr->getType()->is<UnresolvedType>()) {
      ParsedExpr->setType(nullptr);
    }
    Lookup.getOperatorCompletions(ParsedExpr, leadingSequenceExprs);
    Lookup.getPostfixKeywordCompletions(*ExprType, ParsedExpr);
    break;
  }

  case CompletionKind::PostfixExprParen: {
    Lookup.setHaveLParen(true);

    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    if (ShouldCompleteCallPatternAfterParen) {
      ExprContextInfo ParentContextInfo(CurDeclContext, ParsedExpr);
      Lookup.setExpectedTypes(
          ParentContextInfo.getPossibleTypes(),
          ParentContextInfo.isImplicitSingleExpressionReturn());
      if (!ContextInfo.getPossibleCallees().empty()) {
        for (auto &typeAndDecl : ContextInfo.getPossibleCallees())
          Lookup.tryFunctionCallCompletions(typeAndDecl.Type, typeAndDecl.Decl,
                                            typeAndDecl.SemanticContext);
      } else if (ExprType && ((*ExprType)->is<AnyFunctionType>() ||
                              (*ExprType)->is<AnyMetatypeType>())) {
        Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
      }
    } else {
      // Add argument labels, then fallthrough to get values.
      Lookup.addCallArgumentCompletionResults(ContextInfo.getPossibleParams());
    }

    if (!Lookup.FoundFunctionCalls ||
        (Lookup.FoundFunctionCalls &&
         Lookup.FoundFunctionsWithoutFirstKeyword)) {
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());
      Lookup.setHaveLParen(false);

      // Add any keywords that can be used in an argument expr position.
      addSuperKeyword(CompletionContext.getResultSink(), CurDeclContext);
      addExprKeywords(CompletionContext.getResultSink(), CurDeclContext);

      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::KeyPathExprObjC: {
    if (DotLoc.isValid())
      Lookup.setHaveDot(DotLoc);
    Lookup.setIsKeyPathExpr();
    Lookup.includeInstanceMembers();

    if (ExprType) {
      if (isDynamicLookup(*ExprType))
        Lookup.setIsDynamicLookup();

      Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    } else {
      SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
      Lookup.getValueCompletionsInDeclContext(Loc, KeyPathFilter,
                                              /*LiteralCompletions=*/false);
    }
    break;
  }

  case CompletionKind::TypeDeclResultBeginning:
  case CompletionKind::TypeSimpleBeginning: {
    auto Loc = Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.getTypeCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(
        Loc, Kind == CompletionKind::TypeDeclResultBeginning);
    break;
  }

  case CompletionKind::TypeSimpleWithDot: {
    Lookup.setHaveDot(SourceLoc());
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeSimpleWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::NominalMemberBeginning: {
    CompletionOverrideLookup OverrideLookup(CompletionContext.getResultSink(),
                                            P.Context, CurDeclContext,
                                            ParsedKeywords, introducerLoc);
    OverrideLookup.getOverrideCompletions(SourceLoc());
    break;
  }

  case CompletionKind::AttributeBegin: {
    Lookup.getAttributeDeclCompletions(IsInSil, AttTargetDK);
    OptionSet<CustomAttributeKind> ExpectedCustomAttributeKinds;
    if (AttTargetDK) {
      switch (*AttTargetDK) {
      case DeclKind::Var:
        ExpectedCustomAttributeKinds |= CustomAttributeKind::GlobalActor;
        LLVM_FALLTHROUGH;
      case DeclKind::Param:
        ExpectedCustomAttributeKinds |= CustomAttributeKind::ResultBuilder;
        ExpectedCustomAttributeKinds |= CustomAttributeKind::PropertyWrapper;
        break;
      case DeclKind::Func:
        ExpectedCustomAttributeKinds |= CustomAttributeKind::ResultBuilder;
        ExpectedCustomAttributeKinds |= CustomAttributeKind::GlobalActor;
        break;
      default:
        break;
      }

      switch (*AttTargetDK) {
      case DeclKind::Var:
      case DeclKind::Subscript:
        ExpectedCustomAttributeKinds |= CustomAttributeKind::VarMacro;
        break;
      case DeclKind::Struct:
      case DeclKind::Class:
      case DeclKind::Protocol:
      case DeclKind::Enum:
      case DeclKind::Extension:
        ExpectedCustomAttributeKinds |= CustomAttributeKind::ContextMacro;
        break;
      default:
        break;
      }
      if (*AttTargetDK != DeclKind::Param) {
        ExpectedCustomAttributeKinds |= CustomAttributeKind::DeclMacro;
      }
    } else {
      // If we don't know on which decl kind we are completing, suggest all
      // attribute kinds.
      ExpectedCustomAttributeKinds |= CustomAttributeKind::PropertyWrapper;
      ExpectedCustomAttributeKinds |= CustomAttributeKind::ResultBuilder;
      ExpectedCustomAttributeKinds |= CustomAttributeKind::GlobalActor;
      ExpectedCustomAttributeKinds |= CustomAttributeKind::VarMacro;
      ExpectedCustomAttributeKinds |= CustomAttributeKind::ContextMacro;
      ExpectedCustomAttributeKinds |= CustomAttributeKind::DeclMacro;
    }

    Lookup.setExpectedTypes(/*Types=*/{},
                            /*isImplicitSingleExpressionReturn=*/false,
                            /*preferNonVoid=*/false,
                            ExpectedCustomAttributeKinds);

    // TypeName at attribute position after '@'.
    // - VarDecl: Property Wrappers.
    // - ParamDecl/VarDecl/FuncDecl: Result Builders.
    if (!AttTargetDK || *AttTargetDK == DeclKind::Var ||
        *AttTargetDK == DeclKind::Param || *AttTargetDK == DeclKind::Func)
      Lookup.getTypeCompletionsInDeclContext(
          P.Context.SourceMgr.getIDEInspectionTargetLoc());

    // Macro name at attribute position after '@'.
    CodeCompletionMacroRoles macroRoles =
        getCompletionMacroRoles(ExpectedCustomAttributeKinds);
    if (macroRoles) {
      Lookup.getMacroCompletions(macroRoles);
    }
    break;
  }
  case CompletionKind::AttributeDeclParen: {
    Lookup.getAttributeDeclParamCompletions(AttrKind, AttrParamIndex,
                                            AttrParamHasLabel);
    break;
  }
  case CompletionKind::PoundAvailablePlatform: {
    Lookup.getPoundAvailablePlatformCompletions();
    break;
  }
  case CompletionKind::Import: {
    if (DotLoc.isValid())
      Lookup.addSubModuleNames(SubModuleNameVisibilityPairs);
    else
      Lookup.addImportModuleNames();
    break;
  }
  case CompletionKind::LabeledTrailingClosure: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    SmallVector<PossibleParamInfo, 2> params;
    // Only complete function type parameters
    llvm::copy_if(ContextInfo.getPossibleParams(), std::back_inserter(params),
                  [](const PossibleParamInfo &P) {
                    // nullptr indicates out of bounds.
                    if (!P.Param)
                      return true;
                    return P.Param->getPlainType()
                        ->lookThroughAllOptionalTypes()
                        ->is<AnyFunctionType>();
                  });

    bool allRequired = false;
    if (!params.empty()) {
      Lookup.addCallArgumentCompletionResults(
          params, /*isLabeledTrailingClosure=*/true);
      allRequired = llvm::all_of(
          params, [](const PossibleParamInfo &P) { return P.IsRequired; });
    }

    // If there're optional parameters, do global completion or member
    // completion depending on the completion is happening at the start of line.
    if (!allRequired) {
      if (IsAtStartOfLine) {
        //   foo() {}
        //   <HERE>

        auto &Sink = CompletionContext.getResultSink();
        if (isa<Initializer>(CurDeclContext))
          CurDeclContext = CurDeclContext->getParent();

        if (CurDeclContext->isTypeContext()) {
          // Override completion (CompletionKind::NominalMemberBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency);
          addLetVarKeywords(Sink);
          SmallVector<StringRef, 0> ParsedKeywords;
          CompletionOverrideLookup OverrideLookup(Sink, Context, CurDeclContext,
                                                  ParsedKeywords, SourceLoc());
          OverrideLookup.getOverrideCompletions(SourceLoc());
        } else {
          // Global completion (CompletionKind::PostfixExprBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency);
          addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
          addSuperKeyword(Sink, CurDeclContext);
          addLetVarKeywords(Sink);
          addExprKeywords(Sink, CurDeclContext);
          addAnyTypeKeyword(Sink, Context.TheAnyType);
          DoPostfixExprBeginning();
        }
      } else {
        //   foo() {} <HERE>
        // Member completion.
        Expr *analyzedExpr = ContextInfo.getAnalyzedExpr();
        if (!analyzedExpr)
          break;

        // Only if the completion token is the last token in the call.
        if (analyzedExpr->getEndLoc() != CodeCompleteTokenExpr->getLoc())
          break;

        Type resultTy = analyzedExpr->getType();
        // If the call expression doesn't have a type, fallback to:
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 1) Try to type check removing CodeCompletionExpr from the call.
          Expr *removedExpr = analyzedExpr;
          removeCodeCompletionExpr(CurDeclContext->getASTContext(),
                                   removedExpr);
          ConcreteDeclRef referencedDecl;
          auto optT = getTypeOfCompletionContextExpr(
              CurDeclContext->getASTContext(), CurDeclContext,
              CompletionTypeCheckKind::Normal, removedExpr, referencedDecl);
          if (optT) {
            resultTy = *optT;
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 2) Infer it from the possible callee info.
          if (!ContextInfo.getPossibleCallees().empty()) {
            auto calleeInfo = ContextInfo.getPossibleCallees()[0];
            resultTy = calleeInfo.Type->getResult();
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 3) Give up providing postfix completions.
          break;
        }

        auto &SM = CurDeclContext->getASTContext().SourceMgr;
        auto leadingChar = SM.extractText(
            {SM.getIDEInspectionTargetLoc().getAdvancedLoc(-1), 1});
        Lookup.setHaveLeadingSpace(leadingChar.find_first_of(" \t\f\v") !=
                                   StringRef::npos);

        if (isDynamicLookup(resultTy))
          Lookup.setIsDynamicLookup();
        Lookup.getValueExprCompletions(resultTy, /*VD=*/nullptr);
        Lookup.getOperatorCompletions(analyzedExpr, leadingSequenceExprs);
        Lookup.getPostfixKeywordCompletions(resultTy, analyzedExpr);
      }
    }
    break;
  }

  case CompletionKind::ReturnStmtExpr : {
    SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    SmallVector<Type, 2> possibleReturnTypes;
    collectPossibleReturnTypesFromContext(CurDeclContext, possibleReturnTypes);
    Lookup.setExpectedTypes(possibleReturnTypes,
                            /*isImplicitSingleExpressionReturn*/ false);
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::YieldStmtExpr: {
    SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    if (auto FD = dyn_cast<AccessorDecl>(CurDeclContext)) {
      if (FD->isCoroutine()) {
        // TODO: handle multi-value yields.
        Lookup.setExpectedTypes(FD->getStorage()->getValueInterfaceType(),
                                /*isImplicitSingleExpressionReturn*/ false);
      }
    }
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::AfterPoundDirective: {
    addPoundDirectives(CompletionContext.getResultSink());

    CodeCompletionMacroRoles roles;
    if (!CurDeclContext || !CurDeclContext->isTypeContext()) {
      roles |= CodeCompletionMacroRole::Expression;
      roles |= CodeCompletionMacroRole::CodeItem;
    }
    roles |= CodeCompletionMacroRole::Declaration;
    Lookup.getMacroCompletions(roles);

    // FIXME: Add pound expressions (e.g. '#selector()') if it's at statements
    // position.
    break;
  }

  case CompletionKind::PlatformConditon: {
    addPlatformConditions(CompletionContext.getResultSink());
    addConditionalCompilationFlags(CurDeclContext->getASTContext(),
                                   CompletionContext.getResultSink());
    break;
  }

  case CompletionKind::GenericRequirement: {
    auto Loc = Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.getGenericRequirementCompletions(CurDeclContext, Loc);
    break;
  }
  case CompletionKind::PrecedenceGroup:
    Lookup.getPrecedenceGroupCompletions(SyntxKind);
    break;
  case CompletionKind::StmtLabel: {
    SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.getStmtLabelCompletions(Loc, ParentStmtKind == StmtKind::Continue);
    break;
  }
  case CompletionKind::TypeAttrBeginning: {
    Lookup.getTypeAttributeKeywordCompletions();

    // Type names at attribute position after '@'.
    Lookup.getTypeCompletionsInDeclContext(
      P.Context.SourceMgr.getIDEInspectionTargetLoc());
    break;

  }
  case CompletionKind::OptionalBinding: {
    SourceLoc Loc = P.Context.SourceMgr.getIDEInspectionTargetLoc();
    Lookup.getOptionalBindingCompletions(Loc);
    break;
  }

  case CompletionKind::WithoutConstraintType: {
    Lookup.getWithoutConstraintTypes();
    break;
  }

  case CompletionKind::AfterIfStmtElse:
  case CompletionKind::CaseStmtKeyword:
  case CompletionKind::EffectsSpecifier:
  case CompletionKind::ForEachPatternBeginning:
  case CompletionKind::ForEachInKw:
    // Handled earlier by keyword completions.
    break;
  }

  deliverCompletionResults(CompletionContext, Lookup, CurDeclContext, Consumer);
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public IDEInspectionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  Callbacks createCallbacks(Parser &P) override {
    auto callbacks = std::make_shared<CodeCompletionCallbacksImpl>(
        P, CompletionContext, Consumer);
    return {callbacks, callbacks};
  }
};
} // end anonymous namespace

IDEInspectionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

void swift::ide::lookupCodeCompletionResultsFromModule(
    CodeCompletionResultSink &targetSink, const ModuleDecl *module,
    ArrayRef<std::string> accessPath, bool needLeadingDot,
    const SourceFile *SF) {
  // Use the SourceFile as the decl context, to avoid decl context specific
  // behaviors.
  CompletionLookup Lookup(targetSink, module->getASTContext(), SF);
  Lookup.lookupExternalModuleDecls(module, accessPath, needLeadingDot);
}

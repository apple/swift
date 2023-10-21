#include "swift/AST/CASTBridging.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"

using namespace swift;

namespace {
struct BridgedDiagnosticImpl {
  typedef llvm::MallocAllocator Allocator;

  InFlightDiagnostic inFlight;
  std::vector<StringRef> textBlobs;

  BridgedDiagnosticImpl(InFlightDiagnostic inFlight,
                        std::vector<StringRef> textBlobs)
      : inFlight(std::move(inFlight)), textBlobs(std::move(textBlobs)) {}

  BridgedDiagnosticImpl(const BridgedDiagnosticImpl &) = delete;
  BridgedDiagnosticImpl(BridgedDiagnosticImpl &&) = delete;
  BridgedDiagnosticImpl &operator=(const BridgedDiagnosticImpl &) = delete;
  BridgedDiagnosticImpl &operator=(BridgedDiagnosticImpl &&) = delete;

  ~BridgedDiagnosticImpl() {
    inFlight.flush();

    Allocator allocator;
    for (auto text : textBlobs) {
      allocator.Deallocate(text.data(), text.size());
    }
  }
};
} // namespace

template <typename T>
static inline llvm::ArrayRef<T> convertArrayRef(const BridgedArrayRef bridged) {
  return {static_cast<const T *>(bridged.data), size_t(bridged.numElements)};
}

static inline StringRef convertString(BridgedString cStr) {
  return StringRef{reinterpret_cast<const char *>(cStr.data),
                   size_t(cStr.length)};
}

static inline ASTContext &convertASTContext(BridgedASTContext cContext) {
  return *static_cast<ASTContext *>(cContext.raw);
}

static inline DeclContext *convertDeclContext(BridgedDeclContext cDeclContext) {
  return static_cast<DeclContext *>(cDeclContext.raw);
}

static inline BridgedDeclContext bridgeDeclContext(DeclContext *declContext) {
  return BridgedDeclContext{declContext};
}

static inline SourceLoc convertSourceLoc(BridgedSourceLoc cLoc) {
  auto smLoc = llvm::SMLoc::getFromPointer(static_cast<const char *>(cLoc.raw));
  return SourceLoc(smLoc);
}

static inline SourceRange convertSourceRange(BridgedSourceRange cRange) {
  return SourceRange(convertSourceLoc(cRange.startLoc),
                     convertSourceLoc(cRange.endLoc));
}

static inline Identifier convertIdentifier(BridgedIdentifier cIdentifier) {
  return Identifier::getFromOpaquePointer(const_cast<void *>(cIdentifier.raw));
}

static inline BridgedDiagnosticImpl *
convertDiagnostic(BridgedDiagnostic cDiag) {
  return static_cast<BridgedDiagnosticImpl *>(cDiag.raw);
}

static inline DiagnosticEngine &
convertDiagnosticEngine(BridgedDiagnosticEngine cEngine) {
  return *static_cast<DiagnosticEngine *>(cEngine.raw);
}

static inline TypeAttributes *
convertTypeAttributes(BridgedTypeAttributes cAttributes) {
  return static_cast<TypeAttributes *>(cAttributes.raw);
}

static TypeAttrKind convertTypeAttrKind(BridgedTypeAttrKind kind) {
  switch (kind) {
#define TYPE_ATTR(X)                                                           \
  case BridgedTypeAttrKind_##X:                                                \
    return TAK_##X;
#include "swift/AST/Attr.def"
  case BridgedTypeAttrKind_Count:
    return TAK_Count;
  }
}

BridgedDiagnostic Diagnostic_create(BridgedDiagnosticEngine cDiags,
                                    BridgedDiagnosticSeverity severity,
                                    BridgedSourceLoc cLoc,
                                    BridgedString cText) {
  StringRef origText = convertString(cText);
  BridgedDiagnosticImpl::Allocator alloc;
  StringRef text = origText.copy(alloc);

  SourceLoc loc = convertSourceLoc(cLoc);

  Diag<StringRef> diagID;
  switch (severity) {
  case BridgedDiagnosticSeverity::BridgedError:
    diagID = diag::bridged_error;
    break;
  case BridgedDiagnosticSeverity::BridgedFatalError:
    diagID = diag::bridged_fatal_error;
    break;
  case BridgedDiagnosticSeverity::BridgedNote:
    diagID = diag::bridged_note;
    break;
  case BridgedDiagnosticSeverity::BridgedRemark:
    diagID = diag::bridged_remark;
    break;
  case BridgedDiagnosticSeverity::BridgedWarning:
    diagID = diag::bridged_warning;
    break;
  }

  DiagnosticEngine &diags = convertDiagnosticEngine(cDiags);
  return {new BridgedDiagnosticImpl{diags.diagnose(loc, diagID, text), {text}}};
}

/// Highlight a source range as part of the diagnostic.
void Diagnostic_highlight(BridgedDiagnostic cDiag, BridgedSourceLoc cStartLoc,
                          BridgedSourceLoc cEndLoc) {
  SourceLoc startLoc = convertSourceLoc(cStartLoc);
  SourceLoc endLoc = convertSourceLoc(cEndLoc);

  BridgedDiagnosticImpl *diag = convertDiagnostic(cDiag);
  diag->inFlight.highlightChars(startLoc, endLoc);
}

/// Add a Fix-It to replace a source range as part of the diagnostic.
void Diagnostic_fixItReplace(BridgedDiagnostic cDiag,
                             BridgedSourceLoc cStartLoc,
                             BridgedSourceLoc cEndLoc,
                             BridgedString cReplaceText) {

  SourceLoc startLoc = convertSourceLoc(cStartLoc);
  SourceLoc endLoc = convertSourceLoc(cEndLoc);

  StringRef origReplaceText = convertString(cReplaceText);
  BridgedDiagnosticImpl::Allocator alloc;
  StringRef replaceText = origReplaceText.copy(alloc);

  BridgedDiagnosticImpl *diag = convertDiagnostic(cDiag);
  diag->textBlobs.push_back(replaceText);
  diag->inFlight.fixItReplaceChars(startLoc, endLoc, replaceText);
}

/// Finish the given diagnostic and emit it.
void Diagnostic_finish(BridgedDiagnostic cDiag) {
  BridgedDiagnosticImpl *diag = convertDiagnostic(cDiag);
  delete diag;
}

BridgedIdentifier ASTContext_getIdentifier(BridgedASTContext cContext,
                                           BridgedString cStr) {
  StringRef str = convertString(cStr);
  if (str.size() == 1 && str.front() == '_')
    return BridgedIdentifier();

  // If this was a back-ticked identifier, drop the back-ticks.
  if (str.size() >= 2 && str.front() == '`' && str.back() == '`') {
    str = str.drop_front().drop_back();
  }

  return {convertASTContext(cContext).getIdentifier(str).getAsOpaquePointer()};
}

bool ASTContext_langOptsHasFeature(BridgedASTContext cContext,
                                   BridgedFeature feature) {
  return convertASTContext(cContext).LangOpts.hasFeature((Feature)feature);
}

BridgedSourceLoc SourceLoc_advanced(BridgedSourceLoc cLoc, size_t len) {
  SourceLoc loc = convertSourceLoc(cLoc).getAdvancedLoc(len);
  return {loc.getOpaquePointerValue()};
}

void *TopLevelCodeDecl_createStmt(BridgedASTContext cContext,
                                  BridgedDeclContext cDeclContext,
                                  BridgedSourceLoc cStartLoc, void *statement,
                                  BridgedSourceLoc cEndLoc) {
  ASTContext &context = convertASTContext(cContext);
  DeclContext *declContext = convertDeclContext(cDeclContext);

  auto *S = static_cast<Stmt *>(statement);
  auto Brace = BraceStmt::create(context, convertSourceLoc(cStartLoc), {S},
                                 convertSourceLoc(cEndLoc),
                                 /*Implicit=*/true);
  auto *TLCD = new (context) TopLevelCodeDecl(declContext, Brace);
  return (Decl *)TLCD;
}

void *TopLevelCodeDecl_createExpr(BridgedASTContext cContext,
                                  BridgedDeclContext cDeclContext,
                                  BridgedSourceLoc cStartLoc, void *expression,
                                  BridgedSourceLoc cEndLoc) {
  ASTContext &context = convertASTContext(cContext);
  DeclContext *declContext = convertDeclContext(cDeclContext);

  auto *E = static_cast<Expr *>(expression);
  auto Brace = BraceStmt::create(context, convertSourceLoc(cStartLoc), {E},
                                 convertSourceLoc(cEndLoc),
                                 /*Implicit=*/true);
  auto *TLCD = new (context) TopLevelCodeDecl(declContext, Brace);
  return (Decl *)TLCD;
}

void *SequenceExpr_create(BridgedASTContext cContext, BridgedArrayRef exprs) {
  return SequenceExpr::create(convertASTContext(cContext),
                              convertArrayRef<Expr *>(exprs));
}

void *TupleExpr_create(BridgedASTContext cContext, BridgedSourceLoc cLParen,
                       BridgedArrayRef subs, BridgedArrayRef names,
                       BridgedArrayRef cNameLocs, BridgedSourceLoc cRParen) {
  ASTContext &context = convertASTContext(cContext);
  return TupleExpr::create(
      context, convertSourceLoc(cLParen), convertArrayRef<Expr *>(subs),
      convertArrayRef<Identifier>(names), convertArrayRef<SourceLoc>(cNameLocs),
      convertSourceLoc(cRParen),
      /*Implicit*/ false);
}

void *FunctionCallExpr_create(BridgedASTContext cContext, void *fn,
                              void *args) {
  ASTContext &context = convertASTContext(cContext);
  TupleExpr *TE = static_cast<TupleExpr *>(args);
  SmallVector<Argument, 8> arguments;
  for (unsigned i = 0; i < TE->getNumElements(); ++i) {
    arguments.emplace_back(TE->getElementNameLoc(i), TE->getElementName(i),
                           TE->getElement(i));
  }
  auto *argList = ArgumentList::create(context, TE->getLParenLoc(), arguments,
                                       TE->getRParenLoc(), llvm::None,
                                       /*isImplicit*/ false);
  return CallExpr::create(context, static_cast<Expr *>(fn), argList,
                          /*implicit*/ false);
}

void *IdentifierExpr_create(BridgedASTContext cContext, BridgedIdentifier base,
                            BridgedSourceLoc cLoc) {
  ASTContext &context = convertASTContext(cContext);
  auto name = DeclNameRef{convertIdentifier(base)};
  Expr *E = new (context) UnresolvedDeclRefExpr(
      name, DeclRefKind::Ordinary, DeclNameLoc{convertSourceLoc(cLoc)});
  return E;
}

void *StringLiteralExpr_create(BridgedASTContext cContext, BridgedString cStr,
                               BridgedSourceLoc cTokenLoc) {
  ASTContext &context = convertASTContext(cContext);
  auto str = context.AllocateCopy(convertString(cStr));
  return new (context) StringLiteralExpr(str, convertSourceLoc(cTokenLoc));
}

void *IntegerLiteralExpr_create(BridgedASTContext cContext, BridgedString cStr,
                                BridgedSourceLoc cTokenLoc) {
  ASTContext &context = convertASTContext(cContext);
  auto str = context.AllocateCopy(convertString(cStr));
  return new (context) IntegerLiteralExpr(str, convertSourceLoc(cTokenLoc));
}

void *ArrayExpr_create(BridgedASTContext cContext, BridgedSourceLoc cLLoc,
                       BridgedArrayRef elements, BridgedArrayRef commas,
                       BridgedSourceLoc cRLoc) {
  ASTContext &context = convertASTContext(cContext);
  return ArrayExpr::create(
      context, convertSourceLoc(cLLoc), convertArrayRef<Expr *>(elements),
      convertArrayRef<SourceLoc>(commas), convertSourceLoc(cRLoc));
}

void *BooleanLiteralExpr_create(BridgedASTContext cContext, bool value,
                                BridgedSourceLoc cTokenLoc) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) BooleanLiteralExpr(value, convertSourceLoc(cTokenLoc));
}

void *NilLiteralExpr_create(BridgedASTContext cContext,
                            BridgedSourceLoc cNilKeywordLoc) {
  Expr *e = new (convertASTContext(cContext))
      NilLiteralExpr(convertSourceLoc(cNilKeywordLoc));
  return e;
}

void *VarDecl_create(BridgedASTContext cContext,
                     BridgedDeclContext cDeclContext,
                     BridgedSourceLoc cBindingKeywordLoc, void *opaqueNameExpr,
                     void *opaqueInitExpr, bool isStatic, bool isLet) {
  ASTContext &context = convertASTContext(cContext);
  DeclContext *declContext = convertDeclContext(cDeclContext);

  auto *name = static_cast<UnresolvedDeclRefExpr *>(opaqueNameExpr);
  auto *varDecl = new (context) VarDecl(
      isStatic, isLet ? VarDecl::Introducer::Let : VarDecl::Introducer::Var,
      name->getLoc(), name->getName().getBaseIdentifier(), declContext);
  auto *pattern = new (context) NamedPattern(varDecl);
  return PatternBindingDecl::create(
      context,
      /*StaticLoc=*/SourceLoc(), // FIXME
      isStatic ? StaticSpellingKind::KeywordStatic : StaticSpellingKind::None,
      convertSourceLoc(cBindingKeywordLoc), pattern,
      /*EqualLoc=*/SourceLoc(), // FIXME
      static_cast<Expr *>(opaqueInitExpr), declContext);
}

void *SingleValueStmtExpr_createWithWrappedBranches(
    BridgedASTContext cContext, void *S, BridgedDeclContext cDeclContext,
    bool mustBeExpr) {
  ASTContext &context = convertASTContext(cContext);
  DeclContext *declContext = convertDeclContext(cDeclContext);
  return SingleValueStmtExpr::createWithWrappedBranches(
      context, (Stmt *)S, declContext, mustBeExpr);
}

void *IfStmt_create(BridgedASTContext cContext, BridgedSourceLoc cIfLoc,
                    void *cond, void *_Nullable then, BridgedSourceLoc cElseLoc,
                    void *_Nullable elseStmt) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      IfStmt(convertSourceLoc(cIfLoc), (Expr *)cond, (Stmt *)then,
             convertSourceLoc(cElseLoc), (Stmt *)elseStmt, llvm::None, context);
}

void *ReturnStmt_create(BridgedASTContext cContext, BridgedSourceLoc cLoc,
                        void *_Nullable expr) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) ReturnStmt(convertSourceLoc(cLoc), (Expr *)expr);
}

void *BraceStmt_create(BridgedASTContext cContext, BridgedSourceLoc cLBLoc,
                       BridgedArrayRef elements, BridgedSourceLoc cRBLoc) {
  llvm::SmallVector<ASTNode, 6> nodes;
  for (auto node : convertArrayRef<BridgedASTNode>(elements)) {
    if (node.kind == ASTNodeKindExpr) {
      auto expr = (Expr *)node.ptr;
      nodes.push_back(expr);
    } else if (node.kind == ASTNodeKindStmt) {
      auto stmt = (Stmt *)node.ptr;
      nodes.push_back(stmt);
    } else {
      assert(node.kind == ASTNodeKindDecl);
      auto decl = (Decl *)node.ptr;
      nodes.push_back(decl);

      // Variable declarations are part of the list on par with pattern binding
      // declarations per the legacy parser.
      if (auto *bindingDecl = dyn_cast<PatternBindingDecl>(decl)) {
        for (auto i : range(bindingDecl->getNumPatternEntries())) {
          bindingDecl->getPattern(i)->forEachVariable(
              [&nodes](VarDecl *variable) {
            nodes.push_back(variable);
          });
        }
      }
    }
  }

  ASTContext &context = convertASTContext(cContext);
  return BraceStmt::create(context, convertSourceLoc(cLBLoc),
                           context.AllocateCopy(nodes),
                           convertSourceLoc(cRBLoc));
}

void AbstractFunctionDecl_setBody(void *opaqueBody, void *opaqueDecl) {
  auto *decl = static_cast<Decl *>(opaqueDecl);

  cast<AbstractFunctionDecl>(decl)->setBody(
      static_cast<BraceStmt *>(opaqueBody), FuncDecl::BodyKind::Parsed);
}

void *SimpleIdentTypeRepr_create(BridgedASTContext cContext,
                                 BridgedSourceLoc cLoc, BridgedIdentifier id) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) SimpleIdentTypeRepr(DeclNameLoc(convertSourceLoc(cLoc)),
                                           DeclNameRef(convertIdentifier(id)));
}

void *GenericIdentTypeRepr_create(BridgedASTContext cContext,
                                  BridgedIdentifier name,
                                  BridgedSourceLoc cNameLoc,
                                  BridgedArrayRef genericArgs,
                                  BridgedSourceLoc cLAngleLoc,
                                  BridgedSourceLoc cRAngleLoc) {
  ASTContext &context = convertASTContext(cContext);
  auto Loc = DeclNameLoc(convertSourceLoc(cNameLoc));
  auto Name = DeclNameRef(convertIdentifier(name));
  SourceLoc lAngleLoc = convertSourceLoc(cLAngleLoc);
  SourceLoc rAngleLoc = convertSourceLoc(cRAngleLoc);
  return GenericIdentTypeRepr::create(context, Loc, Name,
                                      convertArrayRef<TypeRepr *>(genericArgs),
                                      SourceRange{lAngleLoc, rAngleLoc});
}

void *UnresolvedDotExpr_create(BridgedASTContext cContext, void *base,
                               BridgedSourceLoc cDotLoc, BridgedIdentifier name,
                               BridgedSourceLoc cNameLoc) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      UnresolvedDotExpr((Expr *)base, convertSourceLoc(cDotLoc),
                        DeclNameRef(convertIdentifier(name)),
                        DeclNameLoc(convertSourceLoc(cNameLoc)), false);
}

void *ClosureExpr_create(BridgedASTContext cContext, void *body,
                         BridgedDeclContext cDeclContext) {
  DeclAttributes attributes;
  SourceRange bracketRange;
  SourceLoc asyncLoc;
  SourceLoc throwsLoc;
  SourceLoc arrowLoc;
  SourceLoc inLoc;

  ASTContext &context = convertASTContext(cContext);
  DeclContext *declContext = convertDeclContext(cDeclContext);

  auto params = ParameterList::create(context, inLoc, {}, inLoc);

  auto *out = new (context)
      ClosureExpr(attributes, bracketRange, nullptr, nullptr, asyncLoc,
                  throwsLoc, /*FIXME:thrownType=*/nullptr, arrowLoc, inLoc,
                  nullptr, declContext);
  out->setBody((BraceStmt *)body, true);
  out->setParameterList(params);
  return (Expr *)out;
}

void IterableDeclContext_setParsedMembers(BridgedArrayRef bridgedMembers,
                                          void *opaqueDecl) {
  auto *decl = static_cast<Decl *>(opaqueDecl);
  auto &ctx = decl->getASTContext();

  SmallVector<Decl *> members;
  for (auto *decl : convertArrayRef<Decl *>(bridgedMembers)) {
    members.push_back(decl);
    // Each enum case element is also part of the members list according to the
    // legacy parser.
    if (auto *ECD = dyn_cast<EnumCaseDecl>(decl)) {
      for (auto *EED : ECD->getElements()) {
        members.push_back(EED);
      }
    }
  }

  ctx.evaluator.cacheOutput(
      ParseMembersRequest{cast<IterableDeclContext>(decl)},
      FingerprintAndMembers{llvm::None, ctx.AllocateCopy(members)});
}

static SmallVector<InheritedEntry>
convertToInheritedEntries(BridgedArrayRef cInheritedTypes) {
  SmallVector<InheritedEntry> inheritedEntries;
  for (auto &repr : convertArrayRef<TypeRepr *>(cInheritedTypes)) {
    inheritedEntries.emplace_back(repr);
  }

  return inheritedEntries;
}

BridgedDeclContextAndDecl StructDecl_create(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStructKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, void *_Nullable opaqueGenericParamList,
    BridgedArrayRef cInheritedTypes, void *_Nullable opaqueGenericWhereClause,
    BridgedSourceRange cBraceRange) {
  ASTContext &context = convertASTContext(cContext);

  auto *decl = new (context) StructDecl(
      convertSourceLoc(cStructKeywordLoc), convertIdentifier(cName),
      convertSourceLoc(cNameLoc),
      context.AllocateCopy(convertToInheritedEntries(cInheritedTypes)),
      static_cast<GenericParamList *>(opaqueGenericParamList),
      convertDeclContext(cDeclContext));
  decl->setTrailingWhereClause(
      static_cast<TrailingWhereClause *>(opaqueGenericWhereClause));
  decl->setBraces(convertSourceRange(cBraceRange));

  return {bridgeDeclContext(decl), static_cast<Decl *>(decl)};
}

BridgedDeclContextAndDecl ClassDecl_create(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cClassKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, void *_Nullable opaqueGenericParamList,
    BridgedArrayRef cInheritedTypes, void *_Nullable opaqueGenericWhereClause,
    BridgedSourceRange cBraceRange, bool isActor) {
  ASTContext &context = convertASTContext(cContext);

  auto *decl = new (context) ClassDecl(
      convertSourceLoc(cClassKeywordLoc), convertIdentifier(cName),
      convertSourceLoc(cNameLoc),
      context.AllocateCopy(convertToInheritedEntries(cInheritedTypes)),
      static_cast<GenericParamList *>(opaqueGenericParamList),
      convertDeclContext(cDeclContext), isActor);
  decl->setTrailingWhereClause(
      static_cast<TrailingWhereClause *>(opaqueGenericWhereClause));
  decl->setBraces(convertSourceRange(cBraceRange));

  return {bridgeDeclContext(decl), static_cast<Decl *>(decl)};
}

BridgedDeclContextAndDecl ProtocolDecl_create(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cProtocolKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedArrayRef cPrimaryAssociatedTypeNames,
    BridgedArrayRef cInheritedTypes, void *_Nullable opaqueGenericWhereClause,
    BridgedSourceRange cBraceRange) {
  SmallVector<PrimaryAssociatedTypeName, 2> primaryAssociatedTypeNames;
  for (auto &pair : convertArrayRef<BridgedIdentifierAndSourceLoc>(
           cPrimaryAssociatedTypeNames)) {
    primaryAssociatedTypeNames.emplace_back(convertIdentifier(pair.name),
                                            convertSourceLoc(pair.nameLoc));
  }

  ASTContext &context = convertASTContext(cContext);
  auto *decl = new (context) ProtocolDecl(
      convertDeclContext(cDeclContext), convertSourceLoc(cProtocolKeywordLoc),
      convertSourceLoc(cNameLoc), convertIdentifier(cName),
      context.AllocateCopy(primaryAssociatedTypeNames),
      context.AllocateCopy(convertToInheritedEntries(cInheritedTypes)),
      static_cast<TrailingWhereClause *>(opaqueGenericWhereClause));
  decl->setBraces(convertSourceRange(cBraceRange));

  return {bridgeDeclContext(decl), static_cast<Decl *>(decl)};
}

void *OperatorDecl_create(BridgedASTContext cContext,
                          BridgedDeclContext cDeclContext,
                          BridgedOperatorFixity cFixity,
                          BridgedSourceLoc cOperatorKeywordLoc,
                          BridgedIdentifier cName, BridgedSourceLoc cNameLoc,
                          BridgedSourceLoc cColonLoc,
                          BridgedIdentifier cPrecedenceGroupName,
                          BridgedSourceLoc cPrecedenceGroupLoc) {
  assert(bool(cColonLoc.raw) == (bool)cPrecedenceGroupName.raw);
  assert(bool(cColonLoc.raw) == (bool)cPrecedenceGroupLoc.raw);

  ASTContext &context = convertASTContext(cContext);
  auto operatorKeywordLoc = convertSourceLoc(cOperatorKeywordLoc);
  auto name = convertIdentifier(cName);
  auto nameLoc = convertSourceLoc(cNameLoc);
  auto *declContext = convertDeclContext(cDeclContext);

  OperatorDecl *decl = nullptr;
  switch (cFixity) {
  case BridgedOperatorFixityInfix:
    decl = new (context) InfixOperatorDecl(
        declContext, operatorKeywordLoc, name, nameLoc,
        convertSourceLoc(cColonLoc), convertIdentifier(cPrecedenceGroupName),
        convertSourceLoc(cPrecedenceGroupLoc));
    break;
  case BridgedOperatorFixityPrefix:
    assert(!cColonLoc.raw);
    decl = new (context)
        PrefixOperatorDecl(declContext, operatorKeywordLoc, name, nameLoc);
    break;
  case BridgedOperatorFixityPostfix:
    assert(!cColonLoc.raw);
    decl = new (context)
        PostfixOperatorDecl(declContext, operatorKeywordLoc, name, nameLoc);
    break;
  }

  return static_cast<Decl *>(decl);
}

void *PrecedenceGroupDecl_create(
    BridgedDeclContext cDeclContext,
    BridgedSourceLoc cPrecedencegroupKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedSourceLoc cLeftBraceLoc,
    BridgedSourceLoc cAssociativityKeywordLoc,
    BridgedSourceLoc cAssociativityValueLoc,
    BridgedAssociativity cAssociativity, BridgedSourceLoc cAssignmentKeywordLoc,
    BridgedSourceLoc cAssignmentValueLoc, bool isAssignment,
    BridgedSourceLoc cHigherThanKeywordLoc, BridgedArrayRef cHigherThanNames,
    BridgedSourceLoc cLowerThanKeywordLoc, BridgedArrayRef cLowerThanNames,
    BridgedSourceLoc cRightBraceLoc) {

  SmallVector<PrecedenceGroupDecl::Relation, 2> higherThanNames;
  for (auto &pair :
       convertArrayRef<BridgedIdentifierAndSourceLoc>(cHigherThanNames)) {
    higherThanNames.push_back({convertSourceLoc(pair.nameLoc),
                               convertIdentifier(pair.name), nullptr});
  }

  SmallVector<PrecedenceGroupDecl::Relation, 2> lowerThanNames;
  for (auto &pair :
       convertArrayRef<BridgedIdentifierAndSourceLoc>(cLowerThanNames)) {
    lowerThanNames.push_back({convertSourceLoc(pair.nameLoc),
                              convertIdentifier(pair.name), nullptr});
  }

  auto *decl = PrecedenceGroupDecl::create(
      convertDeclContext(cDeclContext),
      convertSourceLoc(cPrecedencegroupKeywordLoc), convertSourceLoc(cNameLoc),
      convertIdentifier(cName), convertSourceLoc(cLeftBraceLoc),
      convertSourceLoc(cAssociativityKeywordLoc),
      convertSourceLoc(cAssociativityValueLoc),
      static_cast<Associativity>(cAssociativity),
      convertSourceLoc(cAssignmentKeywordLoc),
      convertSourceLoc(cAssignmentValueLoc), isAssignment,
      convertSourceLoc(cHigherThanKeywordLoc), higherThanNames,
      convertSourceLoc(cLowerThanKeywordLoc), lowerThanNames,
      convertSourceLoc(cRightBraceLoc));

  return static_cast<Decl *>(decl);
}

void *ImportDecl_create(BridgedASTContext cContext,
                        BridgedDeclContext cDeclContext,
                        BridgedSourceLoc cImportKeywordLoc,
                        BridgedImportKind cImportKind,
                        BridgedSourceLoc cImportKindLoc,
                        BridgedArrayRef cImportPathElements) {
  ImportPath::Builder builder;
  for (auto &element :
       convertArrayRef<BridgedIdentifierAndSourceLoc>(cImportPathElements)) {
    builder.push_back(convertIdentifier(element.name),
                      convertSourceLoc(element.nameLoc));
  }

  ASTContext &context = convertASTContext(cContext);
  auto *decl = ImportDecl::create(
      context, convertDeclContext(cDeclContext),
      convertSourceLoc(cImportKeywordLoc), static_cast<ImportKind>(cImportKind),
      convertSourceLoc(cImportKindLoc), std::move(builder).get());

  return static_cast<Decl *>(decl);
}

void *OptionalTypeRepr_create(BridgedASTContext cContext, void *base,
                              BridgedSourceLoc cQuestionLoc) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      OptionalTypeRepr((TypeRepr *)base, convertSourceLoc(cQuestionLoc));
}

void *ImplicitlyUnwrappedOptionalTypeRepr_create(
    BridgedASTContext cContext, void *base, BridgedSourceLoc cExclamationLoc) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) ImplicitlyUnwrappedOptionalTypeRepr(
      (TypeRepr *)base, convertSourceLoc(cExclamationLoc));
}

void *ArrayTypeRepr_create(BridgedASTContext cContext, void *base,
                           BridgedSourceLoc cLSquareLoc,
                           BridgedSourceLoc cRSquareLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc lSquareLoc = convertSourceLoc(cLSquareLoc);
  SourceLoc rSquareLoc = convertSourceLoc(cRSquareLoc);
  return new (context)
      ArrayTypeRepr((TypeRepr *)base, SourceRange{lSquareLoc, rSquareLoc});
}

void *DictionaryTypeRepr_create(BridgedASTContext cContext, void *keyType,
                                void *valueType, BridgedSourceLoc cLSquareLoc,
                                BridgedSourceLoc cColonloc,
                                BridgedSourceLoc cRSquareLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc lSquareLoc = convertSourceLoc(cLSquareLoc);
  SourceLoc colonLoc = convertSourceLoc(cColonloc);
  SourceLoc rSquareLoc = convertSourceLoc(cRSquareLoc);
  return new (context)
      DictionaryTypeRepr((TypeRepr *)keyType, (TypeRepr *)valueType, colonLoc,
                         SourceRange{lSquareLoc, rSquareLoc});
}

void *MetatypeTypeRepr_create(BridgedASTContext cContext, void *baseType,
                              BridgedSourceLoc cTypeLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc tyLoc = convertSourceLoc(cTypeLoc);
  return new (context) MetatypeTypeRepr((TypeRepr *)baseType, tyLoc);
}

void *ProtocolTypeRepr_create(BridgedASTContext cContext, void *baseType,
                              BridgedSourceLoc cProtoLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc protoLoc = convertSourceLoc(cProtoLoc);
  return new (context) ProtocolTypeRepr((TypeRepr *)baseType, protoLoc);
}

void *PackExpansionTypeRepr_create(BridgedASTContext cContext, void *base,
                                   BridgedSourceLoc cRepeatLoc) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      PackExpansionTypeRepr(convertSourceLoc(cRepeatLoc), (TypeRepr *)base);
}

BridgedTypeAttrKind TypeAttrKind_fromString(BridgedString cStr) {
  TypeAttrKind kind =
      TypeAttributes::getAttrKindFromString(convertString(cStr));
  switch (kind) {
#define TYPE_ATTR(X) case TAK_##X: return BridgedTypeAttrKind_##X;
#include "swift/AST/Attr.def"
    case TAK_Count: return BridgedTypeAttrKind_Count;
  }
}

BridgedTypeAttributes TypeAttributes_create() { return {new TypeAttributes()}; }

void TypeAttributes_addSimpleAttr(BridgedTypeAttributes cAttributes,
                                  BridgedTypeAttrKind cKind,
                                  BridgedSourceLoc cAtLoc,
                                  BridgedSourceLoc cAttrLoc) {
  TypeAttributes *typeAttributes = convertTypeAttributes(cAttributes);
  typeAttributes->setAttr(convertTypeAttrKind(cKind),
                          convertSourceLoc(cAttrLoc));
  if (typeAttributes->AtLoc.isInvalid())
    typeAttributes->AtLoc = convertSourceLoc(cAtLoc);
}

void *AttributedTypeRepr_create(BridgedASTContext cContext, void *base,
                                BridgedTypeAttributes cAttributes) {
  TypeAttributes *typeAttributes = convertTypeAttributes(cAttributes);
  if (typeAttributes->empty())
    return base;

  ASTContext &context = convertASTContext(cContext);
  auto attributedType =
      new (context) AttributedTypeRepr(*typeAttributes, (TypeRepr *)base);
  delete typeAttributes;
  return attributedType;
}

void *
AttributedTypeSpecifierRepr_create(BridgedASTContext cContext, void *base,
                                   BridgedAttributedTypeSpecifier specifier,
                                   BridgedSourceLoc cSpecifierLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc loc = convertSourceLoc(cSpecifierLoc);
  TypeRepr *baseType = (TypeRepr *)base;
  switch (specifier) {
  case BridgedAttributedTypeSpecifierInOut:
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::InOut, loc);
  case BridgedAttributedTypeSpecifierBorrowing:
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::Borrowing, loc);
  case BridgedAttributedTypeSpecifierConsuming:
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::Consuming, loc);
  case BridgedAttributedTypeSpecifierLegacyShared:
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::LegacyShared, loc);
  case BridgedAttributedTypeSpecifierLegacyOwned:
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::LegacyOwned, loc);
  case BridgedAttributedTypeSpecifierConst:
    return new (context) CompileTimeConstTypeRepr(baseType, loc);
  case BridgedAttributedTypeSpecifierIsolated:
    return new (context) IsolatedTypeRepr(baseType, loc);
  }
}

void *VarargTypeRepr_create(BridgedASTContext cContext, void *base,
                            BridgedSourceLoc cEllipsisLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc ellipsisLoc = convertSourceLoc(cEllipsisLoc);
  TypeRepr *baseType = (TypeRepr *)base;
  return new (context) VarargTypeRepr(baseType, ellipsisLoc);
}

void *TupleTypeRepr_create(BridgedASTContext cContext, BridgedArrayRef elements,
                           BridgedSourceLoc cLParenLoc,
                           BridgedSourceLoc cRParenLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc lParen = convertSourceLoc(cLParenLoc);
  SourceLoc rParen = convertSourceLoc(cRParenLoc);

  SmallVector<TupleTypeReprElement, 8> tupleElements;
  for (auto element : convertArrayRef<BridgedTupleTypeElement>(elements)) {
    TupleTypeReprElement elementRepr;
    elementRepr.Name = convertIdentifier(element.Name);
    elementRepr.NameLoc = convertSourceLoc(element.NameLoc);
    elementRepr.SecondName = convertIdentifier(element.SecondName);
    elementRepr.SecondNameLoc = convertSourceLoc(element.SecondNameLoc);
    elementRepr.UnderscoreLoc = convertSourceLoc(element.UnderscoreLoc);
    elementRepr.ColonLoc = convertSourceLoc(element.ColonLoc);
    elementRepr.Type = (TypeRepr *)element.Type;
    elementRepr.TrailingCommaLoc = convertSourceLoc(element.TrailingCommaLoc);
    tupleElements.emplace_back(elementRepr);
  }

  return TupleTypeRepr::create(context, tupleElements,
                               SourceRange{lParen, rParen});
}

void *MemberTypeRepr_create(BridgedASTContext cContext, void *baseComponent,
                            BridgedArrayRef bridgedMemberComponents) {
  ASTContext &context = convertASTContext(cContext);
  auto memberComponents =
      convertArrayRef<IdentTypeRepr *>(bridgedMemberComponents);

  return MemberTypeRepr::create(context, (TypeRepr *)baseComponent,
                                memberComponents);
}

void *EmptyCompositionTypeRepr_create(BridgedASTContext cContext,
                                      BridgedSourceLoc cAnyLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc anyLoc = convertSourceLoc(cAnyLoc);
  return CompositionTypeRepr::createEmptyComposition(context, anyLoc);
}

void *CompositionTypeRepr_create(BridgedASTContext cContext,
                                 BridgedArrayRef cTypes,
                                 BridgedSourceLoc cFirstTypeLoc,
                                 BridgedSourceLoc cFirstAmpLoc) {
  ASTContext &context = convertASTContext(cContext);
  SourceLoc firstTypeLoc = convertSourceLoc(cFirstTypeLoc);
  SourceLoc firstAmpLoc = convertSourceLoc(cFirstAmpLoc);
  auto types = convertArrayRef<TypeRepr *>(cTypes);
  return CompositionTypeRepr::create(
      context, types, firstTypeLoc,
      SourceRange{firstAmpLoc, types.back()->getEndLoc()});
}

void *FunctionTypeRepr_create(BridgedASTContext cContext, void *argsTy,
                              BridgedSourceLoc cAsyncLoc,
                              BridgedSourceLoc cThrowsLoc,
                              void * _Nullable thrownType,
                              BridgedSourceLoc cArrowLoc, void *returnType) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) FunctionTypeRepr(
      nullptr, (TupleTypeRepr *)argsTy, convertSourceLoc(cAsyncLoc),
      convertSourceLoc(cThrowsLoc), (TypeRepr *)thrownType, convertSourceLoc(cArrowLoc),
      (TypeRepr *)returnType);
}

void *NamedOpaqueReturnTypeRepr_create(BridgedASTContext cContext,
                                       void *baseTy) {
  ASTContext &context = convertASTContext(cContext);
  return new (context) NamedOpaqueReturnTypeRepr((TypeRepr *)baseTy, nullptr);
}

void *OpaqueReturnTypeRepr_create(BridgedASTContext cContext,
                                  BridgedSourceLoc cOpaqueLoc, void *baseTy) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      OpaqueReturnTypeRepr(convertSourceLoc(cOpaqueLoc), (TypeRepr *)baseTy);
}
void *ExistentialTypeRepr_create(BridgedASTContext cContext,
                                 BridgedSourceLoc cAnyLoc, void *baseTy) {
  ASTContext &context = convertASTContext(cContext);
  return new (context)
      ExistentialTypeRepr(convertSourceLoc(cAnyLoc), (TypeRepr *)baseTy);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void TopLevelCodeDecl_dump(void *decl) {
  static_cast<TopLevelCodeDecl *>(decl)->dump(llvm::errs());
}
void Expr_dump(void *expr) { static_cast<Expr *>(expr)->dump(llvm::errs()); }
void Decl_dump(void *decl) { static_cast<Decl *>(decl)->dump(llvm::errs()); }
void Stmt_dump(void *statement) {
  static_cast<Stmt *>(statement)->dump(llvm::errs());
}
void Type_dump(void *type) { static_cast<TypeRepr *>(type)->dump(); }

#pragma clang diagnostic pop

//===----------------------------------------------------------------------===//
// Plugins
//===----------------------------------------------------------------------===//

PluginCapabilityPtr Plugin_getCapability(PluginHandle handle) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  return plugin->getCapability();
}

void Plugin_setCapability(PluginHandle handle, PluginCapabilityPtr data) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  plugin->setCapability(data);
}

const char *Plugin_getExecutableFilePath(PluginHandle handle) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  return plugin->getExecutablePath().data();
}

void Plugin_lock(PluginHandle handle) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  plugin->lock();
}

void Plugin_unlock(PluginHandle handle) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  plugin->unlock();
}

bool Plugin_spawnIfNeeded(PluginHandle handle) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  auto error = plugin->spawnIfNeeded();
  bool hadError(error);
  llvm::consumeError(std::move(error));
  return hadError;
}

bool Plugin_sendMessage(PluginHandle handle, const BridgedData data) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  StringRef message(data.baseAddress, data.size);
  auto error = plugin->sendMessage(message);
  if (error) {
    // FIXME: Pass the error message back to the caller.
    llvm::consumeError(std::move(error));
//    llvm::handleAllErrors(std::move(error), [](const llvm::ErrorInfoBase &err) {
//      llvm::errs() << err.message() << "\n";
//    });
    return true;
  }
  return false;
}

bool Plugin_waitForNextMessage(PluginHandle handle, BridgedData *out) {
  auto *plugin = static_cast<LoadedExecutablePlugin *>(handle);
  auto result = plugin->waitForNextMessage();
  if (!result) {
    // FIXME: Pass the error message back to the caller.
    llvm::consumeError(result.takeError());
//    llvm::handleAllErrors(result.takeError(), [](const llvm::ErrorInfoBase &err) {
//      llvm::errs() << err.message() << "\n";
//    });
    return true;
  }
  auto &message = result.get();
  auto size = message.size();
  auto outPtr = malloc(size);
  memcpy(outPtr, message.data(), size);
  *out = BridgedData{(const char *)outPtr, size};
  return false;
}

//===----------------------------------------------------------------------===//
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

#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/NameLookup.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/IDE/Utils.h"
#include "swift/Markup/XMLUtils.h"
#include "swift/Subsystems.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Basic/CharInfo.h"

#include "llvm/Support/MemoryBuffer.h"

#include <numeric>

using namespace swift;
using namespace swift::ide;

llvm::Optional<std::pair<unsigned, unsigned>>
swift::ide::parseLineCol(StringRef LineCol) {
  unsigned Line, Col;
  size_t ColonIdx = LineCol.find(':');
  if (ColonIdx == StringRef::npos) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    return llvm::None;
  }
  if (LineCol.substr(0, ColonIdx).getAsInteger(10, Line)) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    return llvm::None;
  }
  if (LineCol.substr(ColonIdx+1).getAsInteger(10, Col)) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    return llvm::None;
  }

  if (Line == 0 || Col == 0) {
    llvm::errs() << "wrong pos format, line/col should start from 1\n";
    return llvm::None;
  }

  return std::make_pair(Line, Col);
}

void XMLEscapingPrinter::printText(StringRef Text) {
  swift::markup::appendWithXMLEscaping(OS, Text);
}

void XMLEscapingPrinter::printXML(StringRef Text) {
  OS << Text;
}

SourceManager &NameMatcher::getSourceMgr() const {
  return SrcFile.getASTContext().SourceMgr;
}

ResolvedLoc NameMatcher::resolve(UnresolvedLoc Loc) {
  return resolve({Loc}, {}).front();
}

std::vector<ResolvedLoc> NameMatcher::resolve(ArrayRef<UnresolvedLoc> Locs, ArrayRef<Token> Tokens) {

  // Note the original indices and sort them in reverse source order
  std::vector<size_t> MapToOriginalIndex(Locs.size());
  std::iota(MapToOriginalIndex.begin(), MapToOriginalIndex.end(), 0);
  std::sort(MapToOriginalIndex.begin(), MapToOriginalIndex.end(),
            [this, Locs](size_t first, size_t second) {
              return first != second && !getSourceMgr()
                .isBeforeInBuffer(Locs[first].Loc, Locs[second].Loc);
            });

  // Add the locs themselves
  LocsToResolve.clear();
  llvm::transform(MapToOriginalIndex, std::back_inserter(LocsToResolve),
                  [&](size_t index) { return Locs[index]; });

  InactiveConfigRegionNestings = 0;
  SelectorNestings = 0;
  TokensToCheck = Tokens;
  ResolvedLocs.clear();
  SrcFile.walk(*this);
  checkComments();

  // handle any unresolved locs past the end of the last AST node or comment
  std::vector<ResolvedLoc> Remaining(Locs.size() - ResolvedLocs.size(),
                                     {ASTWalker::ParentTy(),
                                      CharSourceRange(),
                                      {},
                                      llvm::None,
                                      LabelRangeType::None,
                                      /*isActice*/ true,
                                      /*isInSelector*/ false});
  ResolvedLocs.insert(ResolvedLocs.end(), Remaining.begin(), Remaining.end());

  // return in the original order
  std::vector<ResolvedLoc> Ordered(ResolvedLocs.size());
  for(size_t Index = 0; Index < ResolvedLocs.size(); ++Index) {
    size_t Flipped = ResolvedLocs.size() - 1 - Index;
    Ordered[MapToOriginalIndex[Flipped]] = ResolvedLocs[Index];
  }
  return Ordered;
}

static std::vector<CharSourceRange> getLabelRanges(const ParameterList* List, 
                                                   const SourceManager &SM) {
  std::vector<CharSourceRange> LabelRanges;
  for (ParamDecl *Param: *List) {
    if (Param->isImplicit())
      continue;

    SourceLoc NameLoc = Param->getArgumentNameLoc();
    SourceLoc ParamLoc = Param->getNameLoc();
    size_t NameLength;
    if (NameLoc.isValid()) {
      LabelRanges.push_back(Lexer::getCharSourceRangeFromSourceRange(
                                SM, SourceRange(NameLoc, ParamLoc)));
    } else {
      NameLoc = ParamLoc;
      NameLength = Param->getNameStr().size();
      if (SM.extractText({NameLoc, 1}) == "`")
        NameLength += 2;
      LabelRanges.push_back(CharSourceRange(NameLoc, NameLength));
    }
  }
  return LabelRanges;
}

static std::vector<CharSourceRange> getEnumParamListInfo(SourceManager &SM, 
                                                         ParameterList *PL) {
  std::vector<CharSourceRange> LabelRanges;
  for (ParamDecl *Param: *PL) {
    if (Param->isImplicit())
      continue;

    SourceLoc LabelStart;
    if (auto *repr = Param->getTypeRepr())
      LabelStart = repr->getLoc();
    SourceLoc LabelEnd(LabelStart);
    
    if (Param->getNameLoc().isValid()) {
      LabelStart = Param->getNameLoc();
    }
    LabelRanges.push_back(CharSourceRange(SM, LabelStart, LabelEnd));
  }
  return LabelRanges;
}

bool NameMatcher::handleCustomAttrs(Decl *D) {
  // CustomAttrs of non-param VarDecls are handled when this method is called
  // on their containing PatternBindingDecls (see below).
  if (isa<VarDecl>(D) && !isa<ParamDecl>(D))
    return true;

  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    if (auto *SingleVar = PBD->getSingleVar()) {
      D = SingleVar;
    } else {
        return true;
    }
  }

  for (auto *customAttr : D->getOriginalAttrs().getAttributes<CustomAttr, true>()) {
    if (shouldSkip(customAttr->getRangeWithAt()))
      continue;
    auto *Args = customAttr->getArgs();
    if (auto *Repr = customAttr->getTypeRepr()) {
      // Note the associated call arguments of the semantic initializer call
      // in case we're resolving an explicit initializer call within the
      // CustomAttr's type, e.g. on `Wrapper` in `@Wrapper(wrappedValue: 10)`.
      SWIFT_DEFER { CustomAttrArgList = llvm::None; };
      if (Args && !Args->isImplicit())
        CustomAttrArgList = Located<ArgumentList *>(Args, Repr->getLoc());
      if (!Repr->walk(*this))
        return false;
    }
    if (Args && !customAttr->isImplicit()) {
      if (!Args->walk(*this))
        return false;
    }
  }
  return !isDone();
}

ASTWalker::PreWalkAction NameMatcher::walkToDeclPre(Decl *D) {
  // Handle occurrences in any preceding doc comments
  RawComment R = D->getRawComment();
  if (!R.isEmpty()) {
    for(SingleRawComment C: R.Comments) {
      while(!shouldSkip(C.Range))
        tryResolve(ASTWalker::ParentTy(), nextLoc());
    }
  }
  if (isDone())
    return Action::Stop();

  // FIXME: Even implicit Decls should have proper ranges if they include any
  // non-implicit children (fix implicit Decls created for lazy vars).
  if (D->isImplicit())
    return Action::Continue();

  if (shouldSkip(D->getSourceRangeIncludingAttrs()))
    return Action::SkipChildren();

  if (!handleCustomAttrs(D))
    return Action::Stop();

  if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
    for (auto Clause : ICD->getClauses()) {
      if (!Clause.isActive)
        ++InactiveConfigRegionNestings;
      
      for (auto Member : Clause.Elements) {
        Member.walk(*this);
      }
      
      if (!Clause.isActive) {
        assert(InactiveConfigRegionNestings > 0);
        --InactiveConfigRegionNestings;
      }
    }
    return Action::SkipChildren();
  } else if (AbstractFunctionDecl *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
    std::vector<CharSourceRange> LabelRanges;
    if (AFD->getNameLoc() == nextLoc()) {
      auto ParamList = AFD->getParameters();
      LabelRanges = getLabelRanges(ParamList, getSourceMgr());
    }
    tryResolve(ASTWalker::ParentTy(D), D->getLoc(), LabelRangeType::Param,
               LabelRanges, llvm::None);
  } else if (SubscriptDecl *SD = dyn_cast<SubscriptDecl>(D)) {
    tryResolve(ASTWalker::ParentTy(D), D->getLoc(),
               LabelRangeType::NoncollapsibleParam,
               getLabelRanges(SD->getIndices(), getSourceMgr()), llvm::None);
  } else if (EnumElementDecl *EED = dyn_cast<EnumElementDecl>(D)) {
    if (auto *ParamList = EED->getParameterList()) {
      auto LabelRanges = getEnumParamListInfo(getSourceMgr(), ParamList);
      tryResolve(ASTWalker::ParentTy(D), D->getLoc(), LabelRangeType::CallArg,
                 LabelRanges, llvm::None);
    } else {
      tryResolve(ASTWalker::ParentTy(D), D->getLoc());
    }
  } else if (ImportDecl *ID = dyn_cast<ImportDecl>(D)) {
    for(const ImportPath::Element &Element: ID->getImportPath()) {
      tryResolve(ASTWalker::ParentTy(D), Element.Loc);
      if (isDone())
        break;
    }
  } else if (isa<ValueDecl>(D) || isa<ExtensionDecl>(D) ||
             isa<PrecedenceGroupDecl>(D)) {
    tryResolve(ASTWalker::ParentTy(D), D->getLoc());
  }
  return Action::StopIf(isDone());
}

ASTWalker::PreWalkResult<Stmt *> NameMatcher::walkToStmtPre(Stmt *S) {
  if (isDone())
    return Action::Stop();

  // FIXME: Even implicit Stmts should have proper ranges that include any
  // non-implicit Stmts (fix Stmts created for lazy vars).
  auto ShouldSkip = !S->isImplicit() && shouldSkip(S->getSourceRange());
  return Action::SkipChildrenIf(ShouldSkip, S);
}

ArgumentList *NameMatcher::getApplicableArgsFor(Expr *E) {
  if (ParentCalls.empty())
    return nullptr;
  auto &Last = ParentCalls.back();
  return Last.ApplicableTo == E ? Last.Call->getArgs() : nullptr;
}

static Expr *extractNameExpr(Expr *Fn) {
  Fn = Fn->getSemanticsProvidingExpr();
  switch (Fn->getKind()) {
    case ExprKind::DeclRef:
    case ExprKind::UnresolvedDeclRef:
    case ExprKind::UnresolvedMember:
    case ExprKind::UnresolvedDot:
      return Fn;
    default:
      break;
  }
  if (auto *SAE = dyn_cast<SelfApplyExpr>(Fn))
    return extractNameExpr(SAE->getFn());
  if (auto *ACE = dyn_cast<AutoClosureExpr>(Fn))
    if (auto *Unwrapped = ACE->getUnwrappedCurryThunkExpr())
      return extractNameExpr(Unwrapped);
  return nullptr;
}

ASTWalker::PreWalkResult<ArgumentList *>
NameMatcher::walkToArgumentListPre(ArgumentList *ArgList) {
  if (!ArgList->isImplicit()) {
    auto Labels = getCallArgLabelRanges(getSourceMgr(), ArgList,
                                        LabelRangeEndAt::BeforeElemStart);
    tryResolve(Parent, ArgList->getStartLoc(), LabelRangeType::CallArg,
             Labels.first, Labels.second);
  }
  if (isDone())
    return Action::Stop();

  // Handle arg label locations (the index reports property occurrences on them
  // for memberwise inits).
  for (auto Arg : *ArgList) {
    auto Name = Arg.getLabel();
    auto *E = Arg.getExpr();
    if (!Name.empty()) {
      tryResolve(Parent, Arg.getLabelLoc());
      if (isDone())
        return Action::Stop();
    }
    if (!E->walk(*this))
      return Action::Stop();
  }
  // TODO: We should consider changing Action::SkipChildren to still call
  // walkToArgumentListPost, which would eliminate the need for this.
  auto postWalkResult = walkToArgumentListPost(ArgList);
  switch (postWalkResult.Action.Action) {
  case PostWalkAction::Stop:
    return Action::Stop();
  case PostWalkAction::Continue:
    // We already visited the children.
    return Action::SkipChildren(*postWalkResult.Value);
  }
  llvm_unreachable("Unhandled case in switch!");
}

ASTWalker::PreWalkResult<Expr *> NameMatcher::walkToExprPre(Expr *E) {
  if (isDone())
    return Action::Stop();
  if (shouldSkip(E))
    return Action::SkipChildren(E);

  if (isa<ObjCSelectorExpr>(E)) {
      ++SelectorNestings;
  }

  // only match name locations of expressions apparent in the original source
  if (!E->isImplicit()) {

    if (auto *CE = dyn_cast<CallExpr>(E)) {
      // Keep a stack of parent CallExprs along with the expression their
      // arguments belong to.
      if (!CE->isImplicit()) {
        if (auto *ApplicableExpr = extractNameExpr(CE->getFn()))
          ParentCalls.push_back({ApplicableExpr, CE});
      }
    }

    // Try to resolve against the below kinds *before* their children are
    // visited to ensure visitation happens in source order.
    switch (E->getKind()) {
      case ExprKind::UnresolvedMember: {
        auto UME = cast<UnresolvedMemberExpr>(E);
        tryResolve(ASTWalker::ParentTy(E), UME->getNameLoc(),
                   getApplicableArgsFor(E));
      } break;
      case ExprKind::DeclRef: {
        auto DRE = cast<DeclRefExpr>(E);
        tryResolve(ASTWalker::ParentTy(E), DRE->getNameLoc(),
                   getApplicableArgsFor(E));
        break;
      }
      case ExprKind::UnresolvedDeclRef: {
        auto UDRE = cast<UnresolvedDeclRefExpr>(E);
        tryResolve(ASTWalker::ParentTy(E), UDRE->getNameLoc(),
                   getApplicableArgsFor(E));
        break;
      }
      case ExprKind::StringLiteral:
        // Handle multple locations in a single string literal
        do {
          tryResolve(ASTWalker::ParentTy(E), nextLoc());
        } while (!shouldSkip(E));
        break;
      case ExprKind::Binary: {
        BinaryExpr *BinE = cast<BinaryExpr>(E);
        // Visit in source order.
        if (!BinE->getLHS()->walk(*this))
          return Action::Stop();
        if (!BinE->getFn()->walk(*this))
          return Action::Stop();
        if (!BinE->getRHS()->walk(*this))
          return Action::Stop();

        // TODO: We should consider changing Action::SkipChildren to still call
        // walkToArgumentListPost, which would eliminate the need for this.
        auto postWalkResult = walkToExprPost(E);
        switch (postWalkResult.Action.Action) {
        case PostWalkAction::Stop:
          return Action::Stop();
        case PostWalkAction::Continue:
          // We already visited the children.
          return Action::SkipChildren(*postWalkResult.Value);
        }
        llvm_unreachable("Unhandled case in switch!");
      }
      case ExprKind::KeyPath: {
        KeyPathExpr *KP = cast<KeyPathExpr>(E);

        // Swift keypath components are visited already, so there's no need to
        // handle them specially.
        if (!KP->isObjC())
          break;

        for (auto Component: KP->getComponents()) {
          switch (Component.getKind()) {
          case KeyPathExpr::Component::Kind::UnresolvedProperty:
          case KeyPathExpr::Component::Kind::Property:
            tryResolve(ASTWalker::ParentTy(E), Component.getLoc());
            break;
          case KeyPathExpr::Component::Kind::DictionaryKey:
          case KeyPathExpr::Component::Kind::Invalid:
          case KeyPathExpr::Component::Kind::CodeCompletion:
            break;
          case KeyPathExpr::Component::Kind::OptionalForce:
          case KeyPathExpr::Component::Kind::OptionalChain:
          case KeyPathExpr::Component::Kind::OptionalWrap:
          case KeyPathExpr::Component::Kind::UnresolvedSubscript:
          case KeyPathExpr::Component::Kind::Subscript:
          case KeyPathExpr::Component::Kind::Identity:
          case KeyPathExpr::Component::Kind::TupleElement:
            llvm_unreachable("Unexpected component in ObjC KeyPath expression");
            break;
          }
        }
        break;
      }
      default: // ignored
        break;
    }
  }
  return Action::StopIf(isDone(), E);
}

ASTWalker::PostWalkResult<Expr *> NameMatcher::walkToExprPost(Expr *E) {
  if (isDone())
    return Action::Stop();

  if (!E->isImplicit()) {
    // Try to resolve against the below kinds *after* their children have been
    // visited to ensure visitation happens in source order.
    switch (E->getKind()) {
      case ExprKind::MemberRef:
        tryResolve(ASTWalker::ParentTy(E), E->getLoc());
        break;
      case ExprKind::UnresolvedDot: {
        auto UDE = cast<UnresolvedDotExpr>(E);
        tryResolve(ASTWalker::ParentTy(E), UDE->getNameLoc(),
                   getApplicableArgsFor(E));
        break;
      }
      default:
        break;
    }

    if (auto *CE = dyn_cast<CallExpr>(E)) {
      if (!ParentCalls.empty() && ParentCalls.back().Call == CE)
        ParentCalls.pop_back();
    }
  }

  if (isa<ObjCSelectorExpr>(E)) {
    assert(SelectorNestings > 0);
    --SelectorNestings;
  }

  return Action::StopIf(isDone(), E);
}

ASTWalker::PreWalkAction NameMatcher::walkToTypeReprPre(TypeRepr *T) {
  if (isDone())
    return Action::Stop();
  if (shouldSkip(T->getSourceRange()))
    return Action::SkipChildren();

  if (isa<IdentTypeRepr>(T)) {
    // If we're walking a CustomAttr's type we may have an associated call
    // argument to resolve with from its semantic initializer.
    if (CustomAttrArgList.has_value() && CustomAttrArgList->Loc == T->getLoc()) {
      auto Labels =
          getCallArgLabelRanges(getSourceMgr(), CustomAttrArgList->Item,
                                LabelRangeEndAt::BeforeElemStart);
      tryResolve(ASTWalker::ParentTy(T), T->getLoc(), LabelRangeType::CallArg,
                 Labels.first, Labels.second);
    } else {
      tryResolve(ASTWalker::ParentTy(T), T->getLoc());
    }
  }
  return Action::StopIf(isDone());
}

ASTWalker::PreWalkResult<Pattern *> NameMatcher::walkToPatternPre(Pattern *P) {
  if (isDone())
    return Action::Stop();
  if (shouldSkip(P->getSourceRange()))
    return Action::SkipChildren(P);

  tryResolve(ASTWalker::ParentTy(P), P->getStartLoc());
  return Action::StopIf(isDone(), P);
}

bool NameMatcher::checkComments() {
  if (isDone())
    return false;
  TokensToCheck = TokensToCheck.drop_while([this](const Token &tok) -> bool {
    return getSourceMgr().isBeforeInBuffer(tok.getRange().getEnd(), nextLoc());
  });
  if (TokensToCheck.empty())
    return false;

  const Token &next = TokensToCheck.front();
  if (next.is(swift::tok::comment) && next.getRange().contains(nextLoc()) &&
      !next.getText().startswith("///"))
    return tryResolve(ASTWalker::ParentTy(), nextLoc());
  return false;
}

void NameMatcher::skipLocsBefore(SourceLoc Start) {
  while (!isDone() && getSourceMgr().isBeforeInBuffer(nextLoc(), Start)) {
    if (!checkComments()) {
      LocsToResolve.pop_back();
      ResolvedLocs.push_back({ASTWalker::ParentTy(),
                              CharSourceRange(),
                              {},
                              llvm::None,
                              LabelRangeType::None,
                              isActive(),
                              isInSelector()});
    }
  }
}

bool NameMatcher::shouldSkip(Expr *E) {
  if (isa<StringLiteralExpr>(E) && Parent.getAsExpr()) {
    // Attempting to get the CharSourceRange from the SourceRange of a
    // StringLiteralExpr that is a segment of an interpolated string gives
    // incorrect ranges. Use the CharSourceRange of the corresponding token
    // instead.

    auto ExprStart = E->getStartLoc();
    auto RemaingTokens = TokensToCheck.drop_while([&](const Token &tok) -> bool {
      return getSourceMgr().isBeforeInBuffer(tok.getRange().getStart(), ExprStart);
    });

    if (!RemaingTokens.empty() && RemaingTokens.front().getLoc() == ExprStart)
      return shouldSkip(RemaingTokens.front().getRange());
  }
  return shouldSkip(E->getSourceRange());
}

bool NameMatcher::shouldSkip(SourceRange Range) {
  return shouldSkip(Lexer::getCharSourceRangeFromSourceRange(getSourceMgr(),
                                                             Range));
}

bool NameMatcher::shouldSkip(CharSourceRange Range) {
  if (isDone())
    return true;
  if (Range.isInvalid())
    return false;

  skipLocsBefore(Range.getStart());
  return isDone() || !Range.contains(nextLoc());
}

SourceLoc NameMatcher::nextLoc() const {
  assert(!LocsToResolve.empty());
  return LocsToResolve.back().Loc;
}

std::vector<CharSourceRange> getSelectorLabelRanges(SourceManager &SM,
                                                    DeclNameLoc NameLoc) {
  SourceLoc Loc;
  std::vector<CharSourceRange> Ranges;
  size_t index = 0;
  while((Loc = NameLoc.getArgumentLabelLoc(index++)).isValid()) {
    CharSourceRange Range = Lexer::getCharSourceRangeFromSourceRange(SM,
                                                                     SourceRange(Loc));
    Ranges.push_back(Range);
  }

  return Ranges;
}

bool NameMatcher::tryResolve(ASTWalker::ParentTy Node, DeclNameLoc NameLoc,
                             ArgumentList *Args) {
  if (NameLoc.isInvalid())
    return false;

  if (NameLoc.isCompound()) {
    auto Labels = getSelectorLabelRanges(getSourceMgr(), NameLoc);
    bool Resolved = tryResolve(Node, NameLoc.getBaseNameLoc(),
                               LabelRangeType::Selector, Labels, llvm::None);
    if (!isDone()) {
      for (auto Label: Labels) {
        if (tryResolve(Node, Label.getStart())) {
          Resolved = true;
          if (isDone())
            break;
        }
      }
    }
    return Resolved;
  }

  if (LocsToResolve.back().ResolveArgLocs) {
    if (Args) {
      auto Labels = getCallArgLabelRanges(getSourceMgr(), Args,
                                          LabelRangeEndAt::BeforeElemStart);
      return tryResolve(Node, NameLoc.getBaseNameLoc(), LabelRangeType::CallArg,
                        Labels.first, Labels.second);
    }
  }

  return tryResolve(Node, NameLoc.getBaseNameLoc());
}

bool NameMatcher::tryResolve(ASTWalker::ParentTy Node, SourceLoc NameLoc) {
  assert(!isDone());
  return tryResolve(Node, NameLoc, LabelRangeType::None, llvm::None,
                    llvm::None);
}

bool NameMatcher::tryResolve(ASTWalker::ParentTy Node, SourceLoc NameLoc,
                             LabelRangeType RangeType,
                             ArrayRef<CharSourceRange> LabelRanges,
                             llvm::Optional<unsigned> FirstTrailingLabel) {
  skipLocsBefore(NameLoc);
  if (isDone())
    return false;

  CharSourceRange Range = Lexer::getCharSourceRangeFromSourceRange(getSourceMgr(),
                                                                   NameLoc);
  UnresolvedLoc &Next = LocsToResolve.back();
  bool WasResolved = false;
  if (Range.isValid()) {
    if (NameLoc == Next.Loc) {
      LocsToResolve.pop_back();
      ResolvedLocs.push_back({Node, Range, LabelRanges, FirstTrailingLabel,
        RangeType, isActive(), isInSelector()});
      if (isDone())
        return true;
      WasResolved = true;
    }

    if (Range.getByteLength() > 1 &&
        (Range.str().front() == '_' || Range.str().front() == '$')) {
      // Also try after any leading _ or $ for name references of wrapped
      // properties, e.g. 'foo' in '_foo' and '$foo' occurrences.
      auto NewRange = CharSourceRange(Range.getStart().getAdvancedLoc(1),
                                      Range.getByteLength() - 1);
      if (NewRange.getStart() == Next.Loc) {
        LocsToResolve.pop_back();
        ResolvedLocs.push_back({Node,
                                NewRange,
                                {},
                                llvm::None,
                                LabelRangeType::None,
                                isActive(),
                                isInSelector()});
        WasResolved = true;
      }
    }
  }
  return WasResolved;
}

void ResolvedRangeInfo::print(llvm::raw_ostream &OS) const {
  OS << "<Kind>";
  switch (Kind) {
  case RangeKind::SingleExpression: OS << "SingleExpression"; break;
  case RangeKind::SingleDecl: OS << "SingleDecl"; break;
  case RangeKind::MultiTypeMemberDecl: OS << "MultiTypeMemberDecl"; break;
  case RangeKind::MultiStatement: OS << "MultiStatement"; break;
  case RangeKind::PartOfExpression: OS << "PartOfExpression"; break;
  case RangeKind::SingleStatement: OS << "SingleStatement"; break;
  case RangeKind::Invalid: OS << "Invalid"; break;
  }
  OS << "</Kind>\n";

  OS << "<Content>" << ContentRange.str() << "</Content>\n";

  if (auto Ty = getType()) {
    OS << "<Type>";
    Ty->print(OS);
    OS << "</Type>";
    switch(exit()) {
    case ExitState::Positive: OS << "<Exit>true</Exit>"; break;
    case ExitState::Unsure: OS << "<Exit>unsure</Exit>"; break;
    case ExitState::Negative: OS << "<Exit>false</Exit>"; break;
    }
    OS << "\n";
  }

  if (RangeContext) {
    OS << "<Context>";
    printContext(OS, RangeContext);
    OS << "</Context>\n";
  }

  if (CommonExprParent) {
    OS << "<Parent>";
    OS << Expr::getKindName(CommonExprParent->getKind());
    OS << "</Parent>\n";
  }

  if (!HasSingleEntry) {
    OS << "<Entry>Multi</Entry>\n";
  }

  if (UnhandledEffects.contains(EffectKind::Throws)) {
    OS << "<Error>Throwing</Error>\n";
  }
  if (UnhandledEffects.contains(EffectKind::Async)) {
    OS << "<Effect>Async</Effect>\n";
  }

  if (Orphan != OrphanKind::None) {
    OS << "<Orphan>";
    switch (Orphan) {
    case OrphanKind::Continue:
      OS << "Continue";
      break;
    case OrphanKind::Break:
      OS << "Break";
      break;
    case OrphanKind::None:
      llvm_unreachable("cannot enter here.");
    }
    OS << "</Orphan>";
  }

  for (auto &VD : DeclaredDecls) {
    OS << "<Declared>" << VD.VD->getBaseName() << "</Declared>";
    OS << "<OutscopeReference>";
    if (VD.ReferredAfterRange)
      OS << "true";
    else
      OS << "false";
    OS << "</OutscopeReference>\n";
  }
  for (auto &RD : ReferencedDecls) {
    OS << "<Referenced>" << RD.VD->getBaseName() << "</Referenced>";
    OS << "<Type>";
    RD.Ty->print(OS);
    OS << "</Type>\n";
  }

  OS << "<ASTNodes>" << ContainedNodes.size() << "</ASTNodes>\n";
  OS << "<end>\n";
}

CharSourceRange ResolvedRangeInfo::
calculateContentRange(ArrayRef<Token> Tokens) {
  if (Tokens.empty())
    return CharSourceRange();
  auto StartTok = Tokens.front();
  auto EndTok = Tokens.back();
  auto StartLoc = StartTok.hasComment() ?
    StartTok.getCommentStart() : StartTok.getLoc();
  auto EndLoc = EndTok.getRange().getEnd();
  auto Length = static_cast<const char *>(EndLoc.getOpaquePointerValue()) -
                static_cast<const char *>(StartLoc.getOpaquePointerValue());
  return CharSourceRange(StartLoc, Length);
}

bool DeclaredDecl::operator==(const DeclaredDecl& Other) {
  return VD == Other.VD;
}

ReturnInfo::
ReturnInfo(ASTContext &Ctx, ArrayRef<ReturnInfo> Branches):
    ReturnType(Ctx.TheErrorType.getPointer()), Exit(ExitState::Unsure) {
  std::set<TypeBase*> AllTypes;
  std::set<ExitState> AllExitStates;
  for (auto I : Branches) {
    AllTypes.insert(I.ReturnType);
    AllExitStates.insert(I.Exit);
  }
  if (AllTypes.size() == 1) {
    ReturnType = *AllTypes.begin();
  }
  if (AllExitStates.size() == 1) {
    Exit = *AllExitStates.begin();
  }
}

CharSourceRange CallArgInfo::getEntireCharRange(const SourceManager &SM) const {
  return CharSourceRange(SM, LabelRange.getStart(),
                         Lexer::getLocForEndOfToken(SM, ArgExp->getEndLoc()));
}

static Expr* getSingleNonImplicitChild(Expr *Parent) {
  // If this expr is non-implicit, we are done.
  if (!Parent->isImplicit())
    return Parent;

  // Collect all immediate children.
  llvm::SmallVector<Expr*, 4> Children;
  Parent->forEachImmediateChildExpr([&](Expr *E) {
    Children.push_back(E);
    return E;
  });

  // If more than one children are found, we are not sure the non-implicit node.
  if (Children.size() != 1)
    return Parent;

  // Dig deeper if necessary.
  return getSingleNonImplicitChild(Children[0]);
}

std::vector<CallArgInfo> swift::ide::
getCallArgInfo(SourceManager &SM, ArgumentList *Args, LabelRangeEndAt EndKind) {
  std::vector<CallArgInfo> InfoVec;
  auto *OriginalArgs = Args->getOriginalArgs();
  for (auto ElemIndex : indices(*OriginalArgs)) {
    auto *Elem = OriginalArgs->getExpr(ElemIndex);

    SourceLoc LabelStart(Elem->getStartLoc());
    SourceLoc LabelEnd(LabelStart);

    bool IsTrailingClosure = OriginalArgs->isTrailingClosureIndex(ElemIndex);
    auto NameLoc = OriginalArgs->getLabelLoc(ElemIndex);
    if (NameLoc.isValid()) {
      LabelStart = NameLoc;
      if (EndKind == LabelRangeEndAt::LabelNameOnly || IsTrailingClosure) {
        LabelEnd = Lexer::getLocForEndOfToken(SM, NameLoc);
      }
    }
    InfoVec.push_back({getSingleNonImplicitChild(Elem),
                       CharSourceRange(SM, LabelStart, LabelEnd),
                       IsTrailingClosure});
  }
  return InfoVec;
}

std::pair<std::vector<CharSourceRange>, llvm::Optional<unsigned>>
swift::ide::getCallArgLabelRanges(SourceManager &SM, ArgumentList *Args,
                                  LabelRangeEndAt EndKind) {
  std::vector<CharSourceRange> Ranges;
  auto InfoVec = getCallArgInfo(SM, Args, EndKind);

  llvm::Optional<unsigned> FirstTrailing;
  auto I = std::find_if(InfoVec.begin(), InfoVec.end(), [](CallArgInfo &Info) {
    return Info.IsTrailingClosure;
  });
  if (I != InfoVec.end())
    FirstTrailing = std::distance(InfoVec.begin(), I);

  llvm::transform(InfoVec, std::back_inserter(Ranges),
                  [](CallArgInfo &Info) { return Info.LabelRange; });
  return {Ranges, FirstTrailing};
}

//===--- PreCheckExpr.cpp - Expression pre-checking pass ------------------===//
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
//
// Pre-checking resolves unqualified name references, type expressions and
// operators.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "TypoCorrection.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Parse/Confusables.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace constraints;

//===----------------------------------------------------------------------===//
// High-level entry points.
//===----------------------------------------------------------------------===//

static unsigned getNumArgs(ValueDecl *value) {
  if (auto *func = dyn_cast<FuncDecl>(value))
    return func->getParameters()->size();
  return ~0U;
}

static bool matchesDeclRefKind(ValueDecl *value, DeclRefKind refKind) {
  switch (refKind) {
  // An ordinary reference doesn't ignore anything.
  case DeclRefKind::Ordinary:
    return true;

  // A binary-operator reference only honors FuncDecls with a certain type.
  case DeclRefKind::BinaryOperator:
    return (getNumArgs(value) == 2);

  case DeclRefKind::PrefixOperator:
    return (!value->getAttrs().hasAttribute<PostfixAttr>() &&
            getNumArgs(value) == 1);

  case DeclRefKind::PostfixOperator:
    return (value->getAttrs().hasAttribute<PostfixAttr>() &&
            getNumArgs(value) == 1);
  }
  llvm_unreachable("bad declaration reference kind");
}

static bool containsDeclRefKind(LookupResult &lookupResult,
                                DeclRefKind refKind) {
  for (auto candidate : lookupResult) {
    ValueDecl *D = candidate.getValueDecl();
    if (!D)
      continue;
    if (matchesDeclRefKind(D, refKind))
      return true;
  }
  return false;
}

/// Emit a diagnostic with a fixit hint for an invalid binary operator, showing
/// how to split it according to splitCandidate.
static void diagnoseBinOpSplit(ASTContext &Context, UnresolvedDeclRefExpr *UDRE,
                               std::pair<unsigned, bool> splitCandidate,
                               Diag<Identifier, Identifier, bool> diagID) {

  unsigned splitLoc = splitCandidate.first;
  bool isBinOpFirst = splitCandidate.second;
  StringRef nameStr = UDRE->getName().getBaseIdentifier().str();
  auto startStr = nameStr.substr(0, splitLoc);
  auto endStr = nameStr.drop_front(splitLoc);

  // One valid split found, it is almost certainly the right answer.
  auto diag = Context.Diags.diagnose(
      UDRE->getLoc(), diagID, Context.getIdentifier(startStr),
      Context.getIdentifier(endStr), isBinOpFirst);
  // Highlight the whole operator.
  diag.highlight(UDRE->getLoc());
  // Insert whitespace on the left if the binop is at the start, or to the
  // right if it is end.
  if (isBinOpFirst)
    diag.fixItInsert(UDRE->getLoc(), " ");
  else
    diag.fixItInsertAfter(UDRE->getLoc(), " ");

  // Insert a space between the operators.
  diag.fixItInsert(UDRE->getLoc().getAdvancedLoc(splitLoc), " ");
}

/// If we failed lookup of a binary operator, check to see it to see if
/// it is a binary operator juxtaposed with a unary operator (x*-4) that
/// needs whitespace.  If so, emit specific diagnostics for it and return true,
/// otherwise return false.
static bool diagnoseOperatorJuxtaposition(UnresolvedDeclRefExpr *UDRE,
                                          DeclContext *DC) {
  Identifier name = UDRE->getName().getBaseIdentifier();
  StringRef nameStr = name.str();
  if (!name.isOperator() || nameStr.size() < 2)
    return false;

  bool isBinOp = UDRE->getRefKind() == DeclRefKind::BinaryOperator;

  // If this is a binary operator, relex the token, to decide whether it has
  // whitespace around it or not.  If it does "x +++ y", then it isn't likely to
  // be a case where a space was forgotten.
  auto &Context = DC->getASTContext();
  if (isBinOp) {
    auto tok = Lexer::getTokenAtLocation(Context.SourceMgr, UDRE->getLoc());
    if (tok.getKind() != tok::oper_binary_unspaced)
      return false;
  }

  // Okay, we have a failed lookup of a multicharacter operator. Check to see if
  // lookup succeeds if part is split off, and record the matches found.
  //
  // In the case of a binary operator, the bool indicated is false if the
  // first half of the split is the unary operator (x!*4) or true if it is the
  // binary operator (x*+4).
  std::vector<std::pair<unsigned, bool>> WorkableSplits;

  // Check all the potential splits.
  for (unsigned splitLoc = 1, e = nameStr.size(); splitLoc != e; ++splitLoc) {
    // For it to be a valid split, the start and end section must be valid
    // operators, splitting a unicode code point isn't kosher.
    auto startStr = nameStr.substr(0, splitLoc);
    auto endStr = nameStr.drop_front(splitLoc);
    if (!Lexer::isOperator(startStr) || !Lexer::isOperator(endStr))
      continue;

    DeclNameRef startName(Context.getIdentifier(startStr));
    DeclNameRef endName(Context.getIdentifier(endStr));

    // Perform name lookup for the first and second pieces.  If either fail to
    // be found, then it isn't a valid split.
    auto startLookup = TypeChecker::lookupUnqualified(
        DC, startName, UDRE->getLoc(), defaultUnqualifiedLookupOptions);
    if (!startLookup) continue;
    auto endLookup = TypeChecker::lookupUnqualified(DC, endName, UDRE->getLoc(),
                                                    defaultUnqualifiedLookupOptions);
    if (!endLookup) continue;

    // If the overall operator is a binary one, then we're looking at
    // juxtaposed binary and unary operators.
    if (isBinOp) {
      // Look to see if the candidates found could possibly match.
      if (containsDeclRefKind(startLookup, DeclRefKind::PostfixOperator) &&
          containsDeclRefKind(endLookup, DeclRefKind::BinaryOperator))
        WorkableSplits.push_back({ splitLoc, false });

      if (containsDeclRefKind(startLookup, DeclRefKind::BinaryOperator) &&
          containsDeclRefKind(endLookup, DeclRefKind::PrefixOperator))
        WorkableSplits.push_back({ splitLoc, true });
    } else {
      // Otherwise, it is two of the same kind, e.g. "!!x" or "!~x".
      if (containsDeclRefKind(startLookup, UDRE->getRefKind()) &&
          containsDeclRefKind(endLookup, UDRE->getRefKind()))
        WorkableSplits.push_back({ splitLoc, false });
    }
  }

  switch (WorkableSplits.size()) {
  case 0:
    // No splits found, can't produce this diagnostic.
    return false;
  case 1:
    // One candidate: produce an error with a fixit on it.
    if (isBinOp)
      diagnoseBinOpSplit(Context, UDRE, WorkableSplits[0],
                         diag::unspaced_binary_operator_fixit);
    else
      Context.Diags.diagnose(
          UDRE->getLoc().getAdvancedLoc(WorkableSplits[0].first),
          diag::unspaced_unary_operator);
    return true;

  default:
    // Otherwise, we have to produce a series of notes listing the various
    // options.
    Context.Diags
        .diagnose(UDRE->getLoc(), isBinOp ? diag::unspaced_binary_operator
                                          : diag::unspaced_unary_operator)
        .highlight(UDRE->getLoc());

    if (isBinOp) {
      for (auto candidateSplit : WorkableSplits)
        diagnoseBinOpSplit(Context, UDRE, candidateSplit,
                           diag::unspaced_binary_operators_candidate);
    }
    return true;
  }
}

static bool diagnoseRangeOperatorMisspell(DiagnosticEngine &Diags,
                                          UnresolvedDeclRefExpr *UDRE) {
  auto name = UDRE->getName().getBaseIdentifier();
  if (!name.isOperator())
    return false;

  auto corrected = StringRef();
  if (name.str() == ".." || name.str() == "...." ||
      name.str() == ".…" || name.str() == "…" || name.str() == "….")
    corrected = "...";
  else if (name.str() == "...<" || name.str() == "....<" ||
           name.str() == "…<")
    corrected = "..<";

  if (!corrected.empty()) {
    Diags
        .diagnose(UDRE->getLoc(), diag::cannot_find_in_scope_corrected,
                  UDRE->getName(), true, corrected)
        .highlight(UDRE->getSourceRange())
        .fixItReplace(UDRE->getSourceRange(), corrected);

    return true;
  }
  return false;
}

static bool diagnoseNonexistentPowerOperator(DiagnosticEngine &Diags,
                                             UnresolvedDeclRefExpr *UDRE,
                                             DeclContext *DC) {
  auto name = UDRE->getName().getBaseIdentifier();
  if (!(name.isOperator() && name.is("**")))
    return false;

  DC = DC->getModuleScopeContext();

  auto &ctx = DC->getASTContext();
  DeclNameRef powerName(ctx.getIdentifier("pow"));

  // Look if 'pow(_:_:)' exists within current context.
  auto lookUp = TypeChecker::lookupUnqualified(
      DC, powerName, UDRE->getLoc(), defaultUnqualifiedLookupOptions);
  if (lookUp) {
    Diags.diagnose(UDRE->getLoc(), diag::nonexistent_power_operator)
        .highlight(UDRE->getSourceRange());
    return true;
  }

  return false;
}

static bool diagnoseIncDecOperator(DiagnosticEngine &Diags,
                                   UnresolvedDeclRefExpr *UDRE) {
  auto name = UDRE->getName().getBaseIdentifier();
  if (!name.isOperator())
    return false;

  auto corrected = StringRef();
  if (name.str() == "++")
    corrected = "+= 1";
  else if (name.str() == "--")
    corrected = "-= 1";

  if (!corrected.empty()) {
    Diags
        .diagnose(UDRE->getLoc(), diag::cannot_find_in_scope_corrected,
                  UDRE->getName(), true, corrected)
        .highlight(UDRE->getSourceRange());

    return true;
  }
  return false;
}

static bool findNonMembers(ArrayRef<LookupResultEntry> lookupResults,
                           DeclRefKind refKind, bool breakOnMember,
                           SmallVectorImpl<ValueDecl *> &ResultValues,
                           llvm::function_ref<bool(ValueDecl *)> isValid) {
  bool AllDeclRefs = true;
  for (auto Result : lookupResults) {
    // If we find a member, then all of the results aren't non-members.
    bool IsMember =
        (Result.getBaseDecl() && !isa<ModuleDecl>(Result.getBaseDecl()));
    if (IsMember) {
      AllDeclRefs = false;
      if (breakOnMember)
        break;
      continue;
    }

    ValueDecl *D = Result.getValueDecl();
    if (!isValid(D))
      return false;

    if (matchesDeclRefKind(D, refKind))
      ResultValues.push_back(D);
  }

  return AllDeclRefs;
}

/// Find the next element in a chain of members. If this expression is (or
/// could be) the base of such a chain, this will return \c nullptr.
static Expr *getMemberChainSubExpr(Expr *expr) {
  assert(expr && "getMemberChainSubExpr called with null expr!");
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
    return UDE->getBase();
  } else if (auto *CE = dyn_cast<CallExpr>(expr)) {
    return CE->getFn();
  } else if (auto *BOE = dyn_cast<BindOptionalExpr>(expr)) {
    return BOE->getSubExpr();
  } else if (auto *FVE = dyn_cast<ForceValueExpr>(expr)) {
    return FVE->getSubExpr();
  } else if (auto *SE = dyn_cast<SubscriptExpr>(expr)) {
    return SE->getBase();
  } else if (auto *CCE = dyn_cast<CodeCompletionExpr>(expr)) {
    return CCE->getBase();
  } else {
    return nullptr;
  }
}

UnresolvedMemberExpr *TypeChecker::getUnresolvedMemberChainBase(Expr *expr) {
  if (auto *subExpr = getMemberChainSubExpr(expr))
    return getUnresolvedMemberChainBase(subExpr);
  else
    return dyn_cast<UnresolvedMemberExpr>(expr);
}

/// Whether this expression sits at the end of a chain of member accesses.
static bool isMemberChainTail(Expr *expr, Expr *parent) {
  assert(expr && "isMemberChainTail called with null expr!");
  // If this expression's parent is not itself part of a chain (or, this expr
  // has no parent expr), this must be the tail of the chain.
  return !parent || getMemberChainSubExpr(parent) != expr;
}

static bool isValidForwardReference(ValueDecl *D, DeclContext *DC,
                                    ValueDecl **localDeclAfterUse) {
  *localDeclAfterUse = nullptr;

  // References to variables injected by lldb are always valid.
  if (isa<VarDecl>(D) && cast<VarDecl>(D)->isDebuggerVar())
    return true;

  // If we find something in the current context, it must be a forward
  // reference, because otherwise if it was in scope, it would have
  // been returned by the call to ASTScope::lookupLocalDecls() above.
  if (D->getDeclContext()->isLocalContext()) {
    do {
      if (D->getDeclContext() == DC) {
        *localDeclAfterUse = D;
        return false;
      }

      // If we're inside of a 'defer' context, walk up to the parent
      // and check again. We don't want 'defer' bodies to forward
      // reference bindings in the immediate outer scope.
    } while (isa<FuncDecl>(DC) &&
             cast<FuncDecl>(DC)->isDeferBody() &&
             (DC = DC->getParent()));
  }
  return true;
}

/// Checks whether this is a BinaryExpr with operator `&` and returns the
/// BinaryExpr, if so.
static BinaryExpr *getCompositionExpr(Expr *expr) {
  if (auto *binaryExpr = dyn_cast<BinaryExpr>(expr)) {
    // look at the name of the operator, if it is a '&' we can create the
    // composition TypeExpr
    auto fn = binaryExpr->getFn();
    if (auto Overload = dyn_cast<OverloadedDeclRefExpr>(fn)) {
      if (llvm::any_of(Overload->getDecls(), [](auto *decl) -> bool {
            return decl->getBaseName() == "&";
          }))
        return binaryExpr;
    } else if (auto *Decl = dyn_cast<UnresolvedDeclRefExpr>(fn)) {
      if (Decl->getName().isSimpleName() &&
          Decl->getName().getBaseName() == "&")
        return binaryExpr;
    }
  }

  return nullptr;
}

/// Bind an UnresolvedDeclRefExpr by performing name lookup and
/// returning the resultant expression. Context is the DeclContext used
/// for the lookup.
Expr *TypeChecker::resolveDeclRefExpr(UnresolvedDeclRefExpr *UDRE,
                                      DeclContext *DC,
                                      bool replaceInvalidRefsWithErrors) {
  // Process UnresolvedDeclRefExpr by doing an unqualified lookup.
  DeclNameRef Name = UDRE->getName();
  SourceLoc Loc = UDRE->getLoc();

  DeclNameRef LookupName = Name;
  if (Name.isCompoundName()) {
    auto &context = DC->getASTContext();

    // Remove any $ prefixes for lookup
    SmallVector<Identifier, 4> lookupLabels;
    for (auto label : Name.getArgumentNames()) {
      if (label.hasDollarPrefix()) {
        auto unprefixed = label.str().drop_front();
        lookupLabels.push_back(context.getIdentifier(unprefixed));
      } else {
        lookupLabels.push_back(label);
      }
    }

    DeclName lookupName(context, Name.getBaseName(), lookupLabels);
    LookupName = DeclNameRef(lookupName);
  }

  auto errorResult = [&]() -> Expr * {
    if (replaceInvalidRefsWithErrors)
      return new (DC->getASTContext()) ErrorExpr(UDRE->getSourceRange());
    return UDRE;
  };

  // Perform standard value name lookup.
  NameLookupOptions lookupOptions = defaultUnqualifiedLookupOptions;
  // TODO: Include all of the possible members to give a solver a
  //       chance to diagnose name shadowing which requires explicit
  //       name/module qualifier to access top-level name.
  lookupOptions |= NameLookupFlags::IncludeOuterResults;

  LookupResult Lookup;

  bool AllDeclRefs = true;
  SmallVector<ValueDecl*, 4> ResultValues;

  auto &Context = DC->getASTContext();

  // First, look for a local binding in scope.
  if (Loc.isValid() && !Name.isOperator()) {
    ASTScope::lookupLocalDecls(DC->getParentSourceFile(),
                               LookupName.getFullName(), Loc,
                               /*stopAfterInnermostBraceStmt=*/false,
                               ResultValues);
    for (auto *localDecl : ResultValues) {
      Lookup.add(LookupResultEntry(localDecl), /*isOuter=*/false);
    }
  }

  if (!Lookup) {
    // Now, look for all local bindings, even forward references, as well
    // as type members and top-level declarations.
    if (Loc.isInvalid())
      DC = DC->getModuleScopeContext();

    Lookup = TypeChecker::lookupUnqualified(DC, LookupName, Loc, lookupOptions);

    ValueDecl *localDeclAfterUse = nullptr;
    AllDeclRefs =
        findNonMembers(Lookup.innerResults(), UDRE->getRefKind(),
                       /*breakOnMember=*/true, ResultValues,
                       [&](ValueDecl *D) {
                         return isValidForwardReference(D, DC, &localDeclAfterUse);
                       });

    // If local declaration after use is found, check outer results for
    // better matching candidates.
    if (ResultValues.empty() && localDeclAfterUse) {
      auto innerDecl = localDeclAfterUse;
      while (localDeclAfterUse) {
        if (Lookup.outerResults().empty()) {
          Context.Diags.diagnose(Loc, diag::use_local_before_declaration, Name);
          Context.Diags.diagnose(innerDecl, diag::decl_declared_here,
                                 localDeclAfterUse->getName());
          Expr *error = new (Context) ErrorExpr(UDRE->getSourceRange());
          return error;
        }

        Lookup.shiftDownResults();
        ResultValues.clear();
        localDeclAfterUse = nullptr;
        AllDeclRefs =
            findNonMembers(Lookup.innerResults(), UDRE->getRefKind(),
                           /*breakOnMember=*/true, ResultValues,
                           [&](ValueDecl *D) {
                             return isValidForwardReference(D, DC, &localDeclAfterUse);
                           });
      }
    }
  }

  if (!Lookup) {
    // If we failed lookup of an operator, check to see if this is a range
    // operator misspelling. Otherwise try to diagnose a juxtaposition
    // e.g. (x*-4) that needs whitespace.
    if (diagnoseRangeOperatorMisspell(Context.Diags, UDRE) ||
        diagnoseIncDecOperator(Context.Diags, UDRE) ||
        diagnoseOperatorJuxtaposition(UDRE, DC) ||
        diagnoseNonexistentPowerOperator(Context.Diags, UDRE, DC)) {
      return errorResult();
    }

    // Try ignoring access control.
    NameLookupOptions relookupOptions = lookupOptions;
    relookupOptions |= NameLookupFlags::IgnoreAccessControl;
    auto inaccessibleResults =
        TypeChecker::lookupUnqualified(DC, LookupName, Loc, relookupOptions);
    if (inaccessibleResults) {
      // FIXME: What if the unviable candidates have different levels of access?
      const ValueDecl *first = inaccessibleResults.front().getValueDecl();
      Context.Diags.diagnose(
          Loc, diag::candidate_inaccessible, first->getBaseName(),
          first->getFormalAccessScope().accessLevelForDiagnostics());

      // FIXME: If any of the candidates (usually just one) are in the same
      // module we could offer a fix-it.
      for (auto lookupResult : inaccessibleResults) {
        auto *VD = lookupResult.getValueDecl();
        VD->diagnose(diag::decl_declared_here, VD->getName());
      }

      // Don't try to recover here; we'll get more access-related diagnostics
      // downstream if the type of the inaccessible decl is also inaccessible.
      return errorResult();
    }

    // TODO: Name will be a compound name if it was written explicitly as
    // one, but we should also try to propagate labels into this.
    DeclNameLoc nameLoc = UDRE->getNameLoc();

    Identifier simpleName = Name.getBaseIdentifier();
    const char *buffer = simpleName.get();
    llvm::SmallString<64> expectedIdentifier;
    bool isConfused = false;
    uint32_t codepoint;
    uint32_t firstConfusableCodepoint = 0;
    int totalCodepoints = 0;
    int offset = 0;
    while ((codepoint = validateUTF8CharacterAndAdvance(buffer,
                                                        buffer +
                                                        strlen(buffer)))
           != ~0U) {
      int length = (buffer - simpleName.get()) - offset;
      if (auto expectedCodepoint =
          confusable::tryConvertConfusableCharacterToASCII(codepoint)) {
        if (firstConfusableCodepoint == 0) {
          firstConfusableCodepoint = codepoint;
        }
        isConfused = true;
        expectedIdentifier += expectedCodepoint;
      } else {
        expectedIdentifier += (char)codepoint;
      }

      totalCodepoints++;

      offset += length;
    }

    auto emitBasicError = [&] {
      
      if (Name.isSimpleName(Context.Id_self)) {
        // `self` gets diagnosed with a different error when it can't be found.
        Context.Diags
            .diagnose(Loc, diag::cannot_find_self_in_scope)
            .highlight(UDRE->getSourceRange());
      } else {
        Context.Diags
            .diagnose(Loc, diag::cannot_find_in_scope, Name,
                      Name.isOperator())
            .highlight(UDRE->getSourceRange());
      }

      if (!Context.LangOpts.DisableExperimentalClangImporterDiagnostics) {
        Context.getClangModuleLoader()->diagnoseTopLevelValue(
            Name.getFullName());
      }
    };

    if (!isConfused) {
      if (Name.isSimpleName(Context.Id_Self)) {
        if (DeclContext *typeContext = DC->getInnermostTypeContext()){
          Type SelfType = typeContext->getSelfInterfaceType();

          if (typeContext->getSelfClassDecl())
            SelfType = DynamicSelfType::get(SelfType, Context);
          return new (Context)
              TypeExpr(new (Context) SelfTypeRepr(SelfType, Loc));
        }
      }

      TypoCorrectionResults corrections(Name, nameLoc);

      // FIXME: Don't perform typo correction inside macro arguments, because it
      // will invoke synthesizing declarations in this scope, which will attempt to
      // expand this macro which leads to circular reference errors.
      if (!namelookup::isInMacroArgument(DC->getParentSourceFile(), UDRE->getLoc())) {
        TypeChecker::performTypoCorrection(DC, UDRE->getRefKind(), Type(),
                                           lookupOptions, corrections);
      }

      if (auto typo = corrections.claimUniqueCorrection()) {
        auto diag = Context.Diags.diagnose(
            Loc, diag::cannot_find_in_scope_corrected, Name,
            Name.isOperator(), typo->CorrectedName.getBaseIdentifier().str());
        diag.highlight(UDRE->getSourceRange());
        typo->addFixits(diag);
      } else {
        emitBasicError();
      }

      corrections.noteAllCandidates();
    } else {
      emitBasicError();

      if (totalCodepoints == 1) {
        auto charNames = confusable::getConfusableAndBaseCodepointNames(
            firstConfusableCodepoint);
        Context.Diags
            .diagnose(Loc, diag::single_confusable_character,
                      UDRE->getName().isOperator(), simpleName.str(),
                      charNames.first, expectedIdentifier, charNames.second)
            .fixItReplace(Loc, expectedIdentifier);
      } else {
        Context.Diags
            .diagnose(Loc, diag::confusable_character,
                      UDRE->getName().isOperator(), simpleName.str(),
                      expectedIdentifier)
            .fixItReplace(Loc, expectedIdentifier);
      }
    }

    // TODO: consider recovering from here.  We may want some way to suppress
    // downstream diagnostics, though.

    return errorResult();
  }

  // FIXME: Need to refactor the way we build an AST node from a lookup result!

  // If we have an unambiguous reference to a type decl, form a TypeExpr.
  if (Lookup.size() == 1 && UDRE->getRefKind() == DeclRefKind::Ordinary &&
      isa<TypeDecl>(Lookup[0].getValueDecl())) {
    auto *D = cast<TypeDecl>(Lookup[0].getValueDecl());
    // FIXME: This is odd.
    if (isa<ModuleDecl>(D)) {
      return new (Context) DeclRefExpr(D, UDRE->getNameLoc(),
                                       /*Implicit=*/false,
                                       AccessSemantics::Ordinary,
                                       D->getInterfaceType());
    }

    auto *LookupDC = Lookup[0].getDeclContext();
    if (UDRE->isImplicit()) {
      return TypeExpr::createImplicitForDecl(
          UDRE->getNameLoc(), D, LookupDC,
          // It might happen that LookupDC is null if this is checking
          // synthesized code, in that case, don't map the type into context,
          // but return as is -- the synthesis should ensure the type is correct.
          LookupDC ? LookupDC->mapTypeIntoContext(D->getInterfaceType())
                   : D->getInterfaceType());
    } else {
      return TypeExpr::createForDecl(UDRE->getNameLoc(), D, LookupDC);
    }
  }

  if (AllDeclRefs) {
    // Diagnose uses of operators that found no matching candidates.
    if (ResultValues.empty()) {
      assert(UDRE->getRefKind() != DeclRefKind::Ordinary);
      Context.Diags.diagnose(
          Loc, diag::use_nonmatching_operator, Name,
          UDRE->getRefKind() == DeclRefKind::BinaryOperator
              ? 0
              : UDRE->getRefKind() == DeclRefKind::PrefixOperator ? 1 : 2);
      return new (Context) ErrorExpr(UDRE->getSourceRange());
    }

    // For operators, sort the results so that non-generic operations come
    // first.
    // Note: this is part of a performance hack to prefer non-generic operators
    // to generic operators, because the former is far more efficient to check.
    if (UDRE->getRefKind() != DeclRefKind::Ordinary) {
      std::stable_sort(ResultValues.begin(), ResultValues.end(),
                       [&](ValueDecl *x, ValueDecl *y) -> bool {
        auto xGeneric = x->getInterfaceType()->getAs<GenericFunctionType>();
        auto yGeneric = y->getInterfaceType()->getAs<GenericFunctionType>();
        if (static_cast<bool>(xGeneric) != static_cast<bool>(yGeneric)) {
          return xGeneric? false : true;
        }

        if (!xGeneric)
          return false;

        unsigned xDepth = xGeneric->getGenericParams().back()->getDepth();
        unsigned yDepth = yGeneric->getGenericParams().back()->getDepth();
        return xDepth < yDepth;
      });
    }

    return buildRefExpr(ResultValues, DC, UDRE->getNameLoc(),
                        UDRE->isImplicit(), UDRE->getFunctionRefKind());
  }

  ResultValues.clear();
  bool AllMemberRefs = true;
  ValueDecl *Base = nullptr;
  DeclContext *BaseDC = nullptr;
  for (auto Result : Lookup) {
    auto ThisBase = Result.getBaseDecl();

    // Track the base for member declarations.
    if (ThisBase && !isa<ModuleDecl>(ThisBase)) {
      auto Value = Result.getValueDecl();
      ResultValues.push_back(Value);
      if (Base && ThisBase != Base) {
        AllMemberRefs = false;
        break;
      }

      Base = ThisBase;
      BaseDC = Result.getDeclContext();
      continue;
    }

    AllMemberRefs = false;
    break;
  }

  if (AllMemberRefs) {
    Expr *BaseExpr;
    if (auto PD = dyn_cast<ProtocolDecl>(Base)) {
      auto selfParam = PD->getGenericParams()->getParams().front();
      BaseExpr = TypeExpr::createImplicitForDecl(
          UDRE->getNameLoc(), selfParam,
          /*DC*/ nullptr,
          DC->mapTypeIntoContext(selfParam->getInterfaceType()));
    } else if (auto NTD = dyn_cast<NominalTypeDecl>(Base)) {
      BaseExpr = TypeExpr::createImplicitForDecl(
          UDRE->getNameLoc(), NTD, BaseDC,
          DC->mapTypeIntoContext(NTD->getInterfaceType()));
    } else {
      BaseExpr = new (Context) DeclRefExpr(Base, UDRE->getNameLoc(),
                                           /*Implicit=*/true);
    }

    auto isInClosureContext = [&](ValueDecl *decl) -> bool {
      auto *DC = decl->getDeclContext();
      do {
        if (dyn_cast<ClosureExpr>(DC))
          return true;
      } while ((DC = DC->getParent()));

      return false;
    };

    llvm::SmallVector<ValueDecl *, 4> outerAlternatives;
    (void)findNonMembers(Lookup.outerResults(), UDRE->getRefKind(),
                         /*breakOnMember=*/false, outerAlternatives,
                         /*isValid=*/[&](ValueDecl *choice) -> bool {
                           // Values that are defined in a closure
                           // that hasn't been type-checked yet,
                           // cannot be outer candidates.
                           if (isInClosureContext(choice)) {
                             return choice->hasInterfaceType() &&
                                    !choice->isInvalid();
                           }
                           return !choice->isInvalid();
                         });

    // Otherwise, form an UnresolvedDotExpr and sema will resolve it based on
    // type information.
    return new (Context) UnresolvedDotExpr(
        BaseExpr, SourceLoc(), Name, UDRE->getNameLoc(), UDRE->isImplicit(),
        Context.AllocateCopy(outerAlternatives));
  }
  
  // FIXME: If we reach this point, the program we're being handed is likely
  // very broken, but it's still conceivable that this may happen due to
  // invalid shadowed declarations.
  //
  // Make sure we emit a diagnostic, since returning an ErrorExpr without
  // producing one will break things downstream.
  Context.Diags.diagnose(Loc, diag::ambiguous_decl_ref, Name);
  for (auto Result : Lookup) {
    auto *Decl = Result.getValueDecl();
    Context.Diags.diagnose(Decl, diag::decl_declared_here, Decl->getName());
  }
  return new (Context) ErrorExpr(UDRE->getSourceRange());
}

/// If an expression references 'self.init' or 'super.init' in an
/// initializer context, returns the implicit 'self' decl of the constructor.
/// Otherwise, return nil.
VarDecl *
TypeChecker::getSelfForInitDelegationInConstructor(DeclContext *DC,
                                                   UnresolvedDotExpr *ctorRef) {
  // If the reference isn't to a constructor, we're done.
  if (ctorRef->getName().getBaseName() != DeclBaseName::createConstructor())
    return nullptr;

  if (auto ctorContext =
          dyn_cast_or_null<ConstructorDecl>(DC->getInnermostMethodContext())) {
    auto nestedArg = ctorRef->getBase();
    if (auto inout = dyn_cast<InOutExpr>(nestedArg))
      nestedArg = inout->getSubExpr();
    if (nestedArg->isSuperExpr())
      return ctorContext->getImplicitSelfDecl();
    if (auto declRef = dyn_cast<DeclRefExpr>(nestedArg))
      if (declRef->getDecl()->getName() == DC->getASTContext().Id_self)
        return ctorContext->getImplicitSelfDecl();
  }
  return nullptr;
}

namespace {
  /// Update the function reference kind based on adding a direct call to a
  /// callee with this kind.
  FunctionRefKind addingDirectCall(FunctionRefKind kind) {
    switch (kind) {
    case FunctionRefKind::Unapplied:
      return FunctionRefKind::SingleApply;

    case FunctionRefKind::SingleApply:
    case FunctionRefKind::DoubleApply:
      return FunctionRefKind::DoubleApply;

    case FunctionRefKind::Compound:
      return FunctionRefKind::Compound;
    }

    llvm_unreachable("Unhandled FunctionRefKind in switch.");
  }

  /// Update a direct callee expression node that has a function reference kind
  /// based on seeing a call to this callee.
  template<typename E,
           typename = decltype(((E*)nullptr)->getFunctionRefKind())>
  void tryUpdateDirectCalleeImpl(E *callee, int) {
    callee->setFunctionRefKind(addingDirectCall(callee->getFunctionRefKind()));
  }

  /// Version of tryUpdateDirectCalleeImpl for when the callee
  /// expression type doesn't carry a reference.
  template<typename E>
  void tryUpdateDirectCalleeImpl(E *callee, ...) { }

  /// The given expression is the direct callee of a call expression; mark it to
  /// indicate that it has been called.
  void markDirectCallee(Expr *callee) {
    while (true) {
      // Look through identity expressions.
      if (auto identity = dyn_cast<IdentityExpr>(callee)) {
        callee = identity->getSubExpr();
        continue;
      }

      // Look through unresolved 'specialize' expressions.
      if (auto specialize = dyn_cast<UnresolvedSpecializeExpr>(callee)) {
        callee = specialize->getSubExpr();
        continue;
      }
      
      // Look through optional binding.
      if (auto bindOptional = dyn_cast<BindOptionalExpr>(callee)) {
        callee = bindOptional->getSubExpr();
        continue;
      }

      // Look through forced binding.
      if (auto force = dyn_cast<ForceValueExpr>(callee)) {
        callee = force->getSubExpr();
        continue;
      }

      // Calls compose.
      if (auto call = dyn_cast<CallExpr>(callee)) {
        callee = call->getFn();
        continue;
      }

      // We're done.
      break;
    }
                                
    // Cast the callee to its most-specific class, then try to perform an
    // update. If the expression node has a declaration reference in it, the
    // update will succeed. Otherwise, we're done propagating.
    switch (callee->getKind()) {
#define EXPR(Id, Parent)                                  \
    case ExprKind::Id:                                    \
      tryUpdateDirectCalleeImpl(cast<Id##Expr>(callee), 0); \
      break;
#include "swift/AST/ExprNodes.def"
    }
  }

  class PreCheckExpression : public ASTWalker {
    ASTContext &Ctx;
    DeclContext *DC;

    Expr *ParentExpr;

    /// Indicates whether pre-check is allowed to insert
    /// implicit `ErrorExpr` in place of invalid references.
    bool UseErrorExprs;

    bool LeaveClosureBodiesUnchecked;

    /// A stack of expressions being walked, used to determine where to
    /// insert RebindSelfInConstructorExpr nodes.
    llvm::SmallVector<Expr *, 8> ExprStack;

    /// The 'self' variable to use when rebinding 'self' in a constructor.
    VarDecl *UnresolvedCtorSelf = nullptr;

    /// The expression that will be wrapped by a RebindSelfInConstructorExpr
    /// node when visited.
    Expr *UnresolvedCtorRebindTarget = nullptr;

    /// Keep track of acceptable DiscardAssignmentExpr's.
    llvm::SmallPtrSet<DiscardAssignmentExpr*, 2> CorrectDiscardAssignmentExprs;

    /// The current number of nested \c SequenceExprs that we're within.
    unsigned SequenceExprDepth = 0;

    /// The current number of nested \c SingleValueStmtExprs that we're within.
    unsigned SingleValueStmtExprDepth = 0;

    /// Simplify expressions which are type sugar productions that got parsed
    /// as expressions due to the parser not knowing which identifiers are
    /// type names.
    TypeExpr *simplifyTypeExpr(Expr *E);

    /// Simplify unresolved dot expressions which are nested type productions.
    TypeExpr *simplifyNestedTypeExpr(UnresolvedDotExpr *UDE);

    TypeExpr *simplifyUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *USE);

    /// Simplify a key path expression into a canonical form.
    void resolveKeyPathExpr(KeyPathExpr *KPE);

    /// Simplify constructs like `UInt32(1)` into `1 as UInt32` if
    /// the type conforms to the expected literal protocol.
    ///
    /// \returns Either a transformed expression, or `ErrorExpr` upon type
    /// resolution failure, or `nullptr` if transformation is not applicable.
    Expr *simplifyTypeConstructionWithLiteralArg(Expr *E);

    /// Whether the given expression "looks like" a (possibly sugared) type. For
    /// example, `(foo, bar)` "looks like" a type, but `foo + bar` does not.
    bool exprLooksLikeAType(Expr *expr);

    /// Whether the current expression \p E is in a context that might turn out
    /// to be a \c TypeExpr after \c simplifyTypeExpr is called up the tree.
    /// This function allows us to make better guesses about whether invalid
    /// uses of '_' were "supposed" to be \c DiscardAssignmentExprs or patterns,
    /// which results in better diagnostics after type checking.
    bool possiblyInTypeContext(Expr *E);

    /// Whether we can simplify the given discard assignment expr. Not possible
    /// if it's been marked "valid" or if the current state of the AST disallows
    /// such simplification (see \c canSimplifyPlaceholderTypes above).
    bool canSimplifyDiscardAssignmentExpr(DiscardAssignmentExpr *DAE);

    /// In Swift < 5, diagnose and correct invalid multi-argument or
    /// argument-labeled interpolations. Returns \c true if the AST walk should
    /// continue, or \c false if it should be aborted.
    bool correctInterpolationIfStrange(InterpolatedStringLiteralExpr *ISLE);

    /// Scout out the specified destination of an AssignExpr to recursively
    /// identify DiscardAssignmentExpr in legal places.  We can only allow them
    /// in simple pattern-like expressions, so we reject anything complex here.
    void markAcceptableDiscardExprs(Expr *E);

  public:
    PreCheckExpression(DeclContext *dc, Expr *parent,
                       bool replaceInvalidRefsWithErrors,
                       bool leaveClosureBodiesUnchecked)
        : Ctx(dc->getASTContext()), DC(dc),
          ParentExpr(parent), UseErrorExprs(replaceInvalidRefsWithErrors),
          LeaveClosureBodiesUnchecked(leaveClosureBodiesUnchecked) {}

    ASTContext &getASTContext() const { return Ctx; }

    bool walkToClosureExprPre(ClosureExpr *expr);

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    VarDecl *getImplicitSelfDeclForSuperContext(SourceLoc Loc) ;

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      // FIXME(diagnostics): `InOutType` could appear here as a result
      // of successful re-typecheck of the one of the sub-expressions e.g.
      // `let _: Int = { (s: inout S) in s.bar() }`. On the first
      // attempt to type-check whole expression `s.bar()` - is going
      // to have a base which points directly to declaration of `S`.
      // But when diagnostics attempts to type-check `s.bar()` standalone
      // its base would be transformed into `InOutExpr -> DeclRefExr`,
      // and `InOutType` is going to be recorded in constraint system.
      // One possible way to fix this (if diagnostics still use typecheck)
      // might be to make it so self is not wrapped into `InOutExpr`
      // but instead used as @lvalue type in some case of mutable members.
      if (!expr->isImplicit()) {
        if (isa<MemberRefExpr>(expr) || isa<DynamicMemberRefExpr>(expr)) {
          auto *LE = cast<LookupExpr>(expr);
          if (auto *IOE = dyn_cast<InOutExpr>(LE->getBase()))
            LE->setBase(IOE->getSubExpr());
        }

        if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(expr)) {
          if (auto *IOE = dyn_cast<InOutExpr>(DSCE->getBase()))
            DSCE->setBase(IOE->getSubExpr());
        }
      }

      // Local function used to finish up processing before returning. Every
      // return site should call through here.
      auto finish = [&](bool recursive, Expr *expr) -> PreWalkResult<Expr *> {
        if (!expr)
          return Action::Stop();

        // If we're going to recurse, record this expression on the stack.
        if (recursive) {
          if (isa<SequenceExpr>(expr))
            SequenceExprDepth++;
          ExprStack.push_back(expr);
        }
        return Action::VisitChildrenIf(recursive, expr);
      };

      // Resolve 'super' references.
      if (auto *superRef = dyn_cast<SuperRefExpr>(expr)) {
        auto loc = superRef->getLoc();
        auto *selfDecl = getImplicitSelfDeclForSuperContext(loc);
        if (selfDecl == nullptr)
          return finish(true, new (Ctx) ErrorExpr(loc));

        superRef->setSelf(selfDecl);
        return finish(true, superRef);
      }

      // For closures, type-check the patterns and result type as written,
      // but do not walk into the body. That will be type-checked after
      // we've determine the complete function type.
      if (auto closure = dyn_cast<ClosureExpr>(expr))
        return finish(walkToClosureExprPre(closure), expr);

      if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
        // Record the scope of a single value stmt expr, as we want to skip
        // pre-checking of any patterns, similar to closures.
        SingleValueStmtExprDepth += 1;
        return finish(true, expr);
      }

      if (auto unresolved = dyn_cast<UnresolvedDeclRefExpr>(expr)) {
        TypeChecker::checkForForbiddenPrefix(
            getASTContext(), unresolved->getName().getBaseName());
        auto *refExpr =
            TypeChecker::resolveDeclRefExpr(unresolved, DC, UseErrorExprs);

        // Check whether this is standalone `self` in init accessor, which
        // is invalid.
        if (auto *accessor = DC->getInnermostPropertyAccessorContext()) {
          if (accessor->isInitAccessor() && isa<DeclRefExpr>(refExpr)) {
            auto *DRE = cast<DeclRefExpr>(refExpr);
            if (accessor->getImplicitSelfDecl() == DRE->getDecl() &&
                !isa_and_nonnull<UnresolvedDotExpr>(Parent.getAsExpr())) {
              Ctx.Diags.diagnose(unresolved->getLoc(),
                                 diag::invalid_use_of_self_in_init_accessor);
              refExpr = new (Ctx) ErrorExpr(unresolved->getSourceRange());
            }
          }
        }

        return finish(true, refExpr);
      }

      // Let's try to figure out if `InOutExpr` is out of place early
      // otherwise there is a risk of producing solutions which can't
      // be later applied to AST and would result in the crash in some
      // cases. Such expressions are only allowed in argument positions
      // of function/operator calls.
      if (isa<InOutExpr>(expr)) {
        // If this is an implicit `inout` expression we assume that
        // compiler knowns what it's doing.
        if (expr->isImplicit())
          return finish(true, expr);

        auto parents = ParentExpr->getParentMap();

        auto result = parents.find(expr);
        if (result != parents.end()) {
          auto *parent = result->getSecond();

          if (isa<SequenceExpr>(parent))
            return finish(true, expr);

          SourceLoc lastInnerParenLoc;
          // Unwrap to the outermost paren in the sequence.
          // e.g. `foo(((&bar))`
          while (auto *PE = dyn_cast<ParenExpr>(parent)) {
            auto nextParent = parents.find(parent);
            if (nextParent == parents.end())
              break;

            lastInnerParenLoc = PE->getLParenLoc();
            parent = nextParent->second;
          }

          if (isa<ApplyExpr>(parent) || isa<UnresolvedMemberExpr>(parent)) {
            // If outermost paren is associated with a call or
            // a member reference, it might be valid to have `&`
            // before all of the parens.
            if (lastInnerParenLoc.isValid()) {
              auto &DE = getASTContext().Diags;
              auto diag = DE.diagnose(expr->getStartLoc(),
                                      diag::extraneous_address_of);
              diag.fixItExchange(expr->getLoc(), lastInnerParenLoc);
            }
            return finish(true, expr);
          }

          if (isa<SubscriptExpr>(parent)) {
            getASTContext().Diags.diagnose(
                expr->getStartLoc(),
                diag::cannot_pass_inout_arg_to_subscript);
            return finish(false, nullptr);
          }
        }

        getASTContext().Diags.diagnose(expr->getStartLoc(),
                                       diag::extraneous_address_of);
        return finish(false, nullptr);
      }

      if (auto *ISLE = dyn_cast<InterpolatedStringLiteralExpr>(expr)) {
        if (!correctInterpolationIfStrange(ISLE))
          return finish(false, nullptr);
      }

      if (auto *assignment = dyn_cast<AssignExpr>(expr))
        markAcceptableDiscardExprs(assignment->getDest());

      return finish(true, expr);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      // Remove this expression from the stack.
      assert(ExprStack.back() == expr);
      ExprStack.pop_back();

      // Mark the direct callee as being a callee.
      if (auto *call = dyn_cast<ApplyExpr>(expr))
        markDirectCallee(call->getFn());

      // Fold sequence expressions.
      if (auto *seqExpr = dyn_cast<SequenceExpr>(expr)) {
        auto result = TypeChecker::foldSequence(seqExpr, DC);
        SequenceExprDepth--;
        result = result->walk(*this);
        if (!result)
          return Action::Stop();

        return Action::Continue(result);
      }

      // Type check the type parameters in an UnresolvedSpecializeExpr.
      if (auto *us = dyn_cast<UnresolvedSpecializeExpr>(expr)) {
        if (auto *typeExpr = simplifyUnresolvedSpecializeExpr(us))
          return Action::Continue(typeExpr);
      }
      
      // If we're about to step out of a ClosureExpr, restore the DeclContext.
      if (auto *ce = dyn_cast<ClosureExpr>(expr)) {
        assert(DC == ce && "DeclContext imbalance");
        DC = ce->getParent();
      }

      // Restore the depth for the single value stmt counter.
      if (isa<SingleValueStmtExpr>(expr))
        SingleValueStmtExprDepth -= 1;

      // A 'self.init' or 'super.init' application inside a constructor will
      // evaluate to void, with the initializer's result implicitly rebound
      // to 'self'. Recognize the unresolved constructor expression and
      // determine where to place the RebindSelfInConstructorExpr node.
      // When updating this logic, also update
      // RebindSelfInConstructorExpr::getCalledConstructor.
      auto &ctx = getASTContext();
      if (auto unresolvedDot = dyn_cast<UnresolvedDotExpr>(expr)) {
        if (auto self = TypeChecker::getSelfForInitDelegationInConstructor(
                DC, unresolvedDot)) {
          // Walk our ancestor expressions looking for the appropriate place
          // to insert the RebindSelfInConstructorExpr.
          Expr *target = nullptr;
          bool foundApply = false;
          bool foundRebind = false;
          for (auto ancestor : llvm::reverse(ExprStack)) {
            if (isa<RebindSelfInConstructorExpr>(ancestor)) {
              // If we already have a rebind, then we're re-typechecking an
              // expression and are done.
              foundRebind = true;
              break;
            }

            // Recognize applications.
            if (auto apply = dyn_cast<ApplyExpr>(ancestor)) {
              // If we already saw an application, we're done.
              if (foundApply)
                break;

              // If the function being called is not our unresolved initializer
              // reference, we're done.
              if (apply->getSemanticFn() != unresolvedDot)
                break;

              foundApply = true;
              target = ancestor;
              continue;
            }

            // Look through identity, force-value, and 'try' expressions.
            if (isa<IdentityExpr>(ancestor) ||
                isa<ForceValueExpr>(ancestor) ||
                isa<AnyTryExpr>(ancestor)) {
              if (target)
                target = ancestor;
              continue;
            }

            // No other expression kinds are permitted.
            break;
          }

          // If we found a rebind target, note the insertion point.
          if (target && !foundRebind) {
            UnresolvedCtorRebindTarget = target;
            UnresolvedCtorSelf = self;
          }
        }
      }

      // If the expression we've found is the intended target of an
      // RebindSelfInConstructorExpr, wrap it in the
      // RebindSelfInConstructorExpr.
      if (expr == UnresolvedCtorRebindTarget) {
        expr = new (ctx)
            RebindSelfInConstructorExpr(expr, UnresolvedCtorSelf);
        UnresolvedCtorRebindTarget = nullptr;
        return Action::Continue(expr);
      }

      // Double check if there are any BindOptionalExpr remaining in the
      // tree (see comment below for more details), if there are no BOE
      // expressions remaining remove OptionalEvaluationExpr from the tree.
      if (auto OEE = dyn_cast<OptionalEvaluationExpr>(expr)) {
        bool hasBindOptional = false;
        OEE->forEachChildExpr([&](Expr *expr) -> Expr * {
          if (isa<BindOptionalExpr>(expr))
            hasBindOptional = true;
          // If at least a single BOE was found, no reason
          // to walk any further in the tree.
          return hasBindOptional ? nullptr : expr;
        });

        return Action::Continue(hasBindOptional ? OEE : OEE->getSubExpr());
      }

      // Check if there are any BindOptionalExpr in the tree which
      // wrap DiscardAssignmentExpr, such situation corresponds to syntax
      // like - `_? = <value>`, since it doesn't really make
      // sense to have optional assignment to discarded LValue which can
      // never be optional, we can remove BOE from the tree and avoid
      // generating any of the unnecessary constraints.
      if (auto BOE = dyn_cast<BindOptionalExpr>(expr)) {
        if (auto DAE = dyn_cast<DiscardAssignmentExpr>(BOE->getSubExpr()))
          if (CorrectDiscardAssignmentExprs.count(DAE))
            return Action::Continue(DAE);
      }

      // If this is a sugared type that needs to be folded into a single
      // TypeExpr, do it.
      if (auto *simplified = simplifyTypeExpr(expr))
        return Action::Continue(simplified);

      // Diagnose a '_' that isn't on the immediate LHS of an assignment. We
      // skip diagnostics if we've explicitly marked the expression as valid,
      // or if we're inside a SequenceExpr (since the whole tree will be
      // re-checked when we finish folding anyway).
      if (auto *DAE = dyn_cast<DiscardAssignmentExpr>(expr)) {
        if (!CorrectDiscardAssignmentExprs.count(DAE) &&
            SequenceExprDepth == 0) {
          ctx.Diags.diagnose(expr->getLoc(),
                             diag::discard_expr_outside_of_assignment);
          return Action::Stop();
        }
      }

      if (auto KPE = dyn_cast<KeyPathExpr>(expr)) {
        resolveKeyPathExpr(KPE);
        return Action::Continue(KPE);
      }

      if (auto *result = simplifyTypeConstructionWithLiteralArg(expr)) {
        if (isa<ErrorExpr>(result))
           return Action::Stop();

        return Action::Continue(result);
      }

      // If we find an unresolved member chain, wrap it in an
      // UnresolvedMemberChainResultExpr (unless this has already been done).
      auto *parent = Parent.getAsExpr();
      if (isMemberChainTail(expr, parent)) {
        if (auto *UME = TypeChecker::getUnresolvedMemberChainBase(expr)) {
          if (!parent || !isa<UnresolvedMemberChainResultExpr>(parent)) {
            auto *chain = new (ctx) UnresolvedMemberChainResultExpr(expr, UME);
            return Action::Continue(chain);
          }
        }
      }
      return Action::Continue(expr);
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      if (auto *RS = dyn_cast<ReturnStmt>(stmt)) {
        // Pre-check a return statement, which includes potentially turning it
        // into a FailStmt.
        auto &eval = Ctx.evaluator;
        auto *S = evaluateOrDefault(eval, PreCheckReturnStmtRequest{RS, DC},
                                    nullptr);
        if (!S)
          return Action::Stop();

        return Action::Continue(S);
      }
      return Action::Continue(stmt);
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      return Action::VisitChildrenIf(isa<PatternBindingDecl>(D));
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *pattern) override {
      // Constraint generation is responsible for pattern verification and
      // type-checking in the body of the closure and single value stmt expr,
      // so there is no need to walk into patterns.
      return Action::SkipChildrenIf(
          isa<ClosureExpr>(DC) || SingleValueStmtExprDepth > 0, pattern);
    }
  };
} // end anonymous namespace

/// Perform prechecking of a ClosureExpr before we dive into it.  This returns
/// true when we want the body to be considered part of this larger expression.
bool PreCheckExpression::walkToClosureExprPre(ClosureExpr *closure) {
  // If we have a single statement that can become an expression, turn it
  // into an expression now. This needs to happen before we check
  // LeaveClosureBodiesUnchecked, as the closure may become a single expression
  // closure.
  auto *body = closure->getBody();
  if (auto *S = body->getSingleActiveStatement()) {
    if (S->mayProduceSingleValue(Ctx)) {
      auto *SVE = SingleValueStmtExpr::createWithWrappedBranches(
          Ctx, S, /*DC*/ closure, /*mustBeExpr*/ false);
      auto *RS = new (Ctx) ReturnStmt(SourceLoc(), SVE);
      body->setLastElement(RS);
      closure->setBody(body, /*isSingleExpression*/ true);
    }
  }

  // If we won't be checking the body of the closure, don't walk into it here.
  if (!closure->hasSingleExpressionBody()) {
    if (LeaveClosureBodiesUnchecked)
      return false;
  }

  // Update the current DeclContext to be the closure we're about to
  // recurse into.
  assert((closure->getParent() == DC ||
          closure->getParent()->isChildContextOf(DC)) &&
         "Decl context isn't correct");
  DC = closure;
  return true;
}

TypeExpr *PreCheckExpression::simplifyNestedTypeExpr(UnresolvedDotExpr *UDE) {
  if (!UDE->getName().isSimpleName() ||
      UDE->getName().isSpecial())
    return nullptr;

  auto Name = UDE->getName();
  auto NameLoc = UDE->getNameLoc().getBaseNameLoc();

  // Qualified type lookup with a module base is represented as a DeclRefExpr
  // and not a TypeExpr.
  auto handleNestedTypeLookup = [&](
      TypeDecl *TD, DeclNameLoc ParentNameLoc
    ) -> TypeExpr * {
      // See if the type has a member type with this name.
      auto Result = TypeChecker::lookupMemberType(
          DC, TD->getDeclaredInterfaceType(), Name,
          UDE->getLoc(), defaultMemberLookupOptions);

      // If there is no nested type with this name, we have a lookup of
      // a non-type member, so leave the expression as-is.
      if (Result.size() == 1) {
        return TypeExpr::createForMemberDecl(
            ParentNameLoc, TD, UDE->getNameLoc(), Result.front().Member);
      }

      return nullptr;
  };

  if (auto *DRE = dyn_cast<DeclRefExpr>(UDE->getBase())) {
    if (auto *TD = dyn_cast<TypeDecl>(DRE->getDecl()))
      return handleNestedTypeLookup(TD, DRE->getNameLoc());

    return nullptr;
  }

  // Determine whether there is exactly one type declaration, where all
  // other declarations are macros.
  if (auto *ODRE = dyn_cast<OverloadedDeclRefExpr>(UDE->getBase())) {
    TypeDecl *FoundTD = nullptr;
    for (auto *D : ODRE->getDecls()) {
      if (auto *TD = dyn_cast<TypeDecl>(D)) {
        if (FoundTD)
          return nullptr;

        FoundTD = TD;
        continue;
      }

        // Ignore macros; they can't have any nesting.
      if (isa<MacroDecl>(D))
        continue;

      // Anything else prevents folding.
      return nullptr;
    }

    if (FoundTD)
      return handleNestedTypeLookup(FoundTD, ODRE->getNameLoc());

    return nullptr;
  }

  auto *TyExpr = dyn_cast<TypeExpr>(UDE->getBase());
  if (!TyExpr)
    return nullptr;

  auto *InnerTypeRepr = TyExpr->getTypeRepr();
  if (!InnerTypeRepr)
    return nullptr;

  // Fold 'T.Protocol' into a protocol metatype.
  if (Name.isSimpleName(getASTContext().Id_Protocol)) {
    auto *NewTypeRepr =
        new (getASTContext()) ProtocolTypeRepr(InnerTypeRepr, NameLoc);
    return new (getASTContext()) TypeExpr(NewTypeRepr);
  }

  // Fold 'T.Type' into an existential metatype if 'T' is a protocol,
  // or an ordinary metatype otherwise.
  if (Name.isSimpleName(getASTContext().Id_Type)) {
    auto *NewTypeRepr =
        new (getASTContext()) MetatypeTypeRepr(InnerTypeRepr, NameLoc);
    return new (getASTContext()) TypeExpr(NewTypeRepr);
  }

  // Fold 'T.U' into a nested type.

  // Resolve the TypeRepr to get the base type for the lookup.
  TypeResolutionOptions options(TypeResolverContext::InExpression);
  // Pre-check always allows pack references during TypeExpr folding.
  // CSGen will diagnose cases that appear outside of pack expansion
  // expressions.
  options |= TypeResolutionFlags::AllowPackReferences;
  const auto BaseTy = TypeResolution::resolveContextualType(
      InnerTypeRepr, DC, options,
      [](auto unboundTy) {
        // FIXME: Don't let unbound generic types escape type resolution.
        // For now, just return the unbound generic type.
        return unboundTy;
      },
      // FIXME: Don't let placeholder types escape type resolution.
      // For now, just return the placeholder type.
      PlaceholderType::get,
      // TypeExpr pack elements are opened in CSGen.
      /*packElementOpener*/ nullptr);

  if (BaseTy->mayHaveMembers()) {
    // See if there is a member type with this name.
    auto Result = TypeChecker::lookupMemberType(DC, BaseTy, Name,
                                                UDE->getLoc(),
                                                defaultMemberLookupOptions);

    // If there is no nested type with this name, we have a lookup of
    // a non-type member, so leave the expression as-is.
    if (Result.size() == 1) {
      return TypeExpr::createForMemberDecl(InnerTypeRepr, UDE->getNameLoc(),
                                           Result.front().Member);
    }
  }

  return nullptr;
}

TypeExpr *PreCheckExpression::simplifyUnresolvedSpecializeExpr(
    UnresolvedSpecializeExpr *us) {
  // If this is a reference type a specialized type, form a TypeExpr.
  // The base should be a TypeExpr that we already resolved.
  if (auto *te = dyn_cast<TypeExpr>(us->getSubExpr())) {
    if (auto *declRefTR =
            dyn_cast_or_null<DeclRefTypeRepr>(te->getTypeRepr())) {
      return TypeExpr::createForSpecializedDecl(
          declRefTR, us->getUnresolvedParams(),
          SourceRange(us->getLAngleLoc(), us->getRAngleLoc()), getASTContext());
    }
  }

  return nullptr;
}

/// Whether the given expression "looks like" a (possibly sugared) type. For
/// example, `(foo, bar)` "looks like" a type, but `foo + bar` does not.
bool PreCheckExpression::exprLooksLikeAType(Expr *expr) {
  return isa<OptionalEvaluationExpr>(expr) ||
      isa<BindOptionalExpr>(expr) ||
      isa<ForceValueExpr>(expr) ||
      isa<ParenExpr>(expr) ||
      isa<ArrowExpr>(expr) ||
      isa<PackExpansionExpr>(expr) ||
      isa<PackElementExpr>(expr) ||
      isa<TupleExpr>(expr) ||
      (isa<ArrayExpr>(expr) &&
       cast<ArrayExpr>(expr)->getElements().size() == 1) ||
      (isa<DictionaryExpr>(expr) &&
       cast<DictionaryExpr>(expr)->getElements().size() == 1) ||
      getCompositionExpr(expr);
}

bool PreCheckExpression::possiblyInTypeContext(Expr *E) {
  // Walk back up the stack of parents looking for a valid type context.
  for (auto *ParentExpr : llvm::reverse(ExprStack)) {
    // We're considered to be in a type context if either:
    // - We have a valid parent for a TypeExpr, or
    // - The parent "looks like" a type (and is not a call arg), and we can
    //   reach a valid parent for a TypeExpr if we continue walking.
    if (ParentExpr->isValidParentOfTypeExpr(E))
      return true;

    if (!exprLooksLikeAType(ParentExpr))
      return false;

    E = ParentExpr;
  }
  return false;
}

/// Only allow simplification of a DiscardAssignmentExpr if it hasn't already
/// been explicitly marked as correct, and the current AST state allows it.
bool PreCheckExpression::canSimplifyDiscardAssignmentExpr(
    DiscardAssignmentExpr *DAE) {
  return !CorrectDiscardAssignmentExprs.count(DAE) && SequenceExprDepth == 0 &&
         possiblyInTypeContext(DAE);
}


/// In Swift < 5, diagnose and correct invalid multi-argument or
/// argument-labeled interpolations. Returns \c true if the AST walk should
/// continue, or \c false if it should be aborted.
bool PreCheckExpression::correctInterpolationIfStrange(
    InterpolatedStringLiteralExpr *ISLE) {
  // These expressions are valid in Swift 5+.
  if (getASTContext().isSwiftVersionAtLeast(5))
    return true;

  /// Diagnoses appendInterpolation(...) calls with multiple
  /// arguments or argument labels and corrects them.
  class StrangeInterpolationRewriter : public ASTWalker {
    ASTContext &Context;

  public:
    StrangeInterpolationRewriter(ASTContext &Ctx) : Context(Ctx) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    virtual PreWalkAction walkToDeclPre(Decl *D) override {
      // We don't want to look inside decls.
      return Action::SkipChildren();
    }

    virtual PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // One InterpolatedStringLiteralExpr should never be nested inside
      // another except as a child of a CallExpr, and we don't recurse into
      // the children of CallExprs.
      assert(!isa<InterpolatedStringLiteralExpr>(E) &&
             "StrangeInterpolationRewriter found nested interpolation?");

      // We only care about CallExprs.
      if (!isa<CallExpr>(E))
        return Action::Continue(E);

      auto *call = cast<CallExpr>(E);
      auto *args = call->getArgs();

      auto lParen = args->getLParenLoc();
      auto rParen = args->getRParenLoc();

      if (auto callee = dyn_cast<UnresolvedDotExpr>(call->getFn())) {
        if (callee->getName().getBaseName() ==
            Context.Id_appendInterpolation) {

          Optional<Argument> newArg;
          if (args->size() > 1) {
            auto *secondArg = args->get(1).getExpr();
            Context.Diags
                .diagnose(secondArg->getLoc(),
                          diag::string_interpolation_list_changing)
                .highlightChars(secondArg->getLoc(), rParen);
            Context.Diags
                .diagnose(secondArg->getLoc(),
                          diag::string_interpolation_list_insert_parens)
                .fixItInsertAfter(lParen, "(")
                .fixItInsert(rParen, ")");

            // Make sure we don't have an inout arg somewhere, as that's
            // invalid even with the compatibility fix.
            for (auto arg : *args) {
              if (arg.isInOut()) {
                Context.Diags.diagnose(arg.getExpr()->getStartLoc(),
                                       diag::extraneous_address_of);
                return Action::Stop();
              }
            }

            // Form a new argument tuple from the argument list.
            auto *packed = args->packIntoImplicitTupleOrParen(Context);
            newArg = Argument::unlabeled(packed);
          } else if (args->size() == 1 &&
                     args->front().getLabel() != Identifier()) {
            // Form a new argument that drops the label.
            auto *argExpr = args->front().getExpr();
            newArg = Argument::unlabeled(argExpr);

            SourceLoc argLabelLoc = args->front().getLabelLoc(),
                      argLoc = argExpr->getStartLoc();

            Context.Diags
                .diagnose(argLabelLoc,
                          diag::string_interpolation_label_changing)
                .highlightChars(argLabelLoc, argLoc);
            Context.Diags
                .diagnose(argLabelLoc,
                          diag::string_interpolation_remove_label,
                          args->front().getLabel())
                .fixItRemoveChars(argLabelLoc, argLoc);
          }

          // If newArg is no longer null, we need to build a new
          // appendInterpolation(_:) call that takes it to replace the bad
          // appendInterpolation(...) call.
          if (newArg) {
            auto newCallee = new (Context) UnresolvedDotExpr(
                callee->getBase(), /*dotloc=*/SourceLoc(),
                DeclNameRef(Context.Id_appendInterpolation),
                /*nameloc=*/DeclNameLoc(), /*Implicit=*/true);

            auto *newArgList =
                ArgumentList::create(Context, lParen, {*newArg}, rParen,
                                     /*trailingClosureIdx*/ None,
                                     /*implicit*/ false);
            E = CallExpr::create(Context, newCallee, newArgList,
                                 /*implicit=*/false);
          }
        }
      }

      // There is never a CallExpr between an InterpolatedStringLiteralExpr
      // and an un-typechecked appendInterpolation(...) call, so whether we
      // changed E or not, we don't need to recurse any deeper.
      return Action::SkipChildren(E);
    }
  };

  return ISLE->getAppendingExpr()->walk(
      StrangeInterpolationRewriter(getASTContext()));
}

/// Scout out the specified destination of an AssignExpr to recursively
/// identify DiscardAssignmentExpr in legal places.  We can only allow them
/// in simple pattern-like expressions, so we reject anything complex here.
void PreCheckExpression::markAcceptableDiscardExprs(Expr *E) {
  if (!E) return;

  if (auto *PE = dyn_cast<ParenExpr>(E))
    return markAcceptableDiscardExprs(PE->getSubExpr());
  if (auto *TE = dyn_cast<TupleExpr>(E)) {
    for (auto &elt : TE->getElements())
      markAcceptableDiscardExprs(elt);
    return;
  }
  if (auto *BOE = dyn_cast<BindOptionalExpr>(E))
    return markAcceptableDiscardExprs(BOE->getSubExpr());
  if (auto *DAE = dyn_cast<DiscardAssignmentExpr>(E))
    CorrectDiscardAssignmentExprs.insert(DAE);

  // Otherwise, we can't support this.
}

VarDecl *PreCheckExpression::getImplicitSelfDeclForSuperContext(SourceLoc Loc) {
  auto *methodContext = DC->getInnermostMethodContext();
  if (!methodContext) {
    Ctx.Diags.diagnose(Loc, diag::super_not_in_class_method);
    return nullptr;
  }

  // Do an actual lookup for 'self' in case it shows up in a capture list.
  auto *methodSelf = methodContext->getImplicitSelfDecl();
  auto *lookupSelf = ASTScope::lookupSingleLocalDecl(DC->getParentSourceFile(),
                                                     Ctx.Id_self, Loc);
  if (lookupSelf && lookupSelf != methodSelf) {
    // FIXME: This is the wrong diagnostic for if someone manually declares a
    // variable named 'self' using backticks.
    Ctx.Diags.diagnose(Loc, diag::super_in_closure_with_capture);
    Ctx.Diags.diagnose(lookupSelf->getLoc(),
                       diag::super_in_closure_with_capture_here);
    return nullptr;
  }

  return methodSelf;
}

/// Simplify expressions which are type sugar productions that got parsed
/// as expressions due to the parser not knowing which identifiers are
/// type names.
TypeExpr *PreCheckExpression::simplifyTypeExpr(Expr *E) {
  // Fold member types.
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(E)) {
    return simplifyNestedTypeExpr(UDE);
  }

  // Fold '_' into a placeholder type, if we're allowed.
  if (auto *DAE = dyn_cast<DiscardAssignmentExpr>(E)) {
    if (canSimplifyDiscardAssignmentExpr(DAE)) {
      auto *placeholderRepr =
          new (Ctx) PlaceholderTypeRepr(DAE->getLoc());
      return new (Ctx) TypeExpr(placeholderRepr);
    }
  }

  // Fold T? into an optional type when T is a TypeExpr.
  if (isa<OptionalEvaluationExpr>(E) || isa<BindOptionalExpr>(E)) {
    TypeExpr *TyExpr;
    SourceLoc QuestionLoc;
    if (auto *OOE = dyn_cast<OptionalEvaluationExpr>(E)) {
      TyExpr = dyn_cast<TypeExpr>(OOE->getSubExpr());
      QuestionLoc = OOE->getLoc();
    } else {
      TyExpr = dyn_cast<TypeExpr>(cast<BindOptionalExpr>(E)->getSubExpr());
      QuestionLoc = cast<BindOptionalExpr>(E)->getQuestionLoc();
    }
    if (!TyExpr) return nullptr;

    auto *InnerTypeRepr = TyExpr->getTypeRepr();
    assert(!TyExpr->isImplicit() && InnerTypeRepr &&
           "This doesn't work on implicit TypeExpr's, "
           "the TypeExpr should have been built correctly in the first place");
    
    // The optional evaluation is passed through.
    if (isa<OptionalEvaluationExpr>(E))
      return TyExpr;

    auto *NewTypeRepr =
        new (Ctx) OptionalTypeRepr(InnerTypeRepr, QuestionLoc);
    return new (Ctx) TypeExpr(NewTypeRepr);
  }

  // Fold T! into an IUO type when T is a TypeExpr.
  if (auto *FVE = dyn_cast<ForceValueExpr>(E)) {
    auto *TyExpr = dyn_cast<TypeExpr>(FVE->getSubExpr());
    if (!TyExpr) return nullptr;

    auto *InnerTypeRepr = TyExpr->getTypeRepr();
    assert(!TyExpr->isImplicit() && InnerTypeRepr &&
           "This doesn't work on implicit TypeExpr's, "
           "the TypeExpr should have been built correctly in the first place");

    auto *NewTypeRepr = new (Ctx)
        ImplicitlyUnwrappedOptionalTypeRepr(InnerTypeRepr,
                                            FVE->getExclaimLoc());
    return new (Ctx) TypeExpr(NewTypeRepr);
  }

  // Fold (T) into a type T with parens around it.
  if (auto *PE = dyn_cast<ParenExpr>(E)) {
    auto *TyExpr = dyn_cast<TypeExpr>(PE->getSubExpr());
    if (!TyExpr) return nullptr;
    
    TupleTypeReprElement InnerTypeRepr[] = { TyExpr->getTypeRepr() };
    assert(!TyExpr->isImplicit() && InnerTypeRepr[0].Type &&
           "SubscriptExpr doesn't work on implicit TypeExpr's, "
           "the TypeExpr should have been built correctly in the first place");

    auto *NewTypeRepr = TupleTypeRepr::create(Ctx, InnerTypeRepr,
                                              PE->getSourceRange());
    return new (Ctx) TypeExpr(NewTypeRepr);
  }
  
  // Fold a tuple expr like (T1,T2) into a tuple type (T1,T2).
  if (auto *TE = dyn_cast<TupleExpr>(E)) {
    // FIXME: Decide what to do about ().  It could be a type or an expr.
    if (TE->getNumElements() == 0)
      return nullptr;

    SmallVector<TupleTypeReprElement, 4> Elts;
    unsigned EltNo = 0;
    for (auto Elt : TE->getElements()) {
      // Try to simplify the element, e.g. to fold PackExpansionExprs
      // into TypeExprs.
      if (auto simplified = simplifyTypeExpr(Elt))
        Elt = simplified;

      auto *eltTE = dyn_cast<TypeExpr>(Elt);
      if (!eltTE) return nullptr;
      TupleTypeReprElement elt;
      assert(eltTE->getTypeRepr() && !eltTE->isImplicit() &&
             "This doesn't work on implicit TypeExpr's, the "
             "TypeExpr should have been built correctly in the first place");

      // If the tuple element has a label, propagate it.
      elt.Type = eltTE->getTypeRepr();
      elt.Name = TE->getElementName(EltNo);
      elt.NameLoc = TE->getElementNameLoc(EltNo);

      Elts.push_back(elt);
      ++EltNo;
    }
    auto *NewTypeRepr = TupleTypeRepr::create(
        Ctx, Elts, TE->getSourceRange());
    return new (Ctx) TypeExpr(NewTypeRepr);
  }
  

  // Fold [T] into an array type.
  if (auto *AE = dyn_cast<ArrayExpr>(E)) {
    if (AE->getElements().size() != 1)
      return nullptr;

    auto *TyExpr = dyn_cast<TypeExpr>(AE->getElement(0));
    if (!TyExpr)
      return nullptr;

    auto *NewTypeRepr = new (Ctx)
        ArrayTypeRepr(TyExpr->getTypeRepr(),
                      SourceRange(AE->getLBracketLoc(), AE->getRBracketLoc()));
    return new (Ctx) TypeExpr(NewTypeRepr);
  }

  // Fold [K : V] into a dictionary type.
  if (auto *DE = dyn_cast<DictionaryExpr>(E)) {
    if (DE->getElements().size() != 1)
      return nullptr;

    TypeRepr *keyTypeRepr, *valueTypeRepr;
    
    if (auto EltTuple = dyn_cast<TupleExpr>(DE->getElement(0))) {
      auto *KeyTyExpr = dyn_cast<TypeExpr>(EltTuple->getElement(0));
      if (!KeyTyExpr)
        return nullptr;

      auto *ValueTyExpr = dyn_cast<TypeExpr>(EltTuple->getElement(1));
      if (!ValueTyExpr)
        return nullptr;
     
      keyTypeRepr = KeyTyExpr->getTypeRepr();
      valueTypeRepr = ValueTyExpr->getTypeRepr();
    } else {
      auto *TE = dyn_cast<TypeExpr>(DE->getElement(0));
      if (!TE) return nullptr;
      
      auto *TRE = dyn_cast_or_null<TupleTypeRepr>(TE->getTypeRepr());
      while (TRE->isParenType()) {
        TRE = dyn_cast_or_null<TupleTypeRepr>(TRE->getElementType(0));
      }

      assert(TRE->getElements().size() == 2);
      keyTypeRepr = TRE->getElementType(0);
      valueTypeRepr = TRE->getElementType(1);
    }

    auto *NewTypeRepr = new (Ctx) DictionaryTypeRepr(
        keyTypeRepr, valueTypeRepr,
        /*FIXME:colonLoc=*/SourceLoc(),
        SourceRange(DE->getLBracketLoc(), DE->getRBracketLoc()));
    return new (Ctx) TypeExpr(NewTypeRepr);
  }

  // Reinterpret arrow expr T1 -> T2 as function type.
  // FIXME: support 'inout', etc.
  if (auto *AE = dyn_cast<ArrowExpr>(E)) {
    if (!AE->isFolded()) return nullptr;

    auto diagnoseMissingParens = [](ASTContext &ctx, TypeRepr *tyR) {
      bool isVoid = false;
      if (const auto Void = dyn_cast<SimpleIdentTypeRepr>(tyR)) {
        if (Void->getNameRef().isSimpleName(ctx.Id_Void)) {
          isVoid = true;
        }
      }

      if (isVoid) {
        ctx.Diags.diagnose(tyR->getStartLoc(), diag::function_type_no_parens)
            .fixItReplace(tyR->getStartLoc(), "()");
      } else {
        ctx.Diags.diagnose(tyR->getStartLoc(), diag::function_type_no_parens)
            .highlight(tyR->getSourceRange())
            .fixItInsert(tyR->getStartLoc(), "(")
            .fixItInsertAfter(tyR->getEndLoc(), ")");
      }
    };

    auto extractInputTypeRepr = [&](Expr *E) -> TupleTypeRepr * {
      if (!E)
        return nullptr;
      if (auto *TyE = dyn_cast<TypeExpr>(E)) {
        auto ArgRepr = TyE->getTypeRepr();
        if (auto *TTyRepr = dyn_cast<TupleTypeRepr>(ArgRepr))
          return TTyRepr;
        diagnoseMissingParens(Ctx, ArgRepr);
        return TupleTypeRepr::create(Ctx, {ArgRepr}, ArgRepr->getSourceRange());
      }
      if (auto *TE = dyn_cast<TupleExpr>(E))
        if (TE->getNumElements() == 0)
          return TupleTypeRepr::createEmpty(Ctx, TE->getSourceRange());

      // When simplifying a type expr like "(P1 & P2) -> (P3 & P4) -> Int",
      // it may have been folded at the same time; recursively simplify it.
      if (auto ArgsTypeExpr = simplifyTypeExpr(E)) {
        auto ArgRepr = ArgsTypeExpr->getTypeRepr();
        if (auto *TTyRepr = dyn_cast<TupleTypeRepr>(ArgRepr))
          return TTyRepr;
        diagnoseMissingParens(Ctx, ArgRepr);
        return TupleTypeRepr::create(Ctx, {ArgRepr}, ArgRepr->getSourceRange());
      }
      return nullptr;
    };

    auto extractTypeRepr = [&](Expr *E) -> TypeRepr * {
      if (!E)
        return nullptr;
      if (auto *TyE = dyn_cast<TypeExpr>(E))
        return TyE->getTypeRepr();
      if (auto *TE = dyn_cast<TupleExpr>(E))
        if (TE->getNumElements() == 0)
          return TupleTypeRepr::createEmpty(Ctx, TE->getSourceRange());

      // When simplifying a type expr like "P1 & P2 -> P3 & P4 -> Int",
      // it may have been folded at the same time; recursively simplify it.
      if (auto ArgsTypeExpr = simplifyTypeExpr(E))
        return ArgsTypeExpr->getTypeRepr();
      return nullptr;
    };

    TupleTypeRepr *ArgsTypeRepr = extractInputTypeRepr(AE->getArgsExpr());
    if (!ArgsTypeRepr) {
      Ctx.Diags.diagnose(AE->getArgsExpr()->getLoc(),
                         diag::expected_type_before_arrow);
      auto ArgRange = AE->getArgsExpr()->getSourceRange();
      auto ErrRepr = new (Ctx) ErrorTypeRepr(ArgRange);
      ArgsTypeRepr =
          TupleTypeRepr::create(Ctx, {ErrRepr}, ArgRange);
    }

    TypeRepr *ResultTypeRepr = extractTypeRepr(AE->getResultExpr());
    if (!ResultTypeRepr) {
      Ctx.Diags.diagnose(AE->getResultExpr()->getLoc(),
                         diag::expected_type_after_arrow);
      ResultTypeRepr = new (Ctx)
          ErrorTypeRepr(AE->getResultExpr()->getSourceRange());
    }

    auto NewTypeRepr = new (Ctx)
        FunctionTypeRepr(nullptr, ArgsTypeRepr, AE->getAsyncLoc(),
                         AE->getThrowsLoc(), AE->getArrowLoc(), ResultTypeRepr);
    return new (Ctx) TypeExpr(NewTypeRepr);
  }
  
  // Fold 'P & Q' into a composition type
  if (auto *binaryExpr = getCompositionExpr(E)) {
    // The protocols we are composing
    SmallVector<TypeRepr *, 4> Types;

    auto lhsExpr = binaryExpr->getLHS();
    if (auto *lhs = dyn_cast<TypeExpr>(lhsExpr)) {
      Types.push_back(lhs->getTypeRepr());
    } else if (isa<BinaryExpr>(lhsExpr)) {
      // If the lhs is another binary expression, we have a multi element
      // composition: 'A & B & C' is parsed as ((A & B) & C); we get
      // the protocols from the lhs here
      if (auto expr = simplifyTypeExpr(lhsExpr))
        if (auto *repr = dyn_cast<CompositionTypeRepr>(expr->getTypeRepr()))
          // add the protocols to our list
          for (auto proto : repr->getTypes())
            Types.push_back(proto);
        else
          return nullptr;
      else
        return nullptr;
    } else
      return nullptr;

    // Add the rhs which is just a TypeExpr
    auto *rhs = dyn_cast<TypeExpr>(binaryExpr->getRHS());
    if (!rhs) return nullptr;
    Types.push_back(rhs->getTypeRepr());

    auto CompRepr = CompositionTypeRepr::create(Ctx, Types,
                                                lhsExpr->getStartLoc(),
                                                binaryExpr->getSourceRange());
    return new (Ctx) TypeExpr(CompRepr);
  }

  // Fold a pack expansion expr into a TypeExpr when the pattern is a TypeExpr.
  if (auto *expansion = dyn_cast<PackExpansionExpr>(E)) {
    if (auto *pattern = dyn_cast<TypeExpr>(expansion->getPatternExpr())) {
      auto *repr = new (Ctx) PackExpansionTypeRepr(expansion->getStartLoc(),
                                                   pattern->getTypeRepr());
      return new (Ctx) TypeExpr(repr);
    }
  }

  // Fold a PackElementExpr into a TypeExpr when the element is a TypeExpr
  if (auto *element = dyn_cast<PackElementExpr>(E)) {
    if (auto *refExpr = dyn_cast<TypeExpr>(element->getPackRefExpr())) {
      auto *repr = new (Ctx) PackElementTypeRepr(element->getStartLoc(),
                                                 refExpr->getTypeRepr());
      return new (Ctx) TypeExpr(repr);
    }
  }

  return nullptr;
}

void PreCheckExpression::resolveKeyPathExpr(KeyPathExpr *KPE) {
  if (KPE->isObjC())
    return;
  
  if (!KPE->getComponents().empty())
    return;

  TypeRepr *rootType = nullptr;
  SmallVector<KeyPathExpr::Component, 4> components;
  auto &DE = getASTContext().Diags;

  // Pre-order visit of a sequence foo.bar[0]?.baz, which means that the
  // components are pushed in reverse order.
  auto traversePath = [&](Expr *expr, bool isInParsedPath,
                          bool emitErrors = true) {
    Expr *outermostExpr = expr;
    // We can end up in scenarios where the key path has contextual type,
    // but is missing a leading dot. This can happen when we have an
    // implicit TypeExpr or an implicit DeclRefExpr.
    auto diagnoseMissingDot = [&]() {
      DE.diagnose(expr->getLoc(),
                  diag::expr_swift_keypath_not_starting_with_dot)
          .fixItInsert(expr->getStartLoc(), ".");
    };
    while (1) {
      // Base cases: we've reached the top.
      if (auto TE = dyn_cast<TypeExpr>(expr)) {
        assert(!isInParsedPath);
        rootType = TE->getTypeRepr();
        if (TE->isImplicit() && !KPE->expectsContextualRoot())
          diagnoseMissingDot();
        return;
      } else if (isa<KeyPathDotExpr>(expr)) {
        assert(isInParsedPath);
        // Nothing here: the type is either the root, or is inferred.
        return;
      } else if (!KPE->expectsContextualRoot() && expr->isImplicit() &&
                 isa<DeclRefExpr>(expr)) {
        assert(!isInParsedPath);
        diagnoseMissingDot();
        return;
      }

      // Recurring cases:
      if (auto SE = dyn_cast<DotSelfExpr>(expr)) {
        // .self, the identity component.
        components.push_back(KeyPathExpr::Component::forIdentity(
          SE->getSelfLoc()));
        expr = SE->getSubExpr();
      } else if (auto UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
        // .foo
        components.push_back(KeyPathExpr::Component::forUnresolvedProperty(
            UDE->getName(), UDE->getLoc()));

        expr = UDE->getBase();
      } else if (auto CCE = dyn_cast<CodeCompletionExpr>(expr)) {
        components.push_back(
            KeyPathExpr::Component::forCodeCompletion(CCE->getLoc()));

        expr = CCE->getBase();
        if (!expr) {
          // We are completing on the key path's base. Stop iterating.
          return;
        }
      } else if (auto SE = dyn_cast<SubscriptExpr>(expr)) {
        // .[0] or just plain [0]
        components.push_back(KeyPathExpr::Component::forUnresolvedSubscript(
            getASTContext(), SE->getArgs()));

        expr = SE->getBase();
      } else if (auto BOE = dyn_cast<BindOptionalExpr>(expr)) {
        // .? or ?
        components.push_back(KeyPathExpr::Component::forUnresolvedOptionalChain(
            BOE->getQuestionLoc()));

        expr = BOE->getSubExpr();
      } else if (auto FVE = dyn_cast<ForceValueExpr>(expr)) {
        // .! or !
        components.push_back(KeyPathExpr::Component::forUnresolvedOptionalForce(
            FVE->getExclaimLoc()));

        expr = FVE->getSubExpr();
      } else if (auto OEE = dyn_cast<OptionalEvaluationExpr>(expr)) {
        // Do nothing: this is implied to exist as the last expression, by the
        // BindOptionalExprs, but is irrelevant to the components.
        (void)outermostExpr;
        assert(OEE == outermostExpr);
        expr = OEE->getSubExpr();
      } else {
        if (emitErrors) {
          // \(<expr>) may be an attempt to write a string interpolation outside
          // of a string literal; diagnose this case specially.
          if (isa<ParenExpr>(expr) || isa<TupleExpr>(expr)) {
            DE.diagnose(expr->getLoc(),
                        diag::expr_string_interpolation_outside_string);
          } else {
            DE.diagnose(expr->getLoc(),
                        diag::expr_swift_keypath_invalid_component);
          }
        }
        components.push_back(KeyPathExpr::Component());
        return;
      }
    }
  };

  auto root = KPE->getParsedRoot();
  auto path = KPE->getParsedPath();

  if (path) {
    traversePath(path, /*isInParsedPath=*/true);

    // This path looks like \Foo.Bar.[0].baz, which means Foo.Bar has to be a
    // type.
    if (root) {
      if (auto TE = dyn_cast<TypeExpr>(root)) {
        rootType = TE->getTypeRepr();
      } else {
        // FIXME: Probably better to catch this case earlier and force-eval as
        // TypeExpr.
        DE.diagnose(root->getLoc(),
                    diag::expr_swift_keypath_not_starting_with_type);

        // Traverse this path for recovery purposes: it may be a typo like
        // \Foo.property.[0].
        traversePath(root, /*isInParsedPath=*/false,
                     /*emitErrors=*/false);
      }
    }
  } else {
    traversePath(root, /*isInParsedPath=*/false);
  }

  // Key paths must be spelled with at least one component.
  if (components.empty()) {
    // Passes further down the pipeline expect keypaths to always have at least
    // one component, so stuff an invalid component in the AST for recovery.
    components.push_back(KeyPathExpr::Component());
  }

  std::reverse(components.begin(), components.end());

  KPE->setRootType(rootType);
  KPE->setComponents(getASTContext(), components);
}

Expr *PreCheckExpression::simplifyTypeConstructionWithLiteralArg(Expr *E) {
  // If constructor call is expected to produce an optional let's not attempt
  // this optimization because literal initializers aren't failable.
  if (!getASTContext().LangOpts.isSwiftVersionAtLeast(5)) {
    if (!ExprStack.empty()) {
      auto *parent = ExprStack.back();
      if (isa<BindOptionalExpr>(parent) || isa<ForceValueExpr>(parent))
        return nullptr;
    }
  }

  auto *call = dyn_cast<CallExpr>(E);
  if (!call)
    return nullptr;

  auto *typeExpr = dyn_cast<TypeExpr>(call->getFn());
  if (!typeExpr)
    return nullptr;

  auto *unaryArg = call->getArgs()->getUnlabeledUnaryExpr();
  if (!unaryArg)
    return nullptr;

  auto *literal = dyn_cast<LiteralExpr>(unaryArg->getSemanticsProvidingExpr());
  if (!literal)
    return nullptr;

  auto *protocol = TypeChecker::getLiteralProtocol(getASTContext(), literal);
  if (!protocol)
    return nullptr;

  Type castTy;
  if (auto precheckedTy = typeExpr->getInstanceType()) {
    castTy = precheckedTy;
  } else {
    const auto result = TypeResolution::resolveContextualType(
        typeExpr->getTypeRepr(), DC, TypeResolverContext::InExpression,
        [](auto unboundTy) {
          // FIXME: Don't let unbound generic types escape type resolution.
          // For now, just return the unbound generic type.
          return unboundTy;
        },
        // FIXME: Don't let placeholder types escape type resolution.
        // For now, just return the placeholder type.
        PlaceholderType::get,
        // Pack elements for CoerceExprs are opened in CSGen.
        /*packElementOpener*/ nullptr);

    if (result->hasError())
      return new (getASTContext())
          ErrorExpr(typeExpr->getSourceRange(), result, typeExpr);

    castTy = result;
  }

  if (!castTy->getAnyNominal())
    return nullptr;

  // Don't bother to convert deprecated selector syntax.
  if (auto selectorTy = getASTContext().getSelectorType()) {
    if (castTy->isEqual(selectorTy))
      return nullptr;
  }

  return DC->getParentModule()->lookupConformance(castTy, protocol)
             ? CoerceExpr::forLiteralInit(getASTContext(), literal,
                                          call->getSourceRange(),
                                          typeExpr->getTypeRepr())
             : nullptr;
}

bool ConstraintSystem::preCheckTarget(SyntacticElementTarget &target,
                                      bool replaceInvalidRefsWithErrors,
                                      bool leaveClosureBodiesUnchecked) {
  auto *DC = target.getDeclContext();

  bool hadErrors = false;

  if (auto *expr = target.getAsExpr()) {
    hadErrors |= preCheckExpression(expr, DC, replaceInvalidRefsWithErrors,
                                    leaveClosureBodiesUnchecked);
    // Even if the pre-check fails, expression still has to be re-set.
    target.setExpr(expr);
  }

  if (target.isForEachStmt()) {
    auto *stmt = target.getAsForEachStmt();

    auto *sequenceExpr = stmt->getParsedSequence();
    auto *whereExpr = stmt->getWhere();

    hadErrors |= preCheckExpression(sequenceExpr, DC,
                                    /*replaceInvalidRefsWithErrors=*/true,
                                    /*leaveClosureBodiesUnchecked=*/false);

    if (whereExpr) {
      hadErrors |= preCheckExpression(whereExpr, DC,
                                      /*replaceInvalidRefsWithErrors=*/true,
                                      /*leaveClosureBodiesUnchecked=*/false);
    }

    // Update sequence and where expressions to pre-checked versions.
    if (!hadErrors) {
      stmt->setParsedSequence(sequenceExpr);

      if (whereExpr)
        stmt->setWhere(whereExpr);
    }
  }

  return hadErrors;
}

/// Pre-check the expression, validating any types that occur in the
/// expression and folding sequence expressions.
bool ConstraintSystem::preCheckExpression(Expr *&expr, DeclContext *dc,
                                          bool replaceInvalidRefsWithErrors,
                                          bool leaveClosureBodiesUnchecked) {
  auto &ctx = dc->getASTContext();
  FrontendStatsTracer StatsTracer(ctx.Stats, "precheck-expr", expr);

  PreCheckExpression preCheck(dc, expr,
                              replaceInvalidRefsWithErrors,
                              leaveClosureBodiesUnchecked);

  // Perform the pre-check.
  if (auto result = expr->walk(preCheck)) {
    expr = result;
    return false;
  }
  return true;
}

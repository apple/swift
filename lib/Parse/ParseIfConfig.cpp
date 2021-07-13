//===--- ParseIfConfig.cpp - Swift Language Parser for #if directives -----===//
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
//
// Conditional Compilation Block Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Version.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/SyntaxParsingContext.h"
#include "swift/Syntax/SyntaxFactory.h"
#include "swift/Syntax/TokenSyntax.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace swift::syntax;

namespace {

/// Get PlatformConditionKind from platform condition name.
static
Optional<PlatformConditionKind> getPlatformConditionKind(StringRef Name) {
  return llvm::StringSwitch<Optional<PlatformConditionKind>>(Name)
#define PLATFORM_CONDITION(LABEL, IDENTIFIER) \
    .Case(IDENTIFIER, PlatformConditionKind::LABEL)
#include "swift/AST/PlatformConditionKinds.def"
    .Default(None);
}

/// Get platform condition name from PlatformConditionKind.
static StringRef getPlatformConditionName(PlatformConditionKind Kind) {
  switch (Kind) {
#define PLATFORM_CONDITION(LABEL, IDENTIFIER) \
  case PlatformConditionKind::LABEL: return IDENTIFIER;
#include "swift/AST/PlatformConditionKinds.def"
  }
  llvm_unreachable("Unhandled PlatformConditionKind in switch");
}

/// Extract source text of the expression.
static StringRef extractExprSource(SourceManager &SM, Expr *E) {
  CharSourceRange Range =
    Lexer::getCharSourceRangeFromSourceRange(SM, E->getSourceRange());
  return SM.extractText(Range);
}

static bool isValidPrefixUnaryOperator(Optional<StringRef> UnaryOperator) {
  return UnaryOperator != None &&
         (UnaryOperator.getValue() == ">=" || UnaryOperator.getValue() == "<");
}

static bool isValidVersion(const version::Version &Version,
                           const version::Version &ExpectedVersion,
                           StringRef UnaryOperator) {
  if (UnaryOperator == ">=")
    return Version >= ExpectedVersion;
  if (UnaryOperator == "<")
    return Version < ExpectedVersion;
  llvm_unreachable("unsupported unary operator");
}

static llvm::VersionTuple getCanImportVersion(TupleExpr *te,
                                              DiagnosticEngine *D,
                                              bool &underlyingVersion) {
  llvm::VersionTuple result;
  if (te->getElements().size() != 2) {
    if (D) {
      D->diagnose(te->getLoc(), diag::canimport_two_parameters);
    }
    return result;
  }
  auto label = te->getElementName(1);
  auto subE = te->getElement(1);
  if (label.str() == "_version") {
    underlyingVersion = false;
  } else if (label.str() == "_underlyingVersion") {
    underlyingVersion = true;
  } else {
    if (D) {
      D->diagnose(subE->getLoc(), diag::canimport_label);
    }
    return result;
  }
  StringRef verText;
  if (auto *nle = dyn_cast<NumberLiteralExpr>(subE)) {
    verText = nle->getDigitsText();
  } else if (auto *sle= dyn_cast<StringLiteralExpr>(subE)) {
    verText = sle->getValue();
  }
  if (verText.empty())
    return result;
  if (result.tryParse(verText)) {
    if (D) {
      D->diagnose(subE->getLoc(), diag::canimport_version, verText);
    }
  }
  return result;
}

static Expr *getSingleSubExp(Expr *exp, StringRef kindName,
                             DiagnosticEngine *D) {
  if (auto *pe = dyn_cast<ParenExpr>(exp)) {
    return pe->getSubExpr();
  }
  if (kindName == "canImport") {
    if (auto *te = dyn_cast<TupleExpr>(exp)) {
      bool underlyingVersion;
      if (D) {
        // Diagnose canImport syntax
        (void)getCanImportVersion(te, D, underlyingVersion);
      }
      return te->getElement(0);
    }
  }
  return nullptr;
}

/// The condition validator.
class ValidateIfConfigCondition :
  public ExprVisitor<ValidateIfConfigCondition, Expr*> {
  ASTContext &Ctx;
  DiagnosticEngine &D;

  bool HasError;

  /// Get the identifier string of the UnresolvedDeclRefExpr.
  Optional<StringRef> getDeclRefStr(Expr *E, DeclRefKind Kind) {
    auto UDRE = dyn_cast<UnresolvedDeclRefExpr>(E);
    if (!UDRE ||
        !UDRE->hasName() ||
        UDRE->getRefKind() != Kind ||
        UDRE->getName().isCompoundName())
      return None;

    return UDRE->getName().getBaseIdentifier().str();
  }

  /// True for expressions represeting either top level modules
  /// or nested submodules.
  bool isModulePath(Expr *E) {
    auto UDE = dyn_cast<UnresolvedDotExpr>(E);
    if (!UDE)
      return getDeclRefStr(E, DeclRefKind::Ordinary).hasValue();

    return UDE->getFunctionRefKind() == FunctionRefKind::Unapplied &&
           isModulePath(UDE->getBase());
  }

  Expr *diagnoseUnsupportedExpr(Expr *E) {
    D.diagnose(E->getLoc(),
               diag::unsupported_conditional_compilation_expression_type);
    return nullptr;
  }

  // Support '||' and '&&' operator. The precedence of '&&' is higher than '||'.
  // Invalid operator and the next operand are diagnosed and removed from AST.
  Expr *foldSequence(Expr *LHS, ArrayRef<Expr*> &S, bool isRecurse = false) {
    assert(!S.empty() && ((S.size() & 1) == 0));

    auto getNextOperator = [&]() -> Optional<StringRef> {
      assert((S.size() & 1) == 0);
      while (!S.empty()) {
        auto Name = getDeclRefStr(S[0], DeclRefKind::BinaryOperator);
        if (Name.hasValue() && (*Name == "||" || *Name == "&&"))
          return Name;

        auto DiagID = isa<UnresolvedDeclRefExpr>(S[0])
          ? diag::unsupported_conditional_compilation_binary_expression
          : diag::unsupported_conditional_compilation_expression_type;
        D.diagnose(S[0]->getLoc(), DiagID);
        HasError |= true;
        // Consume invalid operator and the immediate RHS.
        S = S.slice(2);
      }
      return None;
    };

    // Extract out the first operator name.
    auto OpName = getNextOperator();
    if (!OpName.hasValue())
      // If failed, it's not a sequence anymore.
      return LHS;
    Expr *Op = S[0];

    // We will definitely be consuming at least one operator.
    // Pull out the prospective RHS and slice off the first two elements.
    Expr *RHS = validate(S[1]);
    S = S.slice(2);

    while (true) {
      // Pull out the next binary operator.
      auto NextOpName = getNextOperator();
      bool IsEnd = !NextOpName.hasValue();
      if (!IsEnd && *OpName == "||" && *NextOpName == "&&") {
        RHS = foldSequence(RHS, S, /*isRecurse*/true);
        continue;
      }

      // Apply the operator with left-associativity by folding the first two
      // operands.
      LHS = BinaryExpr::create(Ctx, LHS, Op, RHS, /*implicit*/ false);

      // If we don't have the next operator, we're done.
      if (IsEnd)
        break;
      if (isRecurse && *OpName == "&&" && *NextOpName == "||")
        break;

      OpName = NextOpName;
      Op = S[0];
      RHS = validate(S[1]);
      S = S.slice(2);
    }

    return LHS;
  }

public:
  ValidateIfConfigCondition(ASTContext &Ctx, DiagnosticEngine &D)
    : Ctx(Ctx), D(D), HasError(false) {}

  // Explicit configuration flag.
  Expr *visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    if (!getDeclRefStr(E, DeclRefKind::Ordinary).hasValue())
      return diagnoseUnsupportedExpr(E);
    return E;
  }

  // 'true' or 'false' constant.
  Expr *visitBooleanLiteralExpr(BooleanLiteralExpr *E) {
    return E;
  }

  // '0' and '1' are warned, but we accept it.
  Expr *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    if (E->isNegative() ||
        (E->getDigitsText() != "0" && E->getDigitsText() != "1")) {
      return diagnoseUnsupportedExpr(E);
    }
    // "#if 0" isn't valid, but it is common, so recognize it and handle it
    // with a fixit.
    StringRef replacement = E->getDigitsText() == "0" ? "false" :"true";
    D.diagnose(E->getLoc(), diag::unsupported_conditional_compilation_integer,
               E->getDigitsText(), replacement)
      .fixItReplace(E->getLoc(), replacement);
    return E;
  }

  // Platform conditions.
  Expr *visitCallExpr(CallExpr *E) {
    auto KindName = getDeclRefStr(E->getFn(), DeclRefKind::Ordinary);
    if (!KindName.hasValue()) {
      D.diagnose(E->getLoc(), diag::unsupported_platform_condition_expression);
      return nullptr;
    }

    Expr *Arg = getSingleSubExp(E->getArg(), *KindName, &D);
    if (!Arg) {
      D.diagnose(E->getLoc(), diag::platform_condition_expected_one_argument);
      return nullptr;
    }
    // '_compiler_version' '(' string-literal ')'
    if (*KindName == "_compiler_version") {
      auto SLE = dyn_cast<StringLiteralExpr>(Arg);
      if (!SLE) {
        D.diagnose(Arg->getLoc(),
                   diag::unsupported_platform_condition_argument,
                   "string literal");
        return nullptr;
      }

      auto ValStr = SLE->getValue();
      if (ValStr.empty()) {
        D.diagnose(SLE->getLoc(), diag::empty_version_string);
        return nullptr;
      }

      auto Val = version::Version::parseCompilerVersionString(
          SLE->getValue(), SLE->getLoc(), &D);
      if (!Val.hasValue())
        return nullptr;
      return E;
    }

    // 'swift' '(' ('>=' | '<') float-literal ( '.' integer-literal )* ')'
    // 'compiler' '(' ('>=' | '<') float-literal ( '.' integer-literal )* ')'
    if (*KindName == "swift" || *KindName == "compiler") {
      auto PUE = dyn_cast<PrefixUnaryExpr>(Arg);
      Optional<StringRef> PrefixName =
          PUE ? getDeclRefStr(PUE->getFn(), DeclRefKind::PrefixOperator) : None;
      if (!isValidPrefixUnaryOperator(PrefixName)) {
        D.diagnose(
            Arg->getLoc(), diag::unsupported_platform_condition_argument,
            "a unary comparison '>=' or '<'; for example, '>=2.2' or '<2.2'");
        return nullptr;
      }
      auto versionString = extractExprSource(Ctx.SourceMgr, PUE->getArg());
      auto Val = version::Version::parseVersionString(
          versionString, PUE->getArg()->getStartLoc(), &D);
      if (!Val.hasValue())
        return nullptr;
      return E;
    }

    if (*KindName == "canImport") {
      if (!isModulePath(Arg)) {
        D.diagnose(E->getLoc(), diag::unsupported_platform_condition_argument,
                   "module name");
        return nullptr;
      }
      return E;
    }

    // ( 'os' | 'arch' | '_endian' | '_runtime' ) '(' identifier ')''
    auto Kind = getPlatformConditionKind(*KindName);
    if (!Kind.hasValue()) {
      D.diagnose(E->getLoc(), diag::unsupported_platform_condition_expression);
      return nullptr;
    }

    auto ArgStr = getDeclRefStr(Arg, DeclRefKind::Ordinary);
    if (!ArgStr.hasValue()) {
      D.diagnose(E->getLoc(), diag::unsupported_platform_condition_argument,
                 "identifier");
      return nullptr;
    }

    PlatformConditionKind suggestedKind = *Kind;
    std::vector<StringRef> suggestedValues;
    if (!LangOptions::checkPlatformConditionSupported(*Kind, *ArgStr,
                                                      suggestedKind, suggestedValues)) {
      if (Kind == PlatformConditionKind::Runtime) {
        // Error for _runtime()
        D.diagnose(Arg->getLoc(),
                   diag::unsupported_platform_runtime_condition_argument);
        return nullptr;
      }

      // Just a warning for other unsupported arguments.
      StringRef DiagName;
      switch (*Kind) {
      case PlatformConditionKind::OS:
        DiagName = "operating system"; break;
      case PlatformConditionKind::Arch:
        DiagName = "architecture"; break;
      case PlatformConditionKind::Endianness:
        DiagName = "endianness"; break;
      case PlatformConditionKind::CanImport:
        DiagName = "import conditional"; break;
      case PlatformConditionKind::TargetEnvironment:
        DiagName = "target environment"; break;
      case PlatformConditionKind::PtrAuth:
        DiagName = "pointer authentication scheme"; break;
      case PlatformConditionKind::Runtime:
        llvm_unreachable("handled above");
      }
      auto Loc = Arg->getLoc();
      D.diagnose(Loc, diag::unknown_platform_condition_argument,
                 DiagName, *KindName);
      if (suggestedKind != *Kind) {
        auto suggestedKindName = getPlatformConditionName(suggestedKind);
        D.diagnose(Loc, diag::note_typo_candidate, suggestedKindName)
          .fixItReplace(E->getFn()->getSourceRange(), suggestedKindName);
      }
      for (auto suggestion : suggestedValues)
        D.diagnose(Loc, diag::note_typo_candidate, suggestion)
          .fixItReplace(Arg->getSourceRange(), suggestion);
    }
    else if (!suggestedValues.empty()) {
      // The value the user gave has been replaced by something newer.
      assert(suggestedValues.size() == 1 && "only support one replacement");
      auto replacement = suggestedValues.front();

      auto Loc = Arg->getLoc();
      D.diagnose(Loc, diag::renamed_platform_condition_argument,
                 *ArgStr, replacement)
        .fixItReplace(Arg->getSourceRange(), replacement);
    }

    return E;
  }

  // Grouped condition. e.g. '(FLAG)'
  Expr *visitParenExpr(ParenExpr *E) {
    E->setSubExpr(validate(E->getSubExpr()));
    return E;
  }

  // Prefix '!'. Other prefix operators are rejected.
  Expr *visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    auto OpName = getDeclRefStr(E->getFn(), DeclRefKind::PrefixOperator);
    if (!OpName.hasValue() || *OpName != "!") {
      D.diagnose(E->getLoc(),
                 diag::unsupported_conditional_compilation_unary_expression);
      return nullptr;
    }
    E->setArg(validate(E->getArg()));
    return E;
  }

  // Fold sequence expression for non-Swift3 mode.
  Expr *visitSequenceExpr(SequenceExpr *E) {
    ArrayRef<Expr*> Elts = E->getElements();
    Expr *foldedExpr = validate(Elts[0]);
    Elts = Elts.slice(1);
    foldedExpr = foldSequence(foldedExpr, Elts);
    assert(Elts.empty());
    return foldedExpr;
  }

  // Other expression types are unsupported.
  Expr *visitExpr(Expr *E) {
    return diagnoseUnsupportedExpr(E);
  }

  Expr *validate(Expr *E) {
    if (auto E2 = visit(E))
      return E2;
    HasError |= true;
    return E;
  }

  bool hasError() const {
    return HasError;
  }
};

/// Validate and modify the condition expression.
/// Returns \c true if the condition contains any error.
static bool validateIfConfigCondition(Expr *&condition,
                                      ASTContext &Context,
                                      DiagnosticEngine &D) {
  ValidateIfConfigCondition Validator(Context, D);
  condition = Validator.validate(condition);
  return Validator.hasError();
}

/// The condition evaluator.
/// The condition must be validated with validateIfConfigCondition().
class EvaluateIfConfigCondition :
  public ExprVisitor<EvaluateIfConfigCondition, bool> {
  ASTContext &Ctx;

  /// Get the identifier string from an \c Expr assuming it's an
  /// \c UnresolvedDeclRefExpr.
  StringRef getDeclRefStr(Expr *E) {
    return cast<UnresolvedDeclRefExpr>(E)->getName().getBaseIdentifier().str();
  }

public:
  EvaluateIfConfigCondition(ASTContext &Ctx) : Ctx(Ctx) {}

  bool visitBooleanLiteralExpr(BooleanLiteralExpr *E) {
    return E->getValue();
  }

  bool visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    return E->getDigitsText() != "0";
  }

  bool visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    auto Name = getDeclRefStr(E);

    // Check whether this is any one of the known compiler features.
    const auto &langOpts = Ctx.LangOpts;
    bool isKnownFeature = llvm::StringSwitch<bool>(Name)
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description, Option) \
        .Case("$" #FeatureName, Option)
#include "swift/Basic/Features.def"
        .Default(false);

    if (isKnownFeature)
      return true;
    
    return langOpts.isCustomConditionalCompilationFlagSet(Name);
  }

  bool visitCallExpr(CallExpr *E) {
    auto KindName = getDeclRefStr(E->getFn());
    auto *Arg = getSingleSubExp(E->getArg(), KindName, nullptr);
    if (KindName == "_compiler_version") {
      auto Str = cast<StringLiteralExpr>(Arg)->getValue();
      auto Val = version::Version::parseCompilerVersionString(
          Str, SourceLoc(), nullptr).getValue();
      auto thisVersion = version::Version::getCurrentCompilerVersion();
      return thisVersion >= Val;
    } else if ((KindName == "swift") || (KindName == "compiler")) {
      auto PUE = cast<PrefixUnaryExpr>(Arg);
      auto PrefixName = getDeclRefStr(PUE->getFn());
      auto Str = extractExprSource(Ctx.SourceMgr, PUE->getArg());
      auto Val = version::Version::parseVersionString(
          Str, SourceLoc(), nullptr).getValue();
      if (KindName == "swift") {
        return isValidVersion(Ctx.LangOpts.EffectiveLanguageVersion, Val,
                              PrefixName);
      } else if (KindName == "compiler") {
        auto currentLanguageVersion =
            version::Version::getCurrentLanguageVersion();
        return isValidVersion(currentLanguageVersion, Val, PrefixName);
      } else {
        llvm_unreachable("unsupported version conditional");
      }
    } else if (KindName == "canImport") {
      auto Str = extractExprSource(Ctx.SourceMgr, Arg);
      bool underlyingModule = false;
      llvm::VersionTuple version;
      if (auto *te = dyn_cast<TupleExpr>(E->getArg())) {
        version = getCanImportVersion(te, nullptr, underlyingModule);
      }
      ImportPath::Module::Builder builder(Ctx, Str, /*separator=*/'.',
                                          Arg->getStartLoc());
      return Ctx.canImportModule(builder.get(), version, underlyingModule);
    }

    auto Val = getDeclRefStr(Arg);
    auto Kind = getPlatformConditionKind(KindName).getValue();
    return Ctx.LangOpts.checkPlatformCondition(Kind, Val);
  }

  bool visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    return !visit(E->getArg());
  }

  bool visitParenExpr(ParenExpr *E) {
    return visit(E->getSubExpr());
  }

  bool visitBinaryExpr(BinaryExpr *E) {
    auto OpName = getDeclRefStr(E->getFn());
    if (OpName == "||") return visit(E->getLHS()) || visit(E->getRHS());
    if (OpName == "&&") return visit(E->getLHS()) && visit(E->getRHS());
    llvm_unreachable("unsupported binary operator");
  }

  bool visitExpr(Expr *E) { llvm_unreachable("Unvalidated condition?"); }
};

/// Evaluate the condition.
/// \c true if success, \c false if failed.
static bool evaluateIfConfigCondition(Expr *Condition, ASTContext &Context) {
  return EvaluateIfConfigCondition(Context).visit(Condition);
}

/// Version condition checker.
class IsVersionIfConfigCondition :
  public ExprVisitor<IsVersionIfConfigCondition, bool> {

  /// Get the identifier string from an \c Expr assuming it's an
  /// \c UnresolvedDeclRefExpr.
  StringRef getDeclRefStr(Expr *E) {
    return cast<UnresolvedDeclRefExpr>(E)->getName().getBaseIdentifier().str();
  }

public:
  IsVersionIfConfigCondition() {}

  bool visitBinaryExpr(BinaryExpr *E) {
    auto OpName = getDeclRefStr(E->getFn());
    if (OpName == "||") return visit(E->getLHS()) && visit(E->getRHS());
    if (OpName == "&&") return visit(E->getLHS()) || visit(E->getRHS());
    llvm_unreachable("unsupported binary operator");
  }

  bool visitCallExpr(CallExpr *E) {
    auto KindName = getDeclRefStr(E->getFn());
    return KindName == "_compiler_version" || KindName == "swift" ||
        KindName == "compiler";
  }

  bool visitPrefixUnaryExpr(PrefixUnaryExpr *E) { return visit(E->getArg()); }
  bool visitParenExpr(ParenExpr *E) { return visit(E->getSubExpr()); }
  bool visitExpr(Expr *E) { return false; }
};

/// Returns \c true if the condition is a version check.
static bool isVersionIfConfigCondition(Expr *Condition) {
  return IsVersionIfConfigCondition().visit(Condition);
}

/// Get the identifier string from an \c Expr if it's an
/// \c UnresolvedDeclRefExpr, otherwise the empty string.
static StringRef getDeclRefStr(Expr *E) {
  if (auto *UDRE = dyn_cast<UnresolvedDeclRefExpr>(E)) {
    return UDRE->getName().getBaseIdentifier().str();
  }
  return "";
}

static bool isPlatformConditionDisjunction(Expr *E, PlatformConditionKind Kind,
                                           ArrayRef<StringRef> Vals) {
  if (auto *Or = dyn_cast<BinaryExpr>(E)) {
    if (getDeclRefStr(Or->getFn()) == "||") {
      return (isPlatformConditionDisjunction(Or->getLHS(), Kind, Vals) &&
              isPlatformConditionDisjunction(Or->getRHS(), Kind, Vals));
    }
  } else if (auto *P = dyn_cast<ParenExpr>(E)) {
    return isPlatformConditionDisjunction(P->getSubExpr(), Kind, Vals);
  } else if (auto *C = dyn_cast<CallExpr>(E)) {
    if (getPlatformConditionKind(getDeclRefStr(C->getFn())) != Kind)
      return false;
    if (auto *ArgP = dyn_cast<ParenExpr>(C->getArg())) {
      if (auto *Arg = ArgP->getSubExpr()) {
        auto ArgStr = getDeclRefStr(Arg);
        for (auto V : Vals) {
          if (ArgStr == V)
            return true;
        }
      }
    }
  }
  return false;
}

// Search for the first occurrence of a _likely_ (but not definite) implicit
// simulator-environment platform condition, or negation thereof. This is
// defined as any logical conjunction of one or more os() platform conditions
// _strictly_ from the set {iOS, tvOS, watchOS} and one or more arch() platform
// conditions _strictly_ from the set {i386, x86_64}.
//
// These are (at the time of writing) defined as de-facto simulators in
// Platform.cpp, and if a user is testing them they're _likely_ looking for
// simulator-ness indirectly. If there is anything else in the condition aside
// from these conditions (or the negation of such a conjunction), we
// conservatively assume the user is testing something other than
// simulator-ness.
static Expr *findAnyLikelySimulatorEnvironmentTest(Expr *Condition) {

  if (!Condition)
    return nullptr;

  if (auto *N = dyn_cast<PrefixUnaryExpr>(Condition)) {
    return findAnyLikelySimulatorEnvironmentTest(N->getArg());
  } else if (auto *P = dyn_cast<ParenExpr>(Condition)) {
    return findAnyLikelySimulatorEnvironmentTest(P->getSubExpr());
  }

  // We assume the user is writing the condition in CNF -- say (os(iOS) ||
  // os(tvOS)) && (arch(i386) || arch(x86_64)) -- rather than DNF, as the former
  // is exponentially more terse, and these conditions are already quite
  // unwieldy. If field evidence shows people using other variants, possibly add
  // them here.

  auto isSimulatorPlatformOSTest = [](Expr *E) -> bool {
    return isPlatformConditionDisjunction(
      E, PlatformConditionKind::OS, {"iOS", "tvOS", "watchOS"});
  };

  auto isSimulatorPlatformArchTest = [](Expr *E) -> bool {
    return isPlatformConditionDisjunction(
      E, PlatformConditionKind::Arch, {"i386", "x86_64"});
  };

  if (auto *And = dyn_cast<BinaryExpr>(Condition)) {
    if (getDeclRefStr(And->getFn()) == "&&") {
      if ((isSimulatorPlatformOSTest(And->getLHS()) &&
           isSimulatorPlatformArchTest(And->getRHS())) ||
          (isSimulatorPlatformOSTest(And->getRHS()) &&
           isSimulatorPlatformArchTest(And->getLHS()))) {
        return And;
      }
    }
  }
  return nullptr;
}

} // end anonymous namespace


/// Parse and populate a #if ... #endif directive.
/// Delegate callback function to parse elements in the blocks.
ParserResult<IfConfigDecl> Parser::parseIfConfig(
    llvm::function_ref<void(SmallVectorImpl<ASTNode> &, bool)> parseElements) {
  assert(Tok.is(tok::pound_if));
  SyntaxParsingContext IfConfigCtx(SyntaxContext, SyntaxKind::IfConfigDecl);

  SmallVector<IfConfigClause, 4> Clauses;
  Parser::StructureMarkerRAII ParsingDecl(
      *this, Tok.getLoc(), Parser::StructureMarkerKind::IfConfig);

  // Find the region containing code completion token.
  SourceLoc codeCompletionClauseLoc;
  if (SourceMgr.hasCodeCompletionBuffer() &&
      SourceMgr.getCodeCompletionBufferID() == L->getBufferID() &&
      SourceMgr.isBeforeInBuffer(Tok.getLoc(),
                                 SourceMgr.getCodeCompletionLoc())) {
    llvm::SaveAndRestore<Optional<StableHasher>> H(CurrentTokenHash, None);
    BacktrackingScope backtrack(*this);
    do {
      auto startLoc = Tok.getLoc();
      consumeToken();
      skipUntilConditionalBlockClose();
      auto endLoc = PreviousLoc;
      if (SourceMgr.rangeContainsTokenLoc(SourceRange(startLoc, endLoc),
                                          SourceMgr.getCodeCompletionLoc())){
        codeCompletionClauseLoc = startLoc;
        break;
      }
    } while (Tok.isNot(tok::pound_endif, tok::eof));
  }

  bool shouldEvaluate =
      // Don't evaluate if it's in '-parse' mode, etc.
      shouldEvaluatePoundIfDecls() &&
      // If it's in inactive #if ... #endif block, there's no point to do it.
      !InInactiveClauseEnvironment &&
      // If this directive contains code completion location, 'isActive' is
      // determined solely by which block has the completion token.
      !codeCompletionClauseLoc.isValid();

  bool foundActive = false;
  bool isVersionCondition = false;
  while (1) {
    SyntaxParsingContext ClauseContext(SyntaxContext,
                                       SyntaxKind::IfConfigClause);

    bool isElse = Tok.is(tok::pound_else);
    SourceLoc ClauseLoc = consumeToken();
    Expr *Condition = nullptr;
    bool isActive = false;

    if (!Tok.isAtStartOfLine() && isElse && Tok.is(tok::kw_if)) {
      diagnose(Tok, diag::unexpected_if_following_else_compilation_directive)
          .fixItReplace(SourceRange(ClauseLoc, consumeToken()), "#elseif");
      isElse = false;
    }

    // Parse the condition.  Evaluate it to determine the active
    // clause unless we're doing a parse-only pass.
    if (isElse) {
      isActive = !foundActive && shouldEvaluate;
      if (SyntaxContext->isEnabled())
        SyntaxContext->addRawSyntax(ParsedRawSyntaxNode());
    } else {
      llvm::SaveAndRestore<bool> S(InPoundIfEnvironment, true);
      ParserResult<Expr> Result = parseExprSequence(diag::expected_expr,
                                                      /*isBasic*/true,
                                                      /*isForDirective*/true);
      if (Result.hasCodeCompletion())
        return makeParserCodeCompletionStatus();
      if (Result.isNull())
        return makeParserError();
      Condition = Result.get();
      if (validateIfConfigCondition(Condition, Context, Diags)) {
        // Error in the condition;
        isActive = false;
        isVersionCondition = false;
      } else if (!foundActive && shouldEvaluate) {
        // Evaluate the condition only if we haven't found any active one and
        // we're not in parse-only mode.
        isActive = evaluateIfConfigCondition(Condition, Context);
        isVersionCondition = isVersionIfConfigCondition(Condition);
      }
    }

    // Treat the region containing code completion token as "active".
    if (codeCompletionClauseLoc.isValid() && !foundActive)
      isActive = (ClauseLoc == codeCompletionClauseLoc);

    foundActive |= isActive;

    if (!Tok.isAtStartOfLine() && Tok.isNot(tok::eof)) {
      diagnose(Tok.getLoc(),
               diag::extra_tokens_conditional_compilation_directive);
    }

    if (Expr *Test = findAnyLikelySimulatorEnvironmentTest(Condition)) {
      diagnose(Test->getLoc(),
               diag::likely_simulator_platform_condition)
        .fixItReplace(Test->getSourceRange(),
                      "targetEnvironment(simulator)");
    }

    // Parse elements
    SmallVector<ASTNode, 16> Elements;
    llvm::SaveAndRestore<bool> S(InInactiveClauseEnvironment,
                                 InInactiveClauseEnvironment || !isActive);
    // Disable updating the interface hash inside inactive blocks.
    Optional<llvm::SaveAndRestore<Optional<StableHasher>>> T;
    if (!isActive)
      T.emplace(CurrentTokenHash, None);

    if (isActive || !isVersionCondition) {
      parseElements(Elements, isActive);
    } else if (SyntaxContext->isEnabled()) {
      // We shouldn't skip code if we are building syntax tree.
      // The parser will keep running and we just discard the AST part.
      DiagnosticSuppression suppression(Context.Diags);
      SmallVector<ASTNode, 16> dropedElements;
      parseElements(dropedElements, false);
    } else {
      DiagnosticTransaction DT(Diags);
      skipUntilConditionalBlockClose();
      DT.abort();
    }

    Clauses.emplace_back(ClauseLoc, Condition,
                         Context.AllocateCopy(Elements), isActive);

    if (Tok.isNot(tok::pound_elseif, tok::pound_else))
      break;

    if (isElse)
      diagnose(Tok, diag::expected_close_after_else_directive);
  }
  SyntaxContext->collectNodesInPlace(SyntaxKind::IfConfigClauseList);

  SourceLoc EndLoc;
  bool HadMissingEnd = parseEndIfDirective(EndLoc);

  auto *ICD = new (Context) IfConfigDecl(CurDeclContext,
                                         Context.AllocateCopy(Clauses),
                                         EndLoc, HadMissingEnd);
  return makeParserResult(ICD);
}

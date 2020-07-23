//===--- TypeChecker.cpp - Type Checking ----------------------------------===//
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
// This file implements the swift::performTypeChecking entry point for
// semantic analysis.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "ConstraintSystem.h"
#include "TypeChecker.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "CodeSynthesis.h"
#include "MiscDiagnostics.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>

using namespace swift;

ProtocolDecl *TypeChecker::getProtocol(ASTContext &Context, SourceLoc loc,
                                       KnownProtocolKind kind) {
  auto protocol = Context.getProtocol(kind);
  if (!protocol && loc.isValid()) {
    Context.Diags.diagnose(loc, diag::missing_protocol,
                           Context.getIdentifier(getProtocolName(kind)));
  }

  if (protocol && protocol->isInvalid()) {
    return nullptr;
  }

  return protocol;
}

ProtocolDecl *TypeChecker::getLiteralProtocol(ASTContext &Context, Expr *expr) {
  if (isa<ArrayExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(), KnownProtocolKind::ExpressibleByArrayLiteral);

  if (isa<DictionaryExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(),
        KnownProtocolKind::ExpressibleByDictionaryLiteral);

  if (!isa<LiteralExpr>(expr))
    return nullptr;
  
  if (isa<NilLiteralExpr>(expr))
    return TypeChecker::getProtocol(Context, expr->getLoc(),
                                    KnownProtocolKind::ExpressibleByNilLiteral);

  if (isa<IntegerLiteralExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(),
        KnownProtocolKind::ExpressibleByIntegerLiteral);

  if (isa<FloatLiteralExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(), KnownProtocolKind::ExpressibleByFloatLiteral);

  if (isa<BooleanLiteralExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(),
        KnownProtocolKind::ExpressibleByBooleanLiteral);

  if (const auto *SLE = dyn_cast<StringLiteralExpr>(expr)) {
    if (SLE->isSingleUnicodeScalar())
      return TypeChecker::getProtocol(
          Context, expr->getLoc(),
          KnownProtocolKind::ExpressibleByUnicodeScalarLiteral);

    if (SLE->isSingleExtendedGraphemeCluster())
      return getProtocol(
          Context, expr->getLoc(),
          KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral);

    return TypeChecker::getProtocol(
        Context, expr->getLoc(), KnownProtocolKind::ExpressibleByStringLiteral);
  }

  if (isa<InterpolatedStringLiteralExpr>(expr))
    return TypeChecker::getProtocol(
        Context, expr->getLoc(),
        KnownProtocolKind::ExpressibleByStringInterpolation);

  if (auto E = dyn_cast<MagicIdentifierLiteralExpr>(expr)) {
    switch (E->getKind()) {
#define MAGIC_STRING_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    case MagicIdentifierLiteralExpr::NAME: \
      return TypeChecker::getProtocol( \
          Context, expr->getLoc(), \
          KnownProtocolKind::ExpressibleByStringLiteral);

#define MAGIC_INT_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    case MagicIdentifierLiteralExpr::NAME: \
      return TypeChecker::getProtocol( \
          Context, expr->getLoc(), \
          KnownProtocolKind::ExpressibleByIntegerLiteral);

#define MAGIC_POINTER_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    case MagicIdentifierLiteralExpr::NAME: \
      return nullptr;

#include "swift/AST/MagicIdentifierKinds.def"
    }
  }

  if (auto E = dyn_cast<ObjectLiteralExpr>(expr)) {
    switch (E->getLiteralKind()) {
#define POUND_OBJECT_LITERAL(Name, Desc, Protocol)                             \
  case ObjectLiteralExpr::Name:                                                \
    return TypeChecker::getProtocol(Context, expr->getLoc(),                   \
                                    KnownProtocolKind::Protocol);
#include "swift/Syntax/TokenKinds.def"
    }
  }

  return nullptr;
}

DeclName TypeChecker::getObjectLiteralConstructorName(ASTContext &Context,
                                                      ObjectLiteralExpr *expr) {
  switch (expr->getLiteralKind()) {
  case ObjectLiteralExpr::colorLiteral: {
    return DeclName(Context, DeclBaseName::createConstructor(),
                    { Context.getIdentifier("_colorLiteralRed"),
                      Context.getIdentifier("green"),
                      Context.getIdentifier("blue"),
                      Context.getIdentifier("alpha") });
  }
  case ObjectLiteralExpr::imageLiteral: {
    return DeclName(Context, DeclBaseName::createConstructor(),
                    { Context.getIdentifier("imageLiteralResourceName") });
  }
  case ObjectLiteralExpr::fileLiteral: {
    return DeclName(Context, DeclBaseName::createConstructor(),
            { Context.getIdentifier("fileReferenceLiteralResourceName") });
  }
  }
  llvm_unreachable("unknown literal constructor");
}

ModuleDecl *TypeChecker::getStdlibModule(const DeclContext *dc) {
  if (auto *stdlib = dc->getASTContext().getStdlibModule()) {
    return stdlib;
  }

  return dc->getParentModule();
}

/// Bind the given extension to the given nominal type.
static void bindExtensionToNominal(ExtensionDecl *ext,
                                   NominalTypeDecl *nominal) {
  if (ext->alreadyBoundToNominal())
    return;

  nominal->addExtension(ext);
}

void swift::bindExtensions(ModuleDecl &mod) {
  // Utility function to try and resolve the extended type without diagnosing.
  // If we succeed, we go ahead and bind the extension. Otherwise, return false.
  auto tryBindExtension = [&](ExtensionDecl *ext) -> bool {
    assert(!ext->canNeverBeBound() &&
           "Only extensions that can ever be bound get here.");
    if (auto nominal = ext->computeExtendedNominal()) {
      bindExtensionToNominal(ext, nominal);
      return true;
    }

    return false;
  };

  // Phase 1 - try to bind each extension, adding those whose type cannot be
  // resolved to a worklist.
  SmallVector<ExtensionDecl *, 8> worklist;

  for (auto file : mod.getFiles()) {
    auto *SF = dyn_cast<SourceFile>(file);
    if (!SF)
      continue;

    for (auto D : SF->getTopLevelDecls()) {
      if (auto ED = dyn_cast<ExtensionDecl>(D))
        if (!tryBindExtension(ED))
          worklist.push_back(ED);
    }
  }

  // Phase 2 - repeatedly go through the worklist and attempt to bind each
  // extension there, removing it from the worklist if we succeed.
  bool changed;
  do {
    changed = false;

    auto last = std::remove_if(worklist.begin(), worklist.end(),
                               tryBindExtension);
    if (last != worklist.end()) {
      worklist.erase(last, worklist.end());
      changed = true;
    }
  } while(changed);

  // Any remaining extensions are invalid. They will be diagnosed later by
  // typeCheckDecl().
}

static void typeCheckDelayedFunctions(SourceFile &SF) {
  unsigned currentFunctionIdx = 0;
  unsigned currentSynthesizedDecl = SF.LastCheckedSynthesizedDecl;
  do {
    // Type check the body of each of the function in turn.  Note that outside
    // functions must be visited before nested functions for type-checking to
    // work correctly.
    for (unsigned n = SF.DelayedFunctions.size(); currentFunctionIdx != n;
         ++currentFunctionIdx) {
      auto *AFD = SF.DelayedFunctions[currentFunctionIdx];
      assert(!AFD->getDeclContext()->isLocalContext());

      TypeChecker::typeCheckAbstractFunctionBody(AFD);
    }

    // Type check synthesized functions and their bodies.
    for (unsigned n = SF.SynthesizedDecls.size();
         currentSynthesizedDecl != n;
         ++currentSynthesizedDecl) {
      auto decl = SF.SynthesizedDecls[currentSynthesizedDecl];
      TypeChecker::typeCheckDecl(decl);
    }

  } while (currentFunctionIdx < SF.DelayedFunctions.size() ||
           currentSynthesizedDecl < SF.SynthesizedDecls.size());

  SF.DelayedFunctions.clear();
}

void swift::performTypeChecking(SourceFile &SF) {
  return (void)evaluateOrDefault(SF.getASTContext().evaluator,
                                 TypeCheckSourceFileRequest{&SF}, {});
}

evaluator::SideEffect
TypeCheckSourceFileRequest::evaluate(Evaluator &eval, SourceFile *SF) const {
  assert(SF && "Source file cannot be null!");
  assert(SF->ASTStage != SourceFile::TypeChecked &&
         "Should not be re-typechecking this file!");

  // Eagerly build the top-level scopes tree before type checking
  // because type-checking expressions mutates the AST and that throws off the
  // scope-based lookups. Only the top-level scopes because extensions have not
  // been bound yet.
  auto &Ctx = SF->getASTContext();
  if (Ctx.LangOpts.EnableASTScopeLookup)
    SF->getScope()
        .buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals();

  BufferIndirectlyCausingDiagnosticRAII cpr(*SF);

  // Could build scope maps here because the AST is stable now.

  {
    FrontendStatsTracer tracer(Ctx.Stats,
                               "Type checking and Semantic analysis");

    if (!Ctx.LangOpts.DisableAvailabilityChecking) {
      // Build the type refinement hierarchy for the primary
      // file before type checking.
      TypeChecker::buildTypeRefinementContextHierarchy(*SF);
    }

    // Type check the top-level elements of the source file.
    for (auto D : SF->getTopLevelDecls()) {
      if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
        TypeChecker::typeCheckTopLevelCodeDecl(TLCD);
        TypeChecker::contextualizeTopLevelCode(TLCD);
      } else {
        TypeChecker::typeCheckDecl(D);
      }
    }

    typeCheckDelayedFunctions(*SF);

    // re-typecheck protocol extensions to diagnose any redundant conformances
    if (SF->getASTContext().LangOpts.EnableConformingExtensions) {
      for (auto D : SF->getTopLevelDecls())
        if (auto *ED = dyn_cast<ExtensionDecl>(D))
          if (ED->getExtendedProtocolDecl() && ED->getInherited().size())
            TypeChecker::typeCheckDecl(ED);
    }
  }

  // Check to see if there's any inconsistent @_implementationOnly imports.
  evaluateOrDefault(
      Ctx.evaluator,
      CheckInconsistentImplementationOnlyImportsRequest{SF->getParentModule()},
      {});

  // Perform various AST transforms we've been asked to perform.
  if (!Ctx.hadError() && Ctx.LangOpts.DebuggerTestingTransform)
    performDebuggerTestingTransform(*SF);

  if (!Ctx.hadError() && Ctx.LangOpts.PCMacro)
    performPCMacro(*SF);

  // Playground transform knows to look out for PCMacro's changes and not
  // to playground log them.
  if (!Ctx.hadError() && Ctx.LangOpts.PlaygroundTransform)
    performPlaygroundTransform(*SF, Ctx.LangOpts.PlaygroundHighPerformance);

  return std::make_tuple<>();
}

void swift::performWholeModuleTypeChecking(SourceFile &SF) {
  auto &Ctx = SF.getASTContext();
  FrontendStatsTracer tracer(Ctx.Stats,
                             "perform-whole-module-type-checking");
  diagnoseObjCMethodConflicts(SF);
  diagnoseObjCUnsatisfiedOptReqConflicts(SF);
  diagnoseUnintendedObjCMethodOverrides(SF);
}

bool swift::isAdditiveArithmeticConformanceDerivationEnabled(SourceFile &SF) {
  auto &ctx = SF.getASTContext();
  // Return true if `AdditiveArithmetic` derived conformances are explicitly
  // enabled.
  if (ctx.LangOpts.EnableExperimentalAdditiveArithmeticDerivedConformances)
    return true;
  // Otherwise, return true iff differentiable programming is enabled.
  // Differentiable programming depends on `AdditiveArithmetic` derived
  // conformances.
  return isDifferentiableProgrammingEnabled(SF);
}

bool swift::performTypeLocChecking(ASTContext &Ctx, TypeLoc &T,
                                   DeclContext *DC,
                                   bool ProduceDiagnostics) {
  return performTypeLocChecking(
                            Ctx, T,
                            /*isSILMode=*/false,
                            /*isSILType=*/false,
                            /*GenericEnv=*/DC->getGenericEnvironmentOfContext(),
                            DC, ProduceDiagnostics);
}

bool swift::performTypeLocChecking(ASTContext &Ctx, TypeLoc &T,
                                   bool isSILMode,
                                   bool isSILType,
                                   GenericEnvironment *GenericEnv,
                                   DeclContext *DC,
                                   bool ProduceDiagnostics) {
  TypeResolutionOptions options = None;
  if (isSILMode) {
    options |= TypeResolutionFlags::SILMode;
  }
  if (isSILType)
    options |= TypeResolutionFlags::SILType;

  const auto resolution =
      TypeResolution::forContextual(DC, GenericEnv, options,
                                    [](auto unboundTy) {
        // FIXME: Don't let unbound generic types escape type resolution.
        // For now, just return the unbound generic type.
        return unboundTy;
      });

  Optional<DiagnosticSuppression> suppression;
  if (!ProduceDiagnostics)
    suppression.emplace(Ctx.Diags);

  // If we've already validated this type, don't do so again.
  if (T.wasValidated()) {
    return T.isError();
  }

  auto type = resolution.resolveType(T.getTypeRepr());
  T.setType(type);
  return type->hasError();
}

/// Expose TypeChecker's handling of GenericParamList to SIL parsing.
GenericEnvironment *
swift::handleSILGenericParams(GenericParamList *genericParams,
                              DeclContext *DC) {
  if (genericParams == nullptr)
    return nullptr;

  SmallVector<GenericParamList *, 2> nestedList;
  for (; genericParams; genericParams = genericParams->getOuterParameters()) {
    nestedList.push_back(genericParams);
  }

  std::reverse(nestedList.begin(), nestedList.end());

  for (unsigned i = 0, e = nestedList.size(); i < e; ++i) {
    auto genericParams = nestedList[i];
    genericParams->setDepth(i);
  }

  auto sig =
      TypeChecker::checkGenericSignature(nestedList.back(), DC,
                                         /*parentSig=*/nullptr,
                                         /*allowConcreteGenericParams=*/true);
  return (sig ? sig->getGenericEnvironment() : nullptr);
}

void swift::typeCheckPatternBinding(PatternBindingDecl *PBD,
                                    unsigned bindingIndex) {
  assert(!PBD->isInitializerChecked(bindingIndex) &&
         PBD->getInit(bindingIndex));

  auto &Ctx = PBD->getASTContext();
  DiagnosticSuppression suppression(Ctx.Diags);
  (void)evaluateOrDefault(
      Ctx.evaluator, PatternBindingEntryRequest{PBD, bindingIndex}, nullptr);
  TypeChecker::typeCheckPatternBinding(PBD, bindingIndex);
}

bool swift::typeCheckAbstractFunctionBodyAtLoc(AbstractFunctionDecl *AFD,
                                               SourceLoc TargetLoc) {
  auto &Ctx = AFD->getASTContext();
  DiagnosticSuppression suppression(Ctx.Diags);
  return !evaluateOrDefault(Ctx.evaluator,
                            TypeCheckFunctionBodyAtLocRequest{AFD, TargetLoc},
                            true);
}

bool swift::typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
  auto &Ctx = static_cast<Decl *>(TLCD)->getASTContext();
  DiagnosticSuppression suppression(Ctx.Diags);
  TypeChecker::typeCheckTopLevelCodeDecl(TLCD);
  return true;
}

void TypeChecker::checkForForbiddenPrefix(ASTContext &C, DeclBaseName Name) {
  if (C.TypeCheckerOpts.DebugForbidTypecheckPrefix.empty())
    return;

  // Don't touch special names or empty names.
  if (Name.isSpecial() || Name.empty())
    return;

  StringRef Str = Name.getIdentifier().str();
  if (Str.startswith(C.TypeCheckerOpts.DebugForbidTypecheckPrefix)) {
    std::string Msg = "forbidden typecheck occurred: ";
    Msg += Str;
    llvm::report_fatal_error(Msg);
  }
}

DeclTypeCheckingSemantics
TypeChecker::getDeclTypeCheckingSemantics(ValueDecl *decl) {
  // Check for a @_semantics attribute.
  if (auto semantics = decl->getAttrs().getAttribute<SemanticsAttr>()) {
    if (semantics->Value.equals("typechecker.type(of:)"))
      return DeclTypeCheckingSemantics::TypeOf;
    if (semantics->Value.equals("typechecker.withoutActuallyEscaping(_:do:)"))
      return DeclTypeCheckingSemantics::WithoutActuallyEscaping;
    if (semantics->Value.equals("typechecker._openExistential(_:do:)"))
      return DeclTypeCheckingSemantics::OpenExistential;
  }
  return DeclTypeCheckingSemantics::Normal;
}

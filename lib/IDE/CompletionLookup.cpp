//===--- CompletionLookup.cpp ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CompletionLookup.h"
#include "CodeCompletionResultBuilder.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"

using namespace swift;
using namespace swift::ide;

namespace {

static bool SwiftKeyPathFilter(ValueDecl *decl, DeclVisibilityKind) {
  switch (decl->getKind()) {
  case DeclKind::Var:
  case DeclKind::Subscript:
    return true;
  default:
    return false;
  }
}

static bool isTopLevelSubcontext(const DeclContext *DC) {
  for (; DC && DC->isLocalContext(); DC = DC->getParent()) {
    switch (DC->getContextKind()) {
    case DeclContextKind::TopLevelCodeDecl:
      return true;
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
    case DeclContextKind::EnumElementDecl:
      return false;
    default:
      continue;
    }
  }
  return false;
}

static KnownProtocolKind
protocolForLiteralKind(CodeCompletionLiteralKind kind) {
  switch (kind) {
  case CodeCompletionLiteralKind::ArrayLiteral:
    return KnownProtocolKind::ExpressibleByArrayLiteral;
  case CodeCompletionLiteralKind::BooleanLiteral:
    return KnownProtocolKind::ExpressibleByBooleanLiteral;
  case CodeCompletionLiteralKind::ColorLiteral:
    return KnownProtocolKind::ExpressibleByColorLiteral;
  case CodeCompletionLiteralKind::ImageLiteral:
    return KnownProtocolKind::ExpressibleByImageLiteral;
  case CodeCompletionLiteralKind::DictionaryLiteral:
    return KnownProtocolKind::ExpressibleByDictionaryLiteral;
  case CodeCompletionLiteralKind::IntegerLiteral:
    return KnownProtocolKind::ExpressibleByIntegerLiteral;
  case CodeCompletionLiteralKind::NilLiteral:
    return KnownProtocolKind::ExpressibleByNilLiteral;
  case CodeCompletionLiteralKind::StringLiteral:
    return KnownProtocolKind::ExpressibleByUnicodeScalarLiteral;
  case CodeCompletionLiteralKind::Tuple:
    llvm_unreachable("no such protocol kind");
  }

  llvm_unreachable("Unhandled CodeCompletionLiteralKind in switch.");
}

static Type defaultTypeLiteralKind(CodeCompletionLiteralKind kind,
                                   ASTContext &Ctx) {
  switch (kind) {
  case CodeCompletionLiteralKind::BooleanLiteral:
    return Ctx.getBoolType();
  case CodeCompletionLiteralKind::IntegerLiteral:
    return Ctx.getIntType();
  case CodeCompletionLiteralKind::StringLiteral:
    return Ctx.getStringType();
  case CodeCompletionLiteralKind::ArrayLiteral:
    return Ctx.getArrayDecl()->getDeclaredType();
  case CodeCompletionLiteralKind::DictionaryLiteral:
    return Ctx.getDictionaryDecl()->getDeclaredType();
  case CodeCompletionLiteralKind::NilLiteral:
  case CodeCompletionLiteralKind::ColorLiteral:
  case CodeCompletionLiteralKind::ImageLiteral:
  case CodeCompletionLiteralKind::Tuple:
    return Type();
  }

  llvm_unreachable("Unhandled CodeCompletionLiteralKind in switch.");
}

/// Whether funcType has a single argument (not including defaulted arguments)
/// that is of type () -> ().
static bool hasTrivialTrailingClosure(const FuncDecl *FD,
                                      AnyFunctionType *funcType) {
  ParameterListInfo paramInfo(funcType->getParams(), FD,
                              /*skipCurriedSelf*/ FD->hasCurriedSelf());

  if (paramInfo.size() - paramInfo.numNonDefaultedParameters() == 1) {
    auto param = funcType->getParams().back();
    if (!param.isAutoClosure()) {
      if (auto Fn = param.getOldType()->getAs<AnyFunctionType>()) {
        return Fn->getParams().empty() && Fn->getResult()->isVoid();
      }
    }
  }

  return false;
}
} // end anonymous namespace

bool swift::ide::DefaultFilter(ValueDecl *VD, DeclVisibilityKind Kind,
                               DynamicLookupInfo dynamicLookupInfo) {
  return true;
}

bool swift::ide::KeyPathFilter(ValueDecl *decl, DeclVisibilityKind,
                               DynamicLookupInfo dynamicLookupInfo) {
  return isa<TypeDecl>(decl) ||
         (isa<VarDecl>(decl) && decl->getDeclContext()->isTypeContext());
}

bool swift::ide::MacroFilter(ValueDecl *decl, DeclVisibilityKind,
                             DynamicLookupInfo dynamicLookupInfo) {
  return isa<MacroDecl>(decl);
}

bool swift::ide::isCodeCompletionAtTopLevel(const DeclContext *DC) {
  if (DC->isModuleScopeContext())
    return true;

  // CC token at top-level is parsed as an expression. If the only element
  // body of the TopLevelCodeDecl is a CodeCompletionExpr without a base
  // expression, the user might be writing a top-level declaration.
  if (const TopLevelCodeDecl *TLCD = dyn_cast<const TopLevelCodeDecl>(DC)) {
    auto body = TLCD->getBody();
    if (!body || body->empty())
      return true;
    if (body->getElements().size() > 1)
      return false;
    auto expr = body->getFirstElement().dyn_cast<Expr *>();
    if (!expr)
      return false;
    if (CodeCompletionExpr *CCExpr = dyn_cast<CodeCompletionExpr>(expr)) {
      if (CCExpr->getBase() == nullptr)
        return true;
    }
  }

  return false;
}

bool swift::ide::isCompletionDeclContextLocalContext(DeclContext *DC) {
  if (!DC->isLocalContext())
    return false;
  if (isCodeCompletionAtTopLevel(DC))
    return false;
  return true;
}

/// Returns \c true if \p DC can handles async call.
bool swift::ide::canDeclContextHandleAsync(const DeclContext *DC) {
  if (auto *func = dyn_cast<AbstractFunctionDecl>(DC))
    return func->isAsyncContext();

  if (auto *closure = dyn_cast<ClosureExpr>(DC)) {
    // See if the closure has 'async' function type.
    if (auto closureType = closure->getType())
      if (auto fnType = closureType->getAs<AnyFunctionType>())
        if (fnType->isAsync())
          return true;

    // If the closure doesn't contain any async call in the body, closure itself
    // doesn't have 'async' type even if 'async' closure is expected.
    //   func foo(fn: () async -> Void)
    //   foo { <HERE> }
    // In this case, the closure is wrapped with a 'FunctionConversionExpr'
    // which has 'async' function type.
    struct AsyncClosureChecker : public ASTWalker {
      const ClosureExpr *Target;
      bool Result = false;

      /// Walk everything in a macro.
      MacroWalking getMacroWalkingBehavior() const override {
        return MacroWalking::ArgumentsAndExpansion;
      }

      AsyncClosureChecker(const ClosureExpr *Target) : Target(Target) {}

      PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
        if (E == Target)
          return Action::SkipChildren(E);

        if (auto conversionExpr = dyn_cast<FunctionConversionExpr>(E)) {
          if (conversionExpr->getSubExpr() == Target) {
            if (conversionExpr->getType()->is<AnyFunctionType>() &&
                conversionExpr->getType()->castTo<AnyFunctionType>()->isAsync())
              Result = true;
            return Action::SkipChildren(E);
          }
        }
        return Action::Continue(E);
      }
    } checker(closure);
    closure->getParent()->walkContext(checker);
    return checker.Result;
  }

  return false;
}

/// Return \c true if the completion happens at top-level of a library file.
bool swift::ide::isCodeCompletionAtTopLevelOfLibraryFile(
    const DeclContext *DC) {
  if (DC->getParentSourceFile()->isScriptMode())
    return false;
  return isCodeCompletionAtTopLevel(DC);
}

// MARK: - CompletionLookup

void CompletionLookup::foundFunction(const AbstractFunctionDecl *AFD) {
  FoundFunctionCalls = true;
  const DeclName Name = AFD->getName();
  auto ArgNames = Name.getArgumentNames();
  if (ArgNames.empty())
    return;
  if (ArgNames[0].empty())
    FoundFunctionsWithoutFirstKeyword = true;
}

void CompletionLookup::foundFunction(const AnyFunctionType *AFT) {
  FoundFunctionCalls = true;
  auto Params = AFT->getParams();
  if (Params.empty())
    return;
  if (Params.size() == 1 && !Params[0].hasLabel()) {
    FoundFunctionsWithoutFirstKeyword = true;
    return;
  }
  if (!Params[0].hasLabel())
    FoundFunctionsWithoutFirstKeyword = true;
}

bool CompletionLookup::canBeUsedAsRequirementFirstType(Type selfTy,
                                                       TypeAliasDecl *TAD) {
  auto T = TAD->getDeclaredInterfaceType();
  auto subMap = selfTy->getMemberSubstitutionMap(TAD->getParentModule(), TAD);
  T = T.subst(subMap)->getCanonicalType();

  ArchetypeType *archeTy = T->getAs<ArchetypeType>();
  if (!archeTy)
    return false;
  archeTy = archeTy->getRoot();

  // For protocol, the 'archeTy' should match with the 'baseTy' which is the
  // dynamic 'Self' type of the protocol. For nominal decls, 'archTy' should
  // be one of the generic params in 'selfTy'. Search 'archeTy' in 'baseTy'.
  return selfTy.findIf([&](Type T) { return archeTy->isEqual(T); });
}

CompletionLookup::CompletionLookup(CodeCompletionResultSink &Sink,
                                   ASTContext &Ctx,
                                   const DeclContext *CurrDeclContext,
                                   CodeCompletionContext *CompletionContext)
    : Sink(Sink), Ctx(Ctx), CurrDeclContext(CurrDeclContext),
      CurrModule(CurrDeclContext ? CurrDeclContext->getParentModule()
                                 : nullptr),
      Importer(static_cast<ClangImporter *>(
          CurrDeclContext->getASTContext().getClangModuleLoader())),
      CompletionContext(CompletionContext) {
  // Determine if we are doing code completion inside a static method.
  if (CurrDeclContext) {
    CurrentMethod = CurrDeclContext->getInnermostMethodContext();
    if (auto *FD = dyn_cast_or_null<FuncDecl>(CurrentMethod))
      InsideStaticMethod = FD->isStatic();
    CanCurrDeclContextHandleAsync = canDeclContextHandleAsync(CurrDeclContext);
  }
}

void CompletionLookup::addSubModuleNames(
    std::vector<std::pair<std::string, bool>> &SubModuleNameVisibilityPairs) {
  for (auto &Pair : SubModuleNameVisibilityPairs) {
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        CodeCompletionResultKind::Declaration, SemanticContextKind::None);
    auto MD = ModuleDecl::create(Ctx.getIdentifier(Pair.first), Ctx);
    MD->setFailedToLoad();
    Builder.setAssociatedDecl(MD);
    Builder.addBaseName(MD->getNameStr());
    Builder.addTypeAnnotation("Module");
    if (Pair.second)
      Builder.setContextualNotRecommended(
          ContextualNotRecommendedReason::RedundantImport);
  }
}

void CompletionLookup::collectImportedModules(
    llvm::StringSet<> &directImportedModules,
    llvm::StringSet<> &allImportedModules) {
  SmallVector<ImportedModule, 16> Imported;
  SmallVector<ImportedModule, 16> FurtherImported;
  CurrDeclContext->getParentSourceFile()->getImportedModules(
      Imported, ModuleDecl::getImportFilterLocal());

  for (ImportedModule &imp : Imported)
    directImportedModules.insert(imp.importedModule->getNameStr());

  while (!Imported.empty()) {
    ModuleDecl *MD = Imported.back().importedModule;
    Imported.pop_back();
    if (!allImportedModules.insert(MD->getNameStr()).second)
      continue;
    FurtherImported.clear();
    MD->getImportedModules(FurtherImported,
                           ModuleDecl::ImportFilterKind::Exported);
    Imported.append(FurtherImported.begin(), FurtherImported.end());
  }
}

void CompletionLookup::addModuleName(
    ModuleDecl *MD, Optional<ContextualNotRecommendedReason> R) {

  // Don't add underscored cross-import overlay modules.
  if (MD->getDeclaringModuleIfCrossImportOverlay())
    return;

  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Declaration, SemanticContextKind::None);
  Builder.setAssociatedDecl(MD);
  auto moduleName = MD->getName();

  // This checks if module aliasing was used. For example, when editing
  // `import ...`, and `-module-alias Foo=Bar` was passed, we want to show
  // Foo as an option to import, instead of Bar (name of the binary), as
  // Foo is the name that should appear in source files.
  auto aliasedName = Ctx.getRealModuleName(
      moduleName, ASTContext::ModuleAliasLookupOption::aliasFromRealName);
  if (aliasedName != moduleName && // check if module aliasing was applied
      !aliasedName.empty()) { // check an alias mapped to the binary name exists
    moduleName = aliasedName; // if so, use the aliased name
  }
  Builder.addBaseName(moduleName.str());
  Builder.addTypeAnnotation("Module");
  if (R)
    Builder.setContextualNotRecommended(*R);
}

void CompletionLookup::addImportModuleNames() {
  SmallVector<Identifier, 0> ModuleNames;
  Ctx.getVisibleTopLevelModuleNames(ModuleNames);

  llvm::StringSet<> directImportedModules;
  llvm::StringSet<> allImportedModules;
  collectImportedModules(directImportedModules, allImportedModules);

  auto mainModuleName = CurrModule->getName();
  for (auto ModuleName : ModuleNames) {
    if (ModuleName == mainModuleName || isHiddenModuleName(ModuleName))
      continue;

    auto MD = ModuleDecl::create(ModuleName, Ctx);
    MD->setFailedToLoad();

    Optional<ContextualNotRecommendedReason> Reason = None;

    // Imported modules are not recommended.
    if (directImportedModules.contains(MD->getNameStr())) {
      Reason = ContextualNotRecommendedReason::RedundantImport;
    } else if (allImportedModules.contains(MD->getNameStr())) {
      Reason = ContextualNotRecommendedReason::RedundantImportIndirect;
    }

    addModuleName(MD, Reason);
  }
}

SemanticContextKind
CompletionLookup::getSemanticContext(const Decl *D, DeclVisibilityKind Reason,
                                     DynamicLookupInfo dynamicLookupInfo) {
  if (ForcedSemanticContext)
    return *ForcedSemanticContext;

  switch (Reason) {
  case DeclVisibilityKind::LocalVariable:
  case DeclVisibilityKind::FunctionParameter:
  case DeclVisibilityKind::GenericParameter:
    return SemanticContextKind::Local;

  case DeclVisibilityKind::MemberOfCurrentNominal:
    return SemanticContextKind::CurrentNominal;

  case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
  case DeclVisibilityKind::MemberOfSuper:
    return SemanticContextKind::Super;

  case DeclVisibilityKind::MemberOfOutsideNominal:
    return SemanticContextKind::OutsideNominal;

  case DeclVisibilityKind::VisibleAtTopLevel:
    if (CurrDeclContext && D->getModuleContext() == CurrModule) {
      // Treat global variables from the same source file as local when
      // completing at top-level.
      if (isa<VarDecl>(D) && isTopLevelSubcontext(CurrDeclContext) &&
          D->getDeclContext()->getParentSourceFile() ==
              CurrDeclContext->getParentSourceFile()) {
        return SemanticContextKind::Local;
      } else {
        return SemanticContextKind::CurrentModule;
      }
    } else {
      return SemanticContextKind::OtherModule;
    }

  case DeclVisibilityKind::DynamicLookup:
    switch (dynamicLookupInfo.getKind()) {
    case DynamicLookupInfo::None:
      llvm_unreachable("invalid DynamicLookupInfo::Kind for dynamic lookup");

    case DynamicLookupInfo::AnyObject:
      // AnyObject results can come from different modules, including the
      // current module, but we always assign them the OtherModule semantic
      // context.  These declarations are uniqued by signature, so it is
      // totally random (determined by the hash function) which of the
      // equivalent declarations (across multiple modules) we will get.
      return SemanticContextKind::OtherModule;

    case DynamicLookupInfo::KeyPathDynamicMember:
      // Use the visibility of the underlying declaration.
      // FIXME: KeyPath<AnyObject, U> !?!?
      assert(dynamicLookupInfo.getKeyPathDynamicMember().originalVisibility !=
             DeclVisibilityKind::DynamicLookup);
      return getSemanticContext(
          D, dynamicLookupInfo.getKeyPathDynamicMember().originalVisibility,
          {});
    }

  case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
    llvm_unreachable("should not see this kind");
  }
  llvm_unreachable("unhandled kind");
}

bool CompletionLookup::isUnresolvedMemberIdealType(Type Ty) {
  assert(Ty);
  if (!IsUnresolvedMember)
    return false;
  Type idealTy = expectedTypeContext.getIdealType();
  if (!idealTy)
    return false;
  /// Consider optional object type is the ideal.
  /// For example:
  ///   enum MyEnum { case foo, bar }
  ///   func foo(_: MyEnum?)
  ///   fooo(.<HERE>)
  /// Prefer '.foo' and '.bar' over '.some' and '.none'.
  idealTy = idealTy->lookThroughAllOptionalTypes();
  return idealTy->isEqual(Ty);
}

CodeCompletionResultBuilder
CompletionLookup::makeResultBuilder(CodeCompletionResultKind kind,
                                    SemanticContextKind semanticContext) const {
  CodeCompletionResultBuilder builder(Sink, kind, semanticContext);
  builder.setTypeContext(expectedTypeContext, CurrDeclContext);
  return builder;
}

void CompletionLookup::addValueBaseName(CodeCompletionResultBuilder &Builder,
                                        DeclBaseName Name) {
  auto NameStr = Name.userFacingName();
  bool shouldEscapeKeywords;
  if (Name.isSpecial()) {
    // Special names (i.e. 'init') are always displayed as its user facing
    // name.
    shouldEscapeKeywords = false;
  } else if (ExprType) {
    // After dot. User can write any keyword after '.' except for `init` and
    // `self`. E.g. 'func `init`()' must be called by 'expr.`init`()'.
    shouldEscapeKeywords = NameStr == "self" || NameStr == "init";
  } else {
    // As primary expresson. We have to escape almost every keywords except
    // for 'self' and 'Self'.
    shouldEscapeKeywords = NameStr != "self" && NameStr != "Self";
  }

  if (!shouldEscapeKeywords) {
    Builder.addBaseName(NameStr);
  } else {
    SmallString<16> buffer;
    Builder.addBaseName(Builder.escapeKeyword(NameStr, true, buffer));
  }
}

void CompletionLookup::addLeadingDot(CodeCompletionResultBuilder &Builder) {
  if (NeedOptionalUnwrap) {
    Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
    Builder.addQuestionMark();
    Builder.addLeadingDot();
    return;
  }
  if (needDot())
    Builder.addLeadingDot();
}

void CompletionLookup::addTypeAnnotation(CodeCompletionResultBuilder &Builder,
                                         Type T, GenericSignature genericSig) {
  PrintOptions PO;
  PO.OpaqueReturnTypePrinting =
      PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
  if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
    PO.setBaseType(typeContext->getDeclaredTypeInContext());
  Builder.addTypeAnnotation(eraseArchetypes(T, genericSig), PO);
  Builder.setResultTypes(T);
}

void CompletionLookup::addTypeAnnotationForImplicitlyUnwrappedOptional(
    CodeCompletionResultBuilder &Builder, Type T, GenericSignature genericSig,
    bool dynamicOrOptional) {

  std::string suffix;
  // FIXME: This retains previous behavior, but in reality the type of dynamic
  // lookups is IUO, not Optional as it is for the @optional attribute.
  if (dynamicOrOptional) {
    T = T->getOptionalObjectType();
    suffix = "?";
  }

  PrintOptions PO;
  PO.PrintOptionalAsImplicitlyUnwrapped = true;
  PO.OpaqueReturnTypePrinting =
      PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
  if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
    PO.setBaseType(typeContext->getDeclaredTypeInContext());
  Builder.addTypeAnnotation(eraseArchetypes(T, genericSig), PO, suffix);
  Builder.setResultTypes(T);
  Builder.setTypeContext(expectedTypeContext, CurrDeclContext);
}

/// For printing in code completion results, replace archetypes with
/// protocol compositions.
///
/// FIXME: Perhaps this should be an option in PrintOptions instead.
Type CompletionLookup::eraseArchetypes(Type type, GenericSignature genericSig) {
  if (!genericSig)
    return type;

  auto buildProtocolComposition = [&](ArrayRef<ProtocolDecl *> protos) -> Type {
    SmallVector<Type, 2> types;
    for (auto proto : protos)
      types.push_back(proto->getDeclaredInterfaceType());
    return ProtocolCompositionType::get(Ctx, types,
                                        /*HasExplicitAnyObject=*/false);
  };

  if (auto *genericFuncType = type->getAs<GenericFunctionType>()) {
    SmallVector<AnyFunctionType::Param, 8> erasedParams;
    for (const auto &param : genericFuncType->getParams()) {
      auto erasedTy = eraseArchetypes(param.getPlainType(), genericSig);
      erasedParams.emplace_back(param.withType(erasedTy));
    }
    return GenericFunctionType::get(
        genericSig, erasedParams,
        eraseArchetypes(genericFuncType->getResult(), genericSig),
        genericFuncType->getExtInfo());
  }

  return type.transform([&](Type t) -> Type {
    // FIXME: Code completion should only deal with one or the other,
    // and not both.
    if (auto *archetypeType = t->getAs<ArchetypeType>()) {
      // Don't erase opaque archetype.
      if (isa<OpaqueTypeArchetypeType>(archetypeType) &&
          archetypeType->isRoot())
        return t;

      auto protos = archetypeType->getConformsTo();
      if (!protos.empty())
        return buildProtocolComposition(protos);
    }

    if (t->isTypeParameter()) {
      const auto protos = genericSig->getRequiredProtocols(t);
      if (!protos.empty())
        return buildProtocolComposition(protos);
    }

    return t;
  });
}

Type CompletionLookup::getTypeOfMember(const ValueDecl *VD,
                                       DynamicLookupInfo dynamicLookupInfo) {
  switch (dynamicLookupInfo.getKind()) {
  case DynamicLookupInfo::None:
    return getTypeOfMember(VD, this->ExprType);
  case DynamicLookupInfo::AnyObject:
    return getTypeOfMember(VD, Type());
  case DynamicLookupInfo::KeyPathDynamicMember: {
    auto &keyPathInfo = dynamicLookupInfo.getKeyPathDynamicMember();

    // Map the result of VD to keypath member lookup results.
    // Given:
    //   struct Wrapper<T> {
    //     subscript<U>(dynamicMember: KeyPath<T, U>) -> Wrapped<U> { get }
    //   }
    //   struct Circle {
    //     var center: Point { get }
    //     var radius: Length { get }
    //   }
    //
    // Consider 'Wrapper<Circle>.center'.
    //   'VD' is 'Circle.center' decl.
    //   'keyPathInfo.subscript' is 'Wrapper<T>.subscript' decl.
    //   'keyPathInfo.baseType' is 'Wrapper<Circle>' type.

    // FIXME: Handle nested keypath member lookup.
    // i.e. cases where 'ExprType' != 'keyPathInfo.baseType'.

    auto *SD = keyPathInfo.subscript;
    const auto elementTy = SD->getElementInterfaceType();
    if (!elementTy->hasTypeParameter())
      return elementTy;

    // Map is:
    //   { τ_0_0(T) => Circle
    //     τ_1_0(U) => U }
    auto subs = keyPathInfo.baseType->getMemberSubstitutions(SD);

    // If the keyPath result type has type parameters, that might affect the
    // subscript result type.
    auto keyPathResultTy =
        getResultTypeOfKeypathDynamicMember(SD)->mapTypeOutOfContext();
    if (keyPathResultTy->hasTypeParameter()) {
      auto keyPathRootTy = getRootTypeOfKeypathDynamicMember(SD).subst(
          QueryTypeSubstitutionMap{subs},
          LookUpConformanceInModule(CurrModule));

      // The result type of the VD.
      // i.e. 'Circle.center' => 'Point'.
      auto innerResultTy = getTypeOfMember(VD, keyPathRootTy);

      if (auto paramTy = keyPathResultTy->getAs<GenericTypeParamType>()) {
        // Replace keyPath result type in the map with the inner result type.
        // i.e. Make the map as:
        //   { τ_0_0(T) => Circle
        //     τ_1_0(U) => Point }
        auto key = paramTy->getCanonicalType()->castTo<GenericTypeParamType>();
        subs[key] = innerResultTy;
      } else {
        // FIXME: Handle the case where the KeyPath result is generic.
        // e.g. 'subscript<U>(dynamicMember: KeyPath<T, Box<U>>) -> Bag<U>'
        // For now, just return the inner type.
        return innerResultTy;
      }
    }

    // Substitute the element type of the subscript using modified map.
    // i.e. 'Wrapped<U>' => 'Wrapped<Point>'.
    return elementTy.subst(QueryTypeSubstitutionMap{subs},
                           LookUpConformanceInModule(CurrModule));
  }
  }
  llvm_unreachable("Unhandled DynamicLookupInfo Kind in switch");
}

Type CompletionLookup::getTypeOfMember(const ValueDecl *VD, Type ExprType) {
  Type T = VD->getInterfaceType();
  assert(!T.isNull());

  if (ExprType) {
    Type ContextTy = VD->getDeclContext()->getDeclaredInterfaceType();
    if (ContextTy) {
      // Look through lvalue types and metatypes
      Type MaybeNominalType = ExprType->getRValueType();

      if (auto Metatype = MaybeNominalType->getAs<MetatypeType>())
        MaybeNominalType = Metatype->getInstanceType();

      if (auto SelfType = MaybeNominalType->getAs<DynamicSelfType>())
        MaybeNominalType = SelfType->getSelfType();

      // For optional protocol requirements and dynamic dispatch,
      // strip off optionality from the base type, but only if
      // we're not actually completing a member of Optional.
      if (!ContextTy->getOptionalObjectType() &&
          MaybeNominalType->getOptionalObjectType())
        MaybeNominalType = MaybeNominalType->getOptionalObjectType();

      // For dynamic lookup don't substitute in the base type.
      if (MaybeNominalType->isAnyObject())
        return T;

      // FIXME: Sometimes ExprType is the type of the member here,
      // and not the type of the base. That is inconsistent and
      // should be cleaned up.
      if (!MaybeNominalType->mayHaveMembers())
        return T;

      // We can't do anything if the base type has unbound generic parameters.
      if (MaybeNominalType->hasUnboundGenericType())
        return T;

      // For everything else, substitute in the base type.
      auto Subs = MaybeNominalType->getMemberSubstitutionMap(CurrModule, VD);

      // For a GenericFunctionType, we only want to substitute the
      // param/result types, as otherwise we might end up with a bad generic
      // signature if there are UnresolvedTypes present in the base type. Note
      // we pass in DesugarMemberTypes so that we see the actual concrete type
      // witnesses instead of type alias types.
      if (auto *GFT = T->getAs<GenericFunctionType>()) {
        T = GFT->substGenericArgs(Subs, SubstFlags::DesugarMemberTypes);
      } else {
        T = T.subst(Subs, SubstFlags::DesugarMemberTypes);
      }
    }
  }

  return T;
}

Type CompletionLookup::getAssociatedTypeType(const AssociatedTypeDecl *ATD) {
  Type BaseTy = BaseType;
  if (!BaseTy)
    BaseTy = ExprType;
  if (!BaseTy && CurrDeclContext)
    BaseTy =
        CurrDeclContext->getInnermostTypeContext()->getDeclaredTypeInContext();
  if (BaseTy) {
    BaseTy = BaseTy->getInOutObjectType()->getMetatypeInstanceType();
    if (auto NTD = BaseTy->getAnyNominal()) {
      auto *Module = NTD->getParentModule();
      auto Conformance = Module->lookupConformance(BaseTy, ATD->getProtocol());
      if (Conformance.isConcrete()) {
        return Conformance.getConcrete()->getTypeWitness(
            const_cast<AssociatedTypeDecl *>(ATD));
      }
    }
  }
  return Type();
}

void CompletionLookup::analyzeActorIsolation(
    const ValueDecl *VD, Type T, bool &implicitlyAsync,
    Optional<ContextualNotRecommendedReason> &NotRecommended) {
  auto isolation = getActorIsolation(const_cast<ValueDecl *>(VD));

  switch (isolation.getKind()) {
  case ActorIsolation::ActorInstance: {
    // TODO: implicitlyThrowing here for distributed
    if (IsCrossActorReference) {
      implicitlyAsync = true;
      // TODO: 'NotRecommended' if this is a r-value reference.
    }
    break;
  }
  case ActorIsolation::GlobalActorUnsafe:
    // For "unsafe" global actor isolation, automatic 'async' only happens
    // if the context has adopted concurrency.
    if (!CanCurrDeclContextHandleAsync &&
        !completionContextUsesConcurrencyFeatures(CurrDeclContext)) {
      return;
    }
    LLVM_FALLTHROUGH;
  case ActorIsolation::GlobalActor: {
    auto getClosureActorIsolation = [this](AbstractClosureExpr *CE) {
      // Prefer solution-specific actor-isolations and fall back to the one
      // recorded in the AST.
      auto isolation = ClosureActorIsolations.find(CE);
      if (isolation != ClosureActorIsolations.end()) {
        return isolation->second;
      } else {
        return CE->getActorIsolation();
      }
    };
    auto contextIsolation = getActorIsolationOfContext(
        const_cast<DeclContext *>(CurrDeclContext), getClosureActorIsolation);
    if (contextIsolation != isolation) {
      implicitlyAsync = true;
    }
    break;
  }
  case ActorIsolation::Unspecified:
  case ActorIsolation::Independent:
    return;
  }

  // If the reference is 'async', all types must be 'Sendable'.
  if (Ctx.LangOpts.StrictConcurrencyLevel >= StrictConcurrency::Complete &&
      implicitlyAsync && T) {
    auto *M = CurrDeclContext->getParentModule();
    if (isa<VarDecl>(VD)) {
      if (!isSendableType(M, T)) {
        NotRecommended = ContextualNotRecommendedReason::CrossActorReference;
      }
    } else {
      assert(isa<FuncDecl>(VD) || isa<SubscriptDecl>(VD));
      // Check if the result and the param types are all 'Sendable'.
      auto *AFT = T->castTo<AnyFunctionType>();
      if (!isSendableType(M, AFT->getResult())) {
        NotRecommended = ContextualNotRecommendedReason::CrossActorReference;
      } else {
        for (auto &param : AFT->getParams()) {
          Type paramType = param.getPlainType();
          if (!isSendableType(M, paramType)) {
            NotRecommended =
                ContextualNotRecommendedReason::CrossActorReference;
            break;
          }
        }
      }
    }
  }
}

void CompletionLookup::addVarDeclRef(const VarDecl *VD,
                                     DeclVisibilityKind Reason,
                                     DynamicLookupInfo dynamicLookupInfo) {
  if (!VD->hasName())
    return;

  const Identifier Name = VD->getName();
  assert(!Name.empty() && "name should not be empty");

  Type VarType;
  auto SolutionSpecificType = SolutionSpecificVarTypes.find(VD);
  if (SolutionSpecificType != SolutionSpecificVarTypes.end()) {
    assert(!VarType && "Type recorded in the AST and is also solution-specific?");
    VarType = SolutionSpecificType->second;
  } else if (VD->hasInterfaceType()) {
    VarType = getTypeOfMember(VD, dynamicLookupInfo);
  }

  Optional<ContextualNotRecommendedReason> NotRecommended;
  // "not recommended" in its own getter.
  if (Kind == LookupKind::ValueInDeclContext) {
    if (auto accessor = dyn_cast<AccessorDecl>(CurrDeclContext)) {
      if (accessor->getStorage() == VD && accessor->isGetter())
        NotRecommended =
            ContextualNotRecommendedReason::VariableUsedInOwnDefinition;
    }
  }
  bool implicitlyAsync = false;
  analyzeActorIsolation(VD, VarType, implicitlyAsync, NotRecommended);
  bool explicitlyAsync = false;
  if (auto accessor = VD->getEffectfulGetAccessor()) {
    explicitlyAsync = accessor->hasAsync();
  }
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(VD, Reason, dynamicLookupInfo));
  Builder.setIsAsync(explicitlyAsync || implicitlyAsync);
  Builder.setCanCurrDeclContextHandleAsync(CanCurrDeclContextHandleAsync);
  Builder.setAssociatedDecl(VD);
  addLeadingDot(Builder);
  addValueBaseName(Builder, Name);

  if (NotRecommended)
    Builder.setContextualNotRecommended(*NotRecommended);

  if (!VarType)
    return;

  if (auto *PD = dyn_cast<ParamDecl>(VD)) {
    if (Name != Ctx.Id_self && PD->isInOut()) {
      // It is useful to show inout for function parameters.
      // But for 'self' it is just noise.
      VarType = InOutType::get(VarType);
    }
  }
  auto DynamicOrOptional =
      IsDynamicLookup || VD->getAttrs().hasAttribute<OptionalAttr>();
  if (DynamicOrOptional) {
    // Values of properties that were found on a AnyObject have
    // Optional<T> type.  Same applies to optional members.
    VarType = OptionalType::get(VarType);
  }

  auto genericSig =
      VD->getInnermostDeclContext()->getGenericSignatureOfContext();
  if (VD->isImplicitlyUnwrappedOptional())
    addTypeAnnotationForImplicitlyUnwrappedOptional(
        Builder, VarType, genericSig, DynamicOrOptional);
  else
    addTypeAnnotation(Builder, VarType, genericSig);

  if (implicitlyAsync)
    Builder.addAnnotatedAsync();

  if (isUnresolvedMemberIdealType(VarType))
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);

  if (auto Accessor = VD->getEffectfulGetAccessor()) {
    if (auto AFT = getTypeOfMember(Accessor, dynamicLookupInfo)->getAs<AnyFunctionType>()) {
      if (Accessor->hasImplicitSelfDecl()) {
        AFT = AFT->getResult()->getAs<AnyFunctionType>();
        assert(AFT);
      }
      addEffectsSpecifiers(Builder, AFT, Accessor);
    }
  }
}

/// Return whether \p param has a non-desirable default value for code
/// completion.
///
/// 'ClangImporter::Implementation::inferDefaultArgument()' automatically adds
/// default values for some parameters;
///   * NS_OPTIONS enum type with the name '...Options'.
///   * NSDictionary and labeled 'options', 'attributes', or 'userInfo'.
///
/// But sometimes, this behavior isn't really desirable. This function add a
/// heuristic where if a parameter matches all the following condition, we
/// consider the imported default value is _not_ desirable:
///   * it is the first parameter,
///   * it doesn't have an argument label, and
///   * the imported function base name ends with those words
/// For example, ClangImporter imports:
///
///   -(void)addAttributes:(NSDictionary *)attrs, options:(NSDictionary *)opts;
///
/// as:
///
///   func addAttributes(_ attrs: [AnyHashable:Any] = [:],
///                      options opts: [AnyHashable:Any] = [:])
///
/// In this case, we don't want 'attrs' defaulted because the function name have
/// 'Attribute' in its name so calling 'value.addAttribute()' doesn't make
/// sense, but we _do_ want to keep 'opts' defaulted.
///
/// Note that:
///
///   -(void)performWithOptions:(NSDictionary *) opts;
///
/// This doesn't match the condition because the base name of the function in
/// Swift is 'peform':
///
///   func perform(options opts: [AnyHashable:Any] = [:])
///
bool isNonDesirableImportedDefaultArg(const ParamDecl *param) {
  auto kind = param->getDefaultArgumentKind();
  if (kind != DefaultArgumentKind::EmptyArray &&
      kind != DefaultArgumentKind::EmptyDictionary)
    return false;

  if (!param->getArgumentName().empty())
    return false;

  auto *func = dyn_cast<FuncDecl>(param->getDeclContext());
  if (!func->hasClangNode())
    return false;
  if (func->getParameters()->front() != param)
    return false;
  if (func->getBaseName().isSpecial())
    return false;

  auto baseName = func->getBaseName().getIdentifier().str();
  switch (kind) {
  case DefaultArgumentKind::EmptyArray:
    return (baseName.endswith("Options"));
  case DefaultArgumentKind::EmptyDictionary:
    return (baseName.endswith("Options") || baseName.endswith("Attributes") ||
            baseName.endswith("UserInfo"));
  default:
    llvm_unreachable("unhandled DefaultArgumentKind");
  }
}

bool CompletionLookup::hasInterestingDefaultValue(const ParamDecl *param) {
  if (!param)
    return false;

  switch (param->getDefaultArgumentKind()) {
  case DefaultArgumentKind::Normal:
  case DefaultArgumentKind::NilLiteral:
  case DefaultArgumentKind::StoredProperty:
  case DefaultArgumentKind::Inherited:
    return true;

  case DefaultArgumentKind::EmptyArray:
  case DefaultArgumentKind::EmptyDictionary:
    if (isNonDesirableImportedDefaultArg(param))
      return false;
    return true;

  case DefaultArgumentKind::None:
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND)                            \
  case DefaultArgumentKind::NAME:
#include "swift/AST/MagicIdentifierKinds.def"
    return false;
  }
}

bool CompletionLookup::shouldAddItemWithoutDefaultArgs(
    const AbstractFunctionDecl *func) {
  if (!func || !Sink.addCallWithNoDefaultArgs)
    return false;
  for (auto param : *func->getParameters()) {
    if (hasInterestingDefaultValue(param))
      return true;
  }
  return false;
}

bool CompletionLookup::addCallArgumentPatterns(
    CodeCompletionResultBuilder &Builder,
    ArrayRef<AnyFunctionType::Param> typeParams,
    ArrayRef<const ParamDecl *> declParams, GenericSignature genericSig,
    bool includeDefaultArgs) {
  assert(declParams.empty() || typeParams.size() == declParams.size());

  bool modifiedBuilder = false;
  bool needComma = false;
  // Iterate over each parameter.
  for (unsigned i = 0; i != typeParams.size(); ++i) {
    auto &typeParam = typeParams[i];

    Identifier argName = typeParam.getLabel();
    Identifier bodyName;
    bool isIUO = false;
    bool hasDefault = false;
    if (!declParams.empty()) {
      const ParamDecl *PD = declParams[i];
      hasDefault =
          PD->isDefaultArgument() && !isNonDesirableImportedDefaultArg(PD);
      // Skip default arguments if we're either not including them or they
      // aren't interesting
      if (hasDefault &&
          (!includeDefaultArgs || !hasInterestingDefaultValue(PD)))
        continue;

      argName = PD->getArgumentName();
      bodyName = PD->getParameterName();
      isIUO = PD->isImplicitlyUnwrappedOptional();
    }

    bool isVariadic = typeParam.isVariadic();
    bool isInOut = typeParam.isInOut();
    bool isAutoclosure = typeParam.isAutoClosure();
    Type paramTy = typeParam.getPlainType();
    if (isVariadic)
      paramTy = ParamDecl::getVarargBaseTy(paramTy);

    Type contextTy;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      contextTy = typeContext->getDeclaredTypeInContext();

    if (needComma)
      Builder.addComma();
    Builder.addCallArgument(argName, bodyName,
                            eraseArchetypes(paramTy, genericSig), contextTy,
                            isVariadic, isInOut, isIUO, isAutoclosure,
                            /*UseUnderscoreLabel=*/false,
                            /*IsLabeledTrailingClosure=*/false, hasDefault);

    modifiedBuilder = true;
    needComma = true;
  }

  return modifiedBuilder;
}

bool CompletionLookup::addCallArgumentPatterns(
    CodeCompletionResultBuilder &Builder, const AnyFunctionType *AFT,
    const ParameterList *Params, GenericSignature genericSig,
    bool includeDefaultArgs) {
  ArrayRef<const ParamDecl *> declParams;
  if (Params)
    declParams = Params->getArray();
  return addCallArgumentPatterns(Builder, AFT->getParams(), declParams,
                                 genericSig, includeDefaultArgs);
}

void CompletionLookup::addEffectsSpecifiers(
    CodeCompletionResultBuilder &Builder, const AnyFunctionType *AFT,
    const AbstractFunctionDecl *AFD, bool forceAsync) {
  assert(AFT != nullptr);

  // 'async'.
  if (forceAsync || (AFD && AFD->hasAsync()) ||
      (AFT->hasExtInfo() && AFT->isAsync()))
    Builder.addAnnotatedAsync();

  // 'throws' or 'rethrows'.
  if (AFD && AFD->getAttrs().hasAttribute<RethrowsAttr>())
    Builder.addAnnotatedRethrows();
  else if (AFT->hasExtInfo() && AFT->isThrowing())
    Builder.addAnnotatedThrows();
}

void CompletionLookup::addPoundAvailable(Optional<StmtKind> ParentKind) {
  if (ParentKind != StmtKind::If && ParentKind != StmtKind::Guard)
    return;
  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword,
      // FIXME: SemanticContextKind::Local is not correct.
      // Use 'None' (and fix prioritization) or introduce a new context.
      SemanticContextKind::Local);
  Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
  Builder.addBaseName("available");
  Builder.addLeftParen();
  Builder.addSimpleTypedParameter("Platform", /*IsVarArg=*/true);
  Builder.addComma();
  Builder.addTextChunk("*");
  Builder.addRightParen();
}

void CompletionLookup::addPoundSelector(bool needPound) {
  // #selector is only available when the Objective-C runtime is.
  if (!Ctx.LangOpts.EnableObjCInterop)
    return;

  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::None);
  if (needPound)
    Builder.addTextChunk("#selector");
  else
    Builder.addTextChunk("selector");
  Builder.addLeftParen();
  Builder.addSimpleTypedParameter("@objc method", /*IsVarArg=*/false);
  Builder.addRightParen();
  Builder.addTypeAnnotation("Selector");
  // This function is called only if the context type is 'Selector'.
  Builder.setResultTypes(Ctx.getSelectorType());
  Builder.setTypeContext(expectedTypeContext, CurrDeclContext);
}

void CompletionLookup::addPoundKeyPath(bool needPound) {
  // #keyPath is only available when the Objective-C runtime is.
  if (!Ctx.LangOpts.EnableObjCInterop)
    return;

  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::None);
  if (needPound)
    Builder.addTextChunk("#keyPath");
  else
    Builder.addTextChunk("keyPath");
  Builder.addLeftParen();
  Builder.addSimpleTypedParameter("@objc property sequence",
                                  /*IsVarArg=*/false);
  Builder.addRightParen();
  Builder.addTypeAnnotation("String");
  Builder.setResultTypes(Ctx.getStringType());
  Builder.setTypeContext(expectedTypeContext, CurrDeclContext);
}

SemanticContextKind
CompletionLookup::getSemanticContextKind(const ValueDecl *VD) {
  // FIXME: to get the corect semantic context we need to know how lookup
  // would have found the VD. For now, just infer a reasonable semantics.

  if (!VD)
    return SemanticContextKind::CurrentModule;

  DeclContext *calleeDC = VD->getDeclContext();

  if (calleeDC->isTypeContext())
    // FIXME: We should distinguish CurrentNominal and Super. We need to
    // propagate the base type to do that.
    return SemanticContextKind::CurrentNominal;

  if (calleeDC->isLocalContext())
    return SemanticContextKind::Local;
  if (calleeDC->getParentModule() == CurrModule)
    return SemanticContextKind::CurrentModule;

  return SemanticContextKind::OtherModule;
}

void CompletionLookup::addSubscriptCallPattern(
    const AnyFunctionType *AFT, const SubscriptDecl *SD,
    const Optional<SemanticContextKind> SemanticContext) {
  foundFunction(AFT);
  GenericSignature genericSig;
  if (SD)
    genericSig = SD->getGenericSignatureOfContext();

  CodeCompletionResultBuilder Builder = makeResultBuilder(
      SD ? CodeCompletionResultKind::Declaration
         : CodeCompletionResultKind::Pattern,
      SemanticContext ? *SemanticContext : getSemanticContextKind(SD));
  if (SD)
    Builder.setAssociatedDecl(SD);
  if (!HaveLParen) {
    Builder.addLeftBracket();
  } else {
    // Add 'ArgumentLabels' only if it has '['. Without existing '[',
    // consider it suggesting 'subscript' itself, not call arguments for it.
    Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
    Builder.addAnnotatedLeftBracket();
  }
  ArrayRef<const ParamDecl *> declParams;
  if (SD)
    declParams = SD->getIndices()->getArray();
  addCallArgumentPatterns(Builder, AFT->getParams(), declParams, genericSig);
  if (!HaveLParen)
    Builder.addRightBracket();
  else
    Builder.addAnnotatedRightBracket();
  if (SD && SD->isImplicitlyUnwrappedOptional())
    addTypeAnnotationForImplicitlyUnwrappedOptional(Builder, AFT->getResult(),
                                                    genericSig);
  else
    addTypeAnnotation(Builder, AFT->getResult(), genericSig);
}

void CompletionLookup::addFunctionCallPattern(
    const AnyFunctionType *AFT, const AbstractFunctionDecl *AFD,
    const Optional<SemanticContextKind> SemanticContext) {
  GenericSignature genericSig;
  if (AFD)
    genericSig = AFD->getGenericSignatureOfContext();

  // Add the pattern, possibly including any default arguments.
  auto addPattern = [&](ArrayRef<const ParamDecl *> declParams = {},
                        bool includeDefaultArgs = true) {
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        AFD ? CodeCompletionResultKind::Declaration
            : CodeCompletionResultKind::Pattern,
        SemanticContext ? *SemanticContext : getSemanticContextKind(AFD));
    Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
    if (AFD)
      Builder.setAssociatedDecl(AFD);

    if (!HaveLParen)
      Builder.addLeftParen();
    else
      Builder.addAnnotatedLeftParen();

    addCallArgumentPatterns(Builder, AFT->getParams(), declParams, genericSig,
                            includeDefaultArgs);

    // The rparen matches the lparen here so that we insert both or neither.
    if (!HaveLParen)
      Builder.addRightParen();
    else
      Builder.addAnnotatedRightParen();

    addEffectsSpecifiers(Builder, AFT, AFD);

    if (AFD && AFD->isImplicitlyUnwrappedOptional())
      addTypeAnnotationForImplicitlyUnwrappedOptional(Builder, AFT->getResult(),
                                                      genericSig);
    else
      addTypeAnnotation(Builder, AFT->getResult(), genericSig);

    Builder.setIsAsync(AFT->hasExtInfo() && AFT->isAsync());
    Builder.setCanCurrDeclContextHandleAsync(CanCurrDeclContextHandleAsync);
  };

  if (!AFD || !AFD->getInterfaceType()->is<AnyFunctionType>()) {
    // Probably, calling closure type expression.
    foundFunction(AFT);
    addPattern();
    return;
  } else {
    // Calling function or method.
    foundFunction(AFD);

    // FIXME: Hack because we don't know we are calling instance
    // method or not. There's invariant that funcTy is derived from AFD.
    // Only if we are calling instance method on meta type, AFT is still
    // curried. So we should be able to detect that by comparing curried level
    // of AFT and the interface type of AFD.
    auto getCurriedLevel = [](const AnyFunctionType *funcTy) -> unsigned {
      unsigned level = 0;
      while ((funcTy = funcTy->getResult()->getAs<AnyFunctionType>()))
        ++level;
      return level;
    };
    bool isImplicitlyCurriedInstanceMethod =
        (AFD->hasImplicitSelfDecl() &&
         getCurriedLevel(AFT) ==
             getCurriedLevel(
                 AFD->getInterfaceType()->castTo<AnyFunctionType>()) &&
         // NOTE: shouldn't be necessary, but just in case curried level check
         // is insufficient.
         AFT->getParams().size() == 1 &&
         AFT->getParams()[0].getLabel().empty());

    if (isImplicitlyCurriedInstanceMethod) {
      addPattern({AFD->getImplicitSelfDecl()}, /*includeDefaultArgs=*/true);
    } else {
      if (shouldAddItemWithoutDefaultArgs(AFD))
        addPattern(AFD->getParameters()->getArray(),
                   /*includeDefaultArgs=*/false);
      addPattern(AFD->getParameters()->getArray(),
                 /*includeDefaultArgs=*/true);
    }
  }
}

bool CompletionLookup::isImplicitlyCurriedInstanceMethod(
    const AbstractFunctionDecl *FD) {
  if (FD->isStatic())
    return false;

  switch (Kind) {
  case LookupKind::ValueExpr:
    return ExprType->is<AnyMetatypeType>();
  case LookupKind::ValueInDeclContext:
    if (InsideStaticMethod)
      return FD->getDeclContext() == CurrentMethod->getDeclContext();
    if (auto Init = dyn_cast<Initializer>(CurrDeclContext)) {
      if (auto PatInit = dyn_cast<PatternBindingInitializer>(Init)) {
        if (PatInit->getInitializedLazyVar())
          return false;
      }
      return FD->getDeclContext() == Init->getInnermostTypeContext();
    }
    return false;
  case LookupKind::EnumElement:
  case LookupKind::Type:
  case LookupKind::TypeInDeclContext:
  case LookupKind::GenericRequirement:
    llvm_unreachable("cannot have a method call while doing a "
                     "type completion");
  case LookupKind::ImportFromModule:
    return false;
  }

  llvm_unreachable("Unhandled LookupKind in switch.");
}

void CompletionLookup::addMethodCall(const FuncDecl *FD,
                                     DeclVisibilityKind Reason,
                                     DynamicLookupInfo dynamicLookupInfo) {
  if (FD->getBaseIdentifier().empty())
    return;
  foundFunction(FD);

  const Identifier Name = FD->getBaseIdentifier();
  assert(!Name.empty() && "name should not be empty");

  Type FunctionType = getTypeOfMember(FD, dynamicLookupInfo);
  assert(FunctionType);

  auto AFT = FunctionType->getAs<AnyFunctionType>();

  bool IsImplicitlyCurriedInstanceMethod = false;
  if (FD->hasImplicitSelfDecl()) {
    IsImplicitlyCurriedInstanceMethod = isImplicitlyCurriedInstanceMethod(FD);

    // Strip off '(_ self: Self)' if needed.
    if (AFT && !IsImplicitlyCurriedInstanceMethod) {
      AFT = AFT->getResult()->getAs<AnyFunctionType>();

      // Check for duplicates with the adjusted type too.
      if (isDuplicate(FD, AFT))
        return;
    }
  }

  bool trivialTrailingClosure = false;
  if (AFT && !IsImplicitlyCurriedInstanceMethod)
    trivialTrailingClosure = hasTrivialTrailingClosure(FD, AFT);

  Optional<ContextualNotRecommendedReason> NotRecommended;
  bool implictlyAsync = false;
  analyzeActorIsolation(FD, AFT, implictlyAsync, NotRecommended);

  // Add the method, possibly including any default arguments.
  auto addMethodImpl = [&](bool includeDefaultArgs = true,
                           bool trivialTrailingClosure = false) {
    CodeCompletionResultBuilder Builder =
        makeResultBuilder(CodeCompletionResultKind::Declaration,
                          getSemanticContext(FD, Reason, dynamicLookupInfo));
    Builder.setIsAsync(implictlyAsync || (AFT->hasExtInfo() && AFT->isAsync()));
    Builder.setHasAsyncAlternative(
        FD->getAsyncAlternative() &&
        !FD->getAsyncAlternative()->shouldHideFromEditor());
    Builder.setCanCurrDeclContextHandleAsync(CanCurrDeclContextHandleAsync);
    Builder.setAssociatedDecl(FD);

    if (IsSuperRefExpr && CurrentMethod &&
        CurrentMethod->getOverriddenDecl() == FD)
      Builder.addFlair(CodeCompletionFlairBit::SuperChain);

    if (NotRecommended)
      Builder.setContextualNotRecommended(*NotRecommended);

    addLeadingDot(Builder);
    addValueBaseName(Builder, Name);
    if (IsDynamicLookup)
      Builder.addDynamicLookupMethodCallTail();
    else if (FD->getAttrs().hasAttribute<OptionalAttr>())
      Builder.addOptionalMethodCallTail();

    if (!AFT) {
      addTypeAnnotation(Builder, FunctionType,
                        FD->getGenericSignatureOfContext());
      return;
    }
    if (IsImplicitlyCurriedInstanceMethod) {
      Builder.addLeftParen();
      addCallArgumentPatterns(
          Builder, AFT->getParams(), {FD->getImplicitSelfDecl()},
          FD->getGenericSignatureOfContext(), includeDefaultArgs);
      Builder.addRightParen();
    } else if (trivialTrailingClosure) {
      Builder.addBraceStmtWithCursor(" { code }");
      addEffectsSpecifiers(Builder, AFT, FD, implictlyAsync);
    } else {
      Builder.addLeftParen();
      addCallArgumentPatterns(Builder, AFT, FD->getParameters(),
                              FD->getGenericSignatureOfContext(),
                              includeDefaultArgs);
      Builder.addRightParen();
      addEffectsSpecifiers(Builder, AFT, FD, implictlyAsync);
    }

    // Build type annotation.
    Type ResultType = AFT->getResult();
    // As we did with parameters in addParamPatternFromFunction,
    // for regular methods we'll print '!' after implicitly
    // unwrapped optional results.
    bool IsIUO = !IsImplicitlyCurriedInstanceMethod &&
                 FD->isImplicitlyUnwrappedOptional();

    PrintOptions PO;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    PO.PrintOptionalAsImplicitlyUnwrapped = IsIUO;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      PO.setBaseType(typeContext->getDeclaredTypeInContext());
    Type AnnotationTy =
        eraseArchetypes(ResultType, FD->getGenericSignatureOfContext());
    if (Builder.shouldAnnotateResults()) {
      Builder.withNestedGroup(
          CodeCompletionString::Chunk::ChunkKind::TypeAnnotationBegin, [&] {
            CodeCompletionStringPrinter printer(Builder);
            auto TL = TypeLoc::withoutLoc(AnnotationTy);
            printer.printTypePre(TL);
            if (IsImplicitlyCurriedInstanceMethod) {
              auto *FnType = AnnotationTy->castTo<AnyFunctionType>();
              AnyFunctionType::printParams(FnType->getParams(), printer,
                                           PrintOptions());
              AnnotationTy = FnType->getResult();
              printer.printText(" -> ");
            }

            // What's left is the result type.
            if (AnnotationTy->isVoid())
              AnnotationTy = Ctx.getVoidDecl()->getDeclaredInterfaceType();
            AnnotationTy.print(printer, PO);
            printer.printTypePost(TL);
          });
    } else {
      llvm::SmallString<32> TypeStr;
      llvm::raw_svector_ostream OS(TypeStr);
      if (IsImplicitlyCurriedInstanceMethod) {
        auto *FnType = AnnotationTy->castTo<AnyFunctionType>();
        AnyFunctionType::printParams(FnType->getParams(), OS);
        AnnotationTy = FnType->getResult();
        OS << " -> ";
      }

      // What's left is the result type.
      if (AnnotationTy->isVoid())
        AnnotationTy = Ctx.getVoidDecl()->getDeclaredInterfaceType();
      AnnotationTy.print(OS, PO);
      Builder.addTypeAnnotation(TypeStr);
    }

    Builder.setResultTypes(ResultType);
    Builder.setTypeContext(expectedTypeContext, CurrDeclContext);

    if (isUnresolvedMemberIdealType(ResultType))
      Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
  };

  // Do not add imported C++ methods that are treated as unsafe in Swift.
  if (Importer->isUnsafeCXXMethod(FD))
    return;

  if (!AFT || IsImplicitlyCurriedInstanceMethod) {
    addMethodImpl();
  } else {
    if (trivialTrailingClosure)
      addMethodImpl(/*includeDefaultArgs=*/false,
                    /*trivialTrailingClosure=*/true);
    if (shouldAddItemWithoutDefaultArgs(FD))
      addMethodImpl(/*includeDefaultArgs=*/false);
    addMethodImpl(/*includeDefaultArgs=*/true);
  }
}

void CompletionLookup::addConstructorCall(const ConstructorDecl *CD,
                                          DeclVisibilityKind Reason,
                                          DynamicLookupInfo dynamicLookupInfo,
                                          Optional<Type> BaseType,
                                          Optional<Type> Result, bool IsOnType,
                                          Identifier addName) {
  foundFunction(CD);
  Type MemberType = getTypeOfMember(CD, BaseType.value_or(ExprType));
  AnyFunctionType *ConstructorType = nullptr;
  if (auto MemberFuncType = MemberType->getAs<AnyFunctionType>())
    ConstructorType = MemberFuncType->getResult()->castTo<AnyFunctionType>();

  bool needInit = false;
  if (!IsOnType) {
    assert(addName.empty());
    needInit = true;
  } else if (addName.empty() && HaveDot) {
    needInit = true;
  }

  // If we won't be able to provide a result, bail out.
  if (!ConstructorType && addName.empty() && !needInit)
    return;

  // Add the constructor, possibly including any default arguments.
  auto addConstructorImpl = [&](bool includeDefaultArgs = true) {
    CodeCompletionResultBuilder Builder =
        makeResultBuilder(CodeCompletionResultKind::Declaration,
                          getSemanticContext(CD, Reason, dynamicLookupInfo));
    Builder.setAssociatedDecl(CD);

    if (IsSuperRefExpr && CurrentMethod &&
        CurrentMethod->getOverriddenDecl() == CD)
      Builder.addFlair(CodeCompletionFlairBit::SuperChain);

    if (needInit) {
      assert(addName.empty());
      addLeadingDot(Builder);
      Builder.addBaseName("init");
    } else if (!addName.empty()) {
      Builder.addBaseName(addName.str());
    } else {
      Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
    }

    if (!ConstructorType) {
      addTypeAnnotation(Builder, MemberType,
                        CD->getGenericSignatureOfContext());
      return;
    }

    if (!HaveLParen)
      Builder.addLeftParen();
    else
      Builder.addAnnotatedLeftParen();

    addCallArgumentPatterns(Builder, ConstructorType, CD->getParameters(),
                            CD->getGenericSignatureOfContext(),
                            includeDefaultArgs);

    // The rparen matches the lparen here so that we insert both or neither.
    if (!HaveLParen)
      Builder.addRightParen();
    else
      Builder.addAnnotatedRightParen();

    addEffectsSpecifiers(Builder, ConstructorType, CD);

    if (!Result.has_value())
      Result = ConstructorType->getResult();
    if (CD->isImplicitlyUnwrappedOptional()) {
      addTypeAnnotationForImplicitlyUnwrappedOptional(
          Builder, *Result, CD->getGenericSignatureOfContext());
    } else {
      addTypeAnnotation(Builder, *Result, CD->getGenericSignatureOfContext());
    }

    Builder.setIsAsync(ConstructorType->hasExtInfo() &&
                       ConstructorType->isAsync());
    Builder.setCanCurrDeclContextHandleAsync(CanCurrDeclContextHandleAsync);
  };

  if (ConstructorType && shouldAddItemWithoutDefaultArgs(CD))
    addConstructorImpl(/*includeDefaultArgs=*/false);
  addConstructorImpl();
}

void CompletionLookup::addConstructorCallsForType(
    Type type, Identifier name, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  if (!Sink.addInitsToTopLevel)
    return;

  // Existential types cannot be instantiated. e.g. 'MyProtocol()'.
  if (type->isExistentialType())
    return;

  // 'AnyObject' is not initializable.
  // FIXME: Should we do this in 'AnyObjectLookupRequest'?
  if (type->isAnyObject())
    return;

  assert(CurrDeclContext);

  auto results =
      swift::lookupSemanticMember(const_cast<DeclContext *>(CurrDeclContext),
                                  type, DeclBaseName::createConstructor());
  for (const auto &entry : results.allResults()) {
    auto *init = cast<ConstructorDecl>(entry.getValueDecl());
    if (init->shouldHideFromEditor())
      continue;
    addConstructorCall(cast<ConstructorDecl>(init), Reason, dynamicLookupInfo,
                       type, None,
                       /*IsOnType=*/true, name);
  }
}

void CompletionLookup::addSubscriptCall(const SubscriptDecl *SD,
                                        DeclVisibilityKind Reason,
                                        DynamicLookupInfo dynamicLookupInfo) {
  // Don't add subscript call to unqualified completion.
  if (!ExprType)
    return;

  // Subscript after '.' is valid only after type part of Swift keypath
  // expression. (e.g. '\TyName.SubTy.[0])
  if (HaveDot && !IsAfterSwiftKeyPathRoot)
    return;

  auto subscriptType =
      getTypeOfMember(SD, dynamicLookupInfo)->getAs<AnyFunctionType>();
  if (!subscriptType)
    return;

  Optional<ContextualNotRecommendedReason> NotRecommended;
  bool implictlyAsync = false;
  analyzeActorIsolation(SD, subscriptType, implictlyAsync, NotRecommended);

  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(SD, Reason, dynamicLookupInfo));
  Builder.setIsAsync(implictlyAsync);
  Builder.setCanCurrDeclContextHandleAsync(CanCurrDeclContextHandleAsync);
  Builder.setAssociatedDecl(SD);

  if (NotRecommended)
    Builder.setContextualNotRecommended(*NotRecommended);

  // '\TyName#^TOKEN^#' requires leading dot.
  if (!HaveDot && IsAfterSwiftKeyPathRoot)
    Builder.addLeadingDot();

  if (NeedOptionalUnwrap) {
    Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
    Builder.addQuestionMark();
  }

  Builder.addLeftBracket();
  addCallArgumentPatterns(Builder, subscriptType, SD->getIndices(),
                          SD->getGenericSignatureOfContext(), true);
  Builder.addRightBracket();

  // Add a type annotation.
  Type resultTy = subscriptType->getResult();
  if (IsDynamicLookup) {
    // Values of properties that were found on a AnyObject have
    // Optional<T> type.
    resultTy = OptionalType::get(resultTy);
  }

  if (implictlyAsync)
    Builder.addAnnotatedAsync();

  addTypeAnnotation(Builder, resultTy, SD->getGenericSignatureOfContext());
}

void CompletionLookup::addNominalTypeRef(const NominalTypeDecl *NTD,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(NTD, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(NTD);
  addLeadingDot(Builder);
  Builder.addBaseName(NTD->getName().str());

  addTypeAnnotation(Builder, NTD->getDeclaredType());

  // Override the type relation for NominalTypes. Use the better relation
  // for the metatypes and the instance type. For example,
  //
  //   func receiveInstance(_: Int) {}
  //   func receiveMetatype(_: Int.Type) {}
  //
  // We want to suggest 'Int' as 'Identical' for both arguments.
  Builder.setResultTypes(
      {NTD->getInterfaceType(), NTD->getDeclaredInterfaceType()});
  Builder.setTypeContext(expectedTypeContext, CurrDeclContext);
}

void CompletionLookup::addTypeAliasRef(const TypeAliasDecl *TAD,
                                       DeclVisibilityKind Reason,
                                       DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(TAD, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(TAD);
  addLeadingDot(Builder);
  Builder.addBaseName(TAD->getName().str());
  if (auto underlyingType = TAD->getUnderlyingType()) {
    if (underlyingType->hasError()) {
      addTypeAnnotation(Builder,
                        TAD->isGeneric()
                        ? TAD->getUnboundGenericType()
                        : TAD->getDeclaredInterfaceType());

    } else {
      addTypeAnnotation(Builder, underlyingType);
    }
  }
}

void CompletionLookup::addGenericTypeParamRef(
    const GenericTypeParamDecl *GP, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  assert(!GP->getName().empty());
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(GP, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(GP);
  addLeadingDot(Builder);
  Builder.addBaseName(GP->getName().str());
  addTypeAnnotation(Builder, GP->getDeclaredInterfaceType());
}

void CompletionLookup::addAssociatedTypeRef(
    const AssociatedTypeDecl *AT, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(AT, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(AT);
  addLeadingDot(Builder);
  Builder.addBaseName(AT->getName().str());
  if (Type T = getAssociatedTypeType(AT))
    addTypeAnnotation(Builder, T);
}

void CompletionLookup::addPrecedenceGroupRef(PrecedenceGroupDecl *PGD) {
  auto semanticContext =
      getSemanticContext(PGD, DeclVisibilityKind::VisibleAtTopLevel, {});
  CodeCompletionResultBuilder builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration, semanticContext);

  builder.addBaseName(PGD->getName().str());
  builder.setAssociatedDecl(PGD);
}

void CompletionLookup::addEnumElementRef(const EnumElementDecl *EED,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo,
                                         bool HasTypeContext) {
  if (!EED->hasName() || !EED->isAccessibleFrom(CurrDeclContext) ||
      EED->shouldHideFromEditor())
    return;

  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(EED, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(EED);

  addLeadingDot(Builder);
  addValueBaseName(Builder, EED->getBaseIdentifier());

  // Enum element is of function type; (Self.type) -> Self or
  // (Self.Type) -> (Args...) -> Self.
  Type EnumType = getTypeOfMember(EED, dynamicLookupInfo);
  if (EnumType->is<AnyFunctionType>())
    EnumType = EnumType->castTo<AnyFunctionType>()->getResult();

  if (EnumType->is<FunctionType>()) {
    Builder.addLeftParen();
    addCallArgumentPatterns(Builder, EnumType->castTo<FunctionType>(),
                            EED->getParameterList(),
                            EED->getGenericSignatureOfContext());
    Builder.addRightParen();

    // Extract result as the enum type.
    EnumType = EnumType->castTo<FunctionType>()->getResult();
  }

  addTypeAnnotation(Builder, EnumType, EED->getGenericSignatureOfContext());

  if (isUnresolvedMemberIdealType(EnumType))
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
}

void CompletionLookup::addMacroExpansion(const MacroDecl *MD,
                                         DeclVisibilityKind Reason) {
  if (!MD->hasName() || !MD->isAccessibleFrom(CurrDeclContext) ||
      MD->shouldHideFromEditor())
    return;

  OptionSet<CustomAttributeKind> expectedKinds =
      expectedTypeContext.getExpectedCustomAttributeKinds();
  if (expectedKinds) {
    CodeCompletionMacroRoles expectedRoles =
        getCompletionMacroRoles(expectedKinds);
    CodeCompletionMacroRoles roles = getCompletionMacroRoles(MD);
    if (!(roles & expectedRoles))
      return;
  }

  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(MD, Reason, DynamicLookupInfo()));
  Builder.setAssociatedDecl(MD);

  addValueBaseName(Builder, MD->getBaseIdentifier());

  Type macroType = MD->getInterfaceType();
  if (MD->parameterList && MD->parameterList->size() > 0) {
    Builder.addLeftParen();
    addCallArgumentPatterns(Builder, macroType->castTo<AnyFunctionType>(),
                            MD->parameterList,
                            MD->getGenericSignature());
    Builder.addRightParen();
  }

  if (!MD->getResultInterfaceType()->isVoid()) {
    addTypeAnnotation(Builder, MD->getResultInterfaceType(),
                      MD->getGenericSignature());
  }
}

void CompletionLookup::addKeyword(StringRef Name, Type TypeAnnotation,
                                  SemanticContextKind SK,
                                  CodeCompletionKeywordKind KeyKind,
                                  unsigned NumBytesToErase) {
  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Keyword, SK);
  addLeadingDot(Builder);
  Builder.addKeyword(Name);
  Builder.setKeywordKind(KeyKind);
  if (TypeAnnotation)
    addTypeAnnotation(Builder, TypeAnnotation);
  if (NumBytesToErase > 0)
    Builder.setNumBytesToErase(NumBytesToErase);
}

void CompletionLookup::addKeyword(StringRef Name, StringRef TypeAnnotation,
                                  CodeCompletionKeywordKind KeyKind,
                                  CodeCompletionFlair flair) {
  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::None);
  Builder.addFlair(flair);
  addLeadingDot(Builder);
  Builder.addKeyword(Name);
  Builder.setKeywordKind(KeyKind);
  if (!TypeAnnotation.empty())
    Builder.addTypeAnnotation(TypeAnnotation);
}

void CompletionLookup::addDeclAttrParamKeyword(StringRef Name,
                                               ArrayRef<StringRef> Parameters,
                                               StringRef Annotation,
                                               bool NeedSpecify) {
  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::None);
  Builder.addDeclAttrParamKeyword(Name, Parameters, Annotation, NeedSpecify);
}

void CompletionLookup::addDeclAttrKeyword(StringRef Name,
                                          StringRef Annotation) {
  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::None);
  Builder.addDeclAttrKeyword(Name, Annotation);
}

/// Add the compound function name for the given function.
/// Returns \c true if the compound function name is actually used.
bool CompletionLookup::addCompoundFunctionNameIfDesiable(
    AbstractFunctionDecl *AFD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  auto funcTy =
      getTypeOfMember(AFD, dynamicLookupInfo)->getAs<AnyFunctionType>();
  bool dropCurryLevel = funcTy && AFD->getDeclContext()->isTypeContext() &&
                        !isImplicitlyCurriedInstanceMethod(AFD);
  if (dropCurryLevel)
    funcTy = funcTy->getResult()->getAs<AnyFunctionType>();

  bool useFunctionReference = PreferFunctionReferencesToCalls;
  if (!useFunctionReference && funcTy) {
    // We know that the CodeCompletionResultType is AST-based so we can pass
    // nullptr for USRTypeContext.
    auto maxRel = CodeCompletionResultType(funcTy).calculateTypeRelation(
        &expectedTypeContext, CurrDeclContext, /*USRTypeContext=*/nullptr);
    useFunctionReference =
        maxRel >= CodeCompletionResultTypeRelation::Convertible;
  }
  if (!useFunctionReference)
    return false;

  // Check for duplicates with the adjusted type too.
  if (dropCurryLevel && isDuplicate(AFD, funcTy))
    return true;

  CodeCompletionResultBuilder Builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration,
                        getSemanticContext(AFD, Reason, dynamicLookupInfo));
  Builder.setAssociatedDecl(AFD);

  // Base name
  addLeadingDot(Builder);
  addValueBaseName(Builder, AFD->getBaseName());

  // Add the argument labels.
  const auto ArgLabels = AFD->getName().getArgumentNames();
  if (!ArgLabels.empty()) {
    if (!HaveLParen)
      Builder.addLeftParen();
    else
      Builder.addAnnotatedLeftParen();

    for (auto ArgLabel : ArgLabels) {
      if (ArgLabel.empty())
        Builder.addTextChunk("_");
      else
        Builder.addTextChunk(ArgLabel.str());
      Builder.addTextChunk(":");
    }

    Builder.addRightParen();
  }

  if (funcTy)
    addTypeAnnotation(Builder, funcTy, AFD->getGenericSignatureOfContext());

  return true;
}

void CompletionLookup::onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                                  DeclVisibilityKind Reason) {

  // Remember the decl name to
  SmallString<32> buffer;
  llvm::raw_svector_ostream OS(buffer);
  PrintOptions PS = PrintOptions::printDocInterface();
  PS.FullyQualifiedTypes = true;
  NTD->getDeclaredType()->print(OS, PS);
  NullTerminatedStringRef qualifiedName(
      buffer, *CompletionContext->getResultSink().Allocator);
  CompletionContext->LookedupNominalTypeNames.push_back(qualifiedName);
}

Type CompletionLookup::normalizeTypeForDuplicationCheck(Type Ty) {
  return Ty.transform([](Type T) {
    if (auto opaque = T->getAs<OpaqueTypeArchetypeType>()) {
      /// Opaque type has a _invisible_ substitution map. Since IDE can't
      /// differentiate them, replace it with empty substitution map.
      return OpaqueTypeArchetypeType::get(opaque->getDecl(),
                                          opaque->getInterfaceType(),
                                          /*Substitutions=*/{});
    }
    return T;
  });
}

void CompletionLookup::foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                                 DynamicLookupInfo dynamicLookupInfo) {
  assert(Reason !=
             DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal &&
         "Including derived requirement in non-override lookup");

  if (D->shouldHideFromEditor())
    return;

  if (IsKeyPathExpr && !KeyPathFilter(D, Reason, dynamicLookupInfo))
    return;

  if (IsSwiftKeyPathExpr && !SwiftKeyPathFilter(D, Reason))
    return;

  // If we've seen this decl+type before (possible when multiple lookups are
  // performed e.g. because of ambiguous base types), bail.
  if (isDuplicate(D, dynamicLookupInfo))
    return;

  // FIXME(InterfaceTypeRequest): Remove this.
  (void)D->getInterfaceType();
  switch (Kind) {
  case LookupKind::ValueExpr:
    if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
      // Do we want compound function names here?
      if (addCompoundFunctionNameIfDesiable(CD, Reason, dynamicLookupInfo))
        return;

      if (auto MT = ExprType->getAs<AnyMetatypeType>()) {
        Type Ty = MT->getInstanceType();
        assert(Ty && "Cannot find instance type.");

        // If instance type is type alias, show users that the constructed
        // type is the typealias instead of the underlying type of the alias.
        Optional<Type> Result = None;
        if (!CD->getInterfaceType()->is<ErrorType>() &&
            isa<TypeAliasType>(Ty.getPointer()) &&
            Ty->getDesugaredType() ==
                CD->getResultInterfaceType().getPointer()) {
          Result = Ty;
        }
        // If the expression type is not a static metatype or an archetype, the
        // base is not a type. Direct call syntax is illegal on values, so we
        // only add initializer completions if we do not have a left parenthesis
        // and either the initializer is required, the base type's instance type
        // is not a class, or this is a 'self' or 'super' reference.
        if (IsStaticMetatype || IsUnresolvedMember || Ty->is<ArchetypeType>())
          addConstructorCall(CD, Reason, dynamicLookupInfo, None, Result,
                             /*isOnType*/ true);
        else if ((IsSelfRefExpr || IsSuperRefExpr || !Ty->is<ClassType>() ||
                  CD->isRequired()) &&
                 !HaveLParen)
          addConstructorCall(CD, Reason, dynamicLookupInfo, None, Result,
                             /*isOnType*/ false);
        return;
      }
      if (!HaveLParen) {
        auto CDC = dyn_cast<ConstructorDecl>(CurrDeclContext);
        if (!CDC)
          return;

        // For classes, we do not want 'init' completions for 'self' in
        // non-convenience initializers and for 'super' in convenience
        // initializers.
        if (ExprType->is<ClassType>()) {
          if ((IsSelfRefExpr && !CDC->isConvenienceInit()) ||
              (IsSuperRefExpr && CDC->isConvenienceInit()))
            return;
        }
        if (IsSelfRefExpr || IsSuperRefExpr)
          addConstructorCall(CD, Reason, dynamicLookupInfo, None, None,
                             /*IsOnType=*/false);
      }
      return;
    }

    if (HaveLParen)
      return;

    LLVM_FALLTHROUGH;

  case LookupKind::ValueInDeclContext:
  case LookupKind::ImportFromModule:
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      addVarDeclRef(VD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      // We cannot call operators with a postfix parenthesis syntax.
      if (FD->isBinaryOperator() || FD->isUnaryOperator())
        return;

      // We cannot call accessors.  We use VarDecls and SubscriptDecls to
      // produce completions that refer to getters and setters.
      if (isa<AccessorDecl>(FD))
        return;

      // Do we want compound function names here?
      if (addCompoundFunctionNameIfDesiable(FD, Reason, dynamicLookupInfo))
        return;

      addMethodCall(FD, Reason, dynamicLookupInfo);

      // SE-0253: Callable values of user-defined nominal types.
      if (FD->isCallAsFunctionMethod() && !HaveDot &&
          (!ExprType || !ExprType->is<AnyMetatypeType>())) {
        Type funcType = getTypeOfMember(FD, dynamicLookupInfo)
                            ->castTo<AnyFunctionType>()
                            ->getResult();

        // Check for duplicates with the adjusted type too.
        if (isDuplicate(FD, funcType))
          return;

        addFunctionCallPattern(
            funcType->castTo<AnyFunctionType>(), FD,
            getSemanticContext(FD, Reason, dynamicLookupInfo));
      }
      return;
    }

    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      addNominalTypeRef(NTD, Reason, dynamicLookupInfo);
      addConstructorCallsForType(NTD->getDeclaredInterfaceType(),
                                 NTD->getName(), Reason, dynamicLookupInfo);
      return;
    }

    if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
      addTypeAliasRef(TAD, Reason, dynamicLookupInfo);
      auto type = TAD->mapTypeIntoContext(TAD->getDeclaredInterfaceType());
      if (type->mayHaveMembers())
        addConstructorCallsForType(type, TAD->getName(), Reason,
                                   dynamicLookupInfo);
      return;
    }

    if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
      addGenericTypeParamRef(GP, Reason, dynamicLookupInfo);
      auto type =
          CurrDeclContext->mapTypeIntoContext(GP->getDeclaredInterfaceType());
      addConstructorCallsForType(type, GP->getName(), Reason,
                                 dynamicLookupInfo);
      return;
    }

    if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
      addAssociatedTypeRef(AT, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
      addEnumElementRef(EED, Reason, dynamicLookupInfo,
                        /*HasTypeContext=*/false);
      return;
    }

    // Swift key path allows .[0]
    if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
      addSubscriptCall(SD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *MD = dyn_cast<MacroDecl>(D)) {
      addMacroExpansion(MD, Reason);
      return;
    }
    return;

  case LookupKind::EnumElement:
    handleEnumElement(D, Reason, dynamicLookupInfo);
    return;

  case LookupKind::Type:
  case LookupKind::TypeInDeclContext:
    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      addNominalTypeRef(NTD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
      addGenericTypeParamRef(GP, Reason, dynamicLookupInfo);
      return;
    }

    LLVM_FALLTHROUGH;
  case LookupKind::GenericRequirement:

    if (TypeAliasDecl *TAD = dyn_cast<TypeAliasDecl>(D)) {
      if (Kind == LookupKind::GenericRequirement &&
          !canBeUsedAsRequirementFirstType(BaseType, TAD))
        return;
      addTypeAliasRef(TAD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
      addAssociatedTypeRef(AT, Reason, dynamicLookupInfo);
      return;
    }

    return;
  }
}

bool CompletionLookup::handleEnumElement(ValueDecl *D,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo) {
  if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
    addEnumElementRef(EED, Reason, dynamicLookupInfo,
                      /*HasTypeContext=*/true);
    return true;
  } else if (auto *ED = dyn_cast<EnumDecl>(D)) {
    for (auto *Ele : ED->getAllElements()) {
      addEnumElementRef(Ele, Reason, dynamicLookupInfo,
                        /*HasTypeContext=*/true);
    }
    return true;
  }
  return false;
}

bool CompletionLookup::tryTupleExprCompletions(Type ExprType) {
  auto *TT = ExprType->getAs<TupleType>();
  if (!TT)
    return false;

  unsigned Index = 0;
  for (auto TupleElt : TT->getElements()) {
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        CodeCompletionResultKind::Pattern, SemanticContextKind::CurrentNominal);
    addLeadingDot(Builder);
    if (TupleElt.hasName()) {
      Builder.addBaseName(TupleElt.getName().str());
    } else {
      llvm::SmallString<4> IndexStr;
      {
        llvm::raw_svector_ostream OS(IndexStr);
        OS << Index;
      }
      Builder.addBaseName(IndexStr.str());
    }
    addTypeAnnotation(Builder, TupleElt.getType());
    ++Index;
  }
  return true;
}

bool CompletionLookup::tryFunctionCallCompletions(
    Type ExprType, const ValueDecl *VD,
    Optional<SemanticContextKind> SemanticContext) {
  ExprType = ExprType->getRValueType();
  if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
    if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(VD)) {
      addFunctionCallPattern(AFT, AFD, SemanticContext);
    } else {
      addFunctionCallPattern(AFT);
    }
    return true;
  }
  return false;
}

bool CompletionLookup::tryModuleCompletions(Type ExprType,
                                            CodeCompletionFilter Filter) {
  if (auto MT = ExprType->getAs<ModuleType>()) {
    ModuleDecl *M = MT->getModule();

    // Only lookup this module's symbols from the cache if it is not the
    // current module.
    if (M == CurrModule)
      return false;

    // If the module is shadowed by a separately imported overlay(s), look up
    // the symbols from the overlay(s) instead.
    SmallVector<ModuleDecl *, 1> ShadowingOrOriginal;
    if (auto *SF = CurrDeclContext->getParentSourceFile()) {
      SF->getSeparatelyImportedOverlays(M, ShadowingOrOriginal);
      if (ShadowingOrOriginal.empty())
        ShadowingOrOriginal.push_back(M);
    }
    for (ModuleDecl *M : ShadowingOrOriginal) {
      RequestedResultsTy Request =
          RequestedResultsTy::fromModule(M, Filter).needLeadingDot(needDot());
      RequestedCachedResults.insert(Request);
    }
    return true;
  }
  return false;
}

bool CompletionLookup::tryUnwrappedCompletions(Type ExprType, bool isIUO) {
  // FIXME: consider types convertible to T?.

  ExprType = ExprType->getRValueType();
  // FIXME: We don't always pass down whether a type is from an
  // unforced IUO.
  if (isIUO) {
    if (Type Unwrapped = ExprType->getOptionalObjectType()) {
      lookupVisibleMemberDecls(*this, Unwrapped, DotLoc, CurrDeclContext,
                               IncludeInstanceMembers,
                               /*includeDerivedRequirements*/ false,
                               /*includeProtocolExtensionMembers*/ true);
      return true;
    }
    assert(IsUnwrappedOptional &&
           "IUOs should be optional if not bound/forced");
    return false;
  }

  if (Type Unwrapped = ExprType->getOptionalObjectType()) {
    llvm::SaveAndRestore<bool> ChangeNeedOptionalUnwrap(NeedOptionalUnwrap,
                                                        true);
    if (DotLoc.isValid()) {
      // Let's not erase the dot if the completion is after a swift key path
      // root because \A?.?.member is the correct way to access wrapped type
      // member from an optional key path root.
      auto loc = IsAfterSwiftKeyPathRoot ? DotLoc.getAdvancedLoc(1) : DotLoc;
      NumBytesToEraseForOptionalUnwrap = Ctx.SourceMgr.getByteDistance(
          loc, Ctx.SourceMgr.getIDEInspectionTargetLoc());
    } else {
      NumBytesToEraseForOptionalUnwrap = 0;
    }
    if (NumBytesToEraseForOptionalUnwrap <=
        CodeCompletionResult::MaxNumBytesToErase) {
      if (!tryTupleExprCompletions(Unwrapped)) {
        lookupVisibleMemberDecls(*this, Unwrapped, DotLoc,
                                 CurrDeclContext,
                                 IncludeInstanceMembers,
                                 /*includeDerivedRequirements*/ false,
                                 /*includeProtocolExtensionMembers*/ true);
      }
    }
    return true;
  }
  return false;
}

void CompletionLookup::getPostfixKeywordCompletions(Type ExprType,
                                                    Expr *ParsedExpr) {
  if (IsSuperRefExpr)
    return;

  if (!ExprType->getAs<ModuleType>()) {
    addKeyword(getTokenText(tok::kw_self), ExprType->getRValueType(),
               SemanticContextKind::CurrentNominal,
               CodeCompletionKeywordKind::kw_self);
  }

  if (isa<TypeExpr>(ParsedExpr)) {
    if (auto *T = ExprType->getAs<AnyMetatypeType>()) {
      auto instanceTy = T->getInstanceType();
      if (instanceTy->isAnyExistentialType()) {
        addKeyword("Protocol", MetatypeType::get(instanceTy),
                   SemanticContextKind::CurrentNominal);
        addKeyword("Type", ExistentialMetatypeType::get(instanceTy),
                   SemanticContextKind::CurrentNominal);
      } else {
        addKeyword("Type", MetatypeType::get(instanceTy),
                   SemanticContextKind::CurrentNominal);
      }
    }
  }
}

void CompletionLookup::getValueExprCompletions(Type ExprType, ValueDecl *VD) {
  Kind = LookupKind::ValueExpr;
  NeedLeadingDot = !HaveDot;

  ExprType = ExprType->getRValueType();
  assert(!ExprType->hasTypeParameter());

  this->ExprType = ExprType;

  // Open existential types, so that lookupVisibleMemberDecls() can properly
  // substitute them.
  bool WasOptional = false;
  if (auto OptionalType = ExprType->getOptionalObjectType()) {
    ExprType = OptionalType;
    WasOptional = true;
  }

  if (!ExprType->getMetatypeInstanceType()->isAnyObject()) {
    if (ExprType->isAnyExistentialType()) {
      ExprType = OpenedArchetypeType::getAny(ExprType->getCanonicalType(),
                                             CurrDeclContext->getGenericSignatureOfContext());
    }
  }
  if (!IsSelfRefExpr && !IsSuperRefExpr && ExprType->getAnyNominal() &&
      ExprType->getAnyNominal()->isActor()) {
    IsCrossActorReference = true;
  }

  if (WasOptional)
    ExprType = OptionalType::get(ExprType);

  // Handle special cases
  bool isIUO = VD && VD->isImplicitlyUnwrappedOptional();
  if (tryFunctionCallCompletions(ExprType, VD))
    return;
  if (tryModuleCompletions(ExprType, {CodeCompletionFilterFlag::Expr,
                                      CodeCompletionFilterFlag::Type}))
    return;
  if (tryTupleExprCompletions(ExprType))
    return;
  // Don't check/return so we still add the members of Optional itself below
  tryUnwrappedCompletions(ExprType, isIUO);

  lookupVisibleMemberDecls(*this, ExprType, DotLoc, CurrDeclContext,
                           IncludeInstanceMembers,
                           /*includeDerivedRequirements*/ false,
                           /*includeProtocolExtensionMembers*/ true);
}

void CompletionLookup::collectOperators(
    SmallVectorImpl<OperatorDecl *> &results) {
  assert(CurrDeclContext);
  for (auto import : namelookup::getAllImports(CurrDeclContext))
    import.importedModule->getOperatorDecls(results);
}

void CompletionLookup::addPostfixBang(Type resultType) {
  CodeCompletionResultBuilder builder = makeResultBuilder(
      CodeCompletionResultKind::BuiltinOperator, SemanticContextKind::None);
  // FIXME: we can't use the exclamation mark chunk kind, or it isn't
  // included in the completion name.
  builder.addTextChunk("!");
  assert(resultType);
  addTypeAnnotation(builder, resultType);
}

void CompletionLookup::addPostfixOperatorCompletion(OperatorDecl *op,
                                                    Type resultType) {
  // FIXME: we should get the semantic context of the function, not the
  // operator decl.
  auto semanticContext =
      getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel, {});
  CodeCompletionResultBuilder builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration, semanticContext);

  // FIXME: handle variable amounts of space.
  if (HaveLeadingSpace)
    builder.setNumBytesToErase(1);
  builder.setAssociatedDecl(op);
  builder.addBaseName(op->getName().str());
  assert(resultType);
  addTypeAnnotation(builder, resultType);
}

void CompletionLookup::tryPostfixOperator(Expr *expr, PostfixOperatorDecl *op) {
  ConcreteDeclRef referencedDecl;
  FunctionType *funcTy = getTypeOfCompletionOperator(
      const_cast<DeclContext *>(CurrDeclContext), expr, op->getName(),
      DeclRefKind::PostfixOperator, referencedDecl);
  if (!funcTy)
    return;

  // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
  addPostfixOperatorCompletion(op, funcTy->getResult());
}

void CompletionLookup::addAssignmentOperator(Type RHSType, Type resultType) {
  CodeCompletionResultBuilder builder = makeResultBuilder(
      CodeCompletionResultKind::BuiltinOperator, SemanticContextKind::None);

  if (HaveLeadingSpace)
    builder.addAnnotatedWhitespace(" ");
  else
    builder.addWhitespace(" ");
  builder.addEqual();
  builder.addWhitespace(" ");
  assert(RHSType && resultType);
  Type contextTy;
  if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
    contextTy = typeContext->getDeclaredTypeInContext();
  builder.addCallArgument(Identifier(), RHSType, contextTy);
  addTypeAnnotation(builder, resultType);
}

void CompletionLookup::addInfixOperatorCompletion(OperatorDecl *op,
                                                  Type resultType,
                                                  Type RHSType) {
  // FIXME: we should get the semantic context of the function, not the
  // operator decl.
  auto semanticContext =
      getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel, {});
  CodeCompletionResultBuilder builder =
      makeResultBuilder(CodeCompletionResultKind::Declaration, semanticContext);
  builder.setAssociatedDecl(op);

  if (HaveLeadingSpace)
    builder.addAnnotatedWhitespace(" ");
  else
    builder.addWhitespace(" ");
  builder.addBaseName(op->getName().str());
  builder.addWhitespace(" ");
  if (RHSType) {
    Type contextTy;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      contextTy = typeContext->getDeclaredTypeInContext();
    builder.addCallArgument(Identifier(), RHSType, contextTy);
  }
  if (resultType)
    addTypeAnnotation(builder, resultType);
}

void CompletionLookup::tryInfixOperatorCompletion(Expr *foldedExpr,
                                                  InfixOperatorDecl *op) {
  ConcreteDeclRef referencedDecl;
  FunctionType *funcTy = getTypeOfCompletionOperator(
      const_cast<DeclContext *>(CurrDeclContext), foldedExpr, op->getName(),
      DeclRefKind::BinaryOperator, referencedDecl);
  if (!funcTy)
    return;

  Type lhsTy = funcTy->getParams()[0].getPlainType();
  Type rhsTy = funcTy->getParams()[1].getPlainType();
  Type resultTy = funcTy->getResult();

  // Don't complete optional operators on non-optional types.
  if (!lhsTy->getRValueType()->getOptionalObjectType()) {
    // 'T ?? T'
    if (op->getName().str() == "??")
      return;
    // 'T == nil'
    if (auto NT = rhsTy->getNominalOrBoundGenericNominal())
      if (NT->getName() ==
          CurrDeclContext->getASTContext().Id_OptionalNilComparisonType)
        return;
  }

  // If the right-hand side and result type are both type parameters, we're
  // not providing a useful completion.
  if (resultTy->isTypeParameter() && rhsTy->isTypeParameter())
    return;

  // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
  addInfixOperatorCompletion(op, funcTy->getResult(),
                             funcTy->getParams()[1].getPlainType());
}

Expr *
CompletionLookup::typeCheckLeadingSequence(Expr *LHS,
                                           ArrayRef<Expr *> leadingSequence) {
  if (leadingSequence.empty())
    return LHS;

  SourceRange sequenceRange(leadingSequence.front()->getStartLoc(),
                            LHS->getEndLoc());
  auto *expr = findParsedExpr(CurrDeclContext, sequenceRange);
  if (!expr)
    return LHS;

  if (expr->getType() && !expr->getType()->hasError())
    return expr;

  if (!typeCheckExpression(const_cast<DeclContext *>(CurrDeclContext), expr))
    return expr;
  return LHS;
}

void CompletionLookup::getOperatorCompletions(
    Expr *LHS, ArrayRef<Expr *> leadingSequence) {
  if (IsSuperRefExpr)
    return;

  Expr *foldedExpr = typeCheckLeadingSequence(LHS, leadingSequence);

  SmallVector<OperatorDecl *, 16> operators;
  collectOperators(operators);
  // FIXME: this always chooses the first operator with the given name.
  llvm::DenseSet<Identifier> seenPostfixOperators;
  llvm::DenseSet<Identifier> seenInfixOperators;

  for (auto op : operators) {
    switch (op->getKind()) {
    case DeclKind::PrefixOperator:
      // Don't insert prefix operators in postfix position.
      // FIXME: where should these get completed?
      break;
    case DeclKind::PostfixOperator:
      if (seenPostfixOperators.insert(op->getName()).second)
        tryPostfixOperator(LHS, cast<PostfixOperatorDecl>(op));
      break;
    case DeclKind::InfixOperator:
      if (seenInfixOperators.insert(op->getName()).second)
        tryInfixOperatorCompletion(foldedExpr, cast<InfixOperatorDecl>(op));
      break;
    default:
      llvm_unreachable("unexpected operator kind");
    }
  }

  if (leadingSequence.empty() && LHS->getType() &&
      LHS->getType()->hasLValueType()) {
    addAssignmentOperator(LHS->getType()->getRValueType(),
                          CurrDeclContext->getASTContext().TheEmptyTupleType);
  }

  // FIXME: unify this with the ?.member completions.
  if (auto T = LHS->getType())
    if (auto ValueT = T->getRValueType()->getOptionalObjectType())
      addPostfixBang(ValueT);
}

void CompletionLookup::addTypeRelationFromProtocol(
    CodeCompletionResultBuilder &builder, CodeCompletionLiteralKind kind) {
  Type literalType;

  // The literal can produce any type that conforms to its ExpressibleBy
  // protocol. Figure out as which type we want to show it in code completion.
  auto *PD = Ctx.getProtocol(protocolForLiteralKind(kind));
  for (auto T : expectedTypeContext.getPossibleTypes()) {
    if (!T)
      continue;

    // Convert through optional types unless we're looking for a protocol
    // that Optional itself conforms to.
    if (kind != CodeCompletionLiteralKind::NilLiteral) {
      if (auto optionalObjT = T->getOptionalObjectType()) {
        T = optionalObjT;
      }
    }

    // Check for conformance to the literal protocol.
    if (T->getAnyNominal()) {
      if (CurrModule->lookupConformance(T, PD)) {
        literalType = T;
        break;
      }
    }
  }

  // Fallback to showing the default type.
  if (!literalType) {
    literalType = defaultTypeLiteralKind(kind, Ctx);
  }
  if (literalType) {
    addTypeAnnotation(builder, literalType);
    builder.setResultTypes(literalType);
    builder.setTypeContext(expectedTypeContext, CurrDeclContext);
  }
}

void CompletionLookup::addValueLiteralCompletions() {
  auto &context = CurrDeclContext->getASTContext();

  CodeCompletionFlair flair;
  if (isCodeCompletionAtTopLevelOfLibraryFile(CurrDeclContext))
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;

  auto addFromProto =
      [&](CodeCompletionLiteralKind kind,
          llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer,
          bool isKeyword = false) {
        CodeCompletionResultBuilder builder = makeResultBuilder(
            CodeCompletionResultKind::Literal, SemanticContextKind::None);
        builder.setLiteralKind(kind);
        builder.addFlair(flair);

        consumer(builder);
        addTypeRelationFromProtocol(builder, kind);
      };

  // FIXME: the pedantically correct way is to resolve Swift.*LiteralType.

  using LK = CodeCompletionLiteralKind;
  using Builder = CodeCompletionResultBuilder;

  // Add literal completions that conform to specific protocols.
  addFromProto(LK::IntegerLiteral,
               [](Builder &builder) { builder.addTextChunk("0"); });
  addFromProto(
      LK::BooleanLiteral, [](Builder &builder) { builder.addBaseName("true"); },
      /*isKeyword=*/true);
  addFromProto(
      LK::BooleanLiteral,
      [](Builder &builder) { builder.addBaseName("false"); },
      /*isKeyword=*/true);
  addFromProto(
      LK::NilLiteral, [](Builder &builder) { builder.addBaseName("nil"); },
      /*isKeyword=*/true);
  addFromProto(LK::StringLiteral, [&](Builder &builder) {
    builder.addTextChunk("\"");
    builder.addSimpleNamedParameter("abc");
    builder.addTextChunk("\"");
  });
  addFromProto(LK::ArrayLiteral, [&](Builder &builder) {
    builder.addLeftBracket();
    builder.addSimpleNamedParameter("values");
    builder.addRightBracket();
  });
  addFromProto(LK::DictionaryLiteral, [&](Builder &builder) {
    builder.addLeftBracket();
    builder.addSimpleNamedParameter("key");
    builder.addTextChunk(": ");
    builder.addSimpleNamedParameter("value");
    builder.addRightBracket();
  });

  // Optionally add object literals.
  if (Sink.includeObjectLiterals) {
    auto floatType = context.getFloatType();
    addFromProto(LK::ColorLiteral, [&](Builder &builder) {
      builder.addBaseName("#colorLiteral");
      builder.addLeftParen();
      builder.addCallArgument(context.getIdentifier("red"), floatType);
      builder.addComma();
      builder.addCallArgument(context.getIdentifier("green"), floatType);
      builder.addComma();
      builder.addCallArgument(context.getIdentifier("blue"), floatType);
      builder.addComma();
      builder.addCallArgument(context.getIdentifier("alpha"), floatType);
      builder.addRightParen();
    });

    auto stringType = context.getStringType();
    addFromProto(LK::ImageLiteral, [&](Builder &builder) {
      builder.addBaseName("#imageLiteral");
      builder.addLeftParen();
      builder.addCallArgument(context.getIdentifier("resourceName"),
                              stringType);
      builder.addRightParen();
    });
  }

  // Add tuple completion (item, item).
  {
    CodeCompletionResultBuilder builder = makeResultBuilder(
        CodeCompletionResultKind::Literal, SemanticContextKind::None);
    builder.setLiteralKind(LK::Tuple);
    builder.addFlair(flair);

    builder.addLeftParen();
    builder.addSimpleNamedParameter("values");
    builder.addRightParen();
    for (auto T : expectedTypeContext.getPossibleTypes()) {
      if (T && T->is<TupleType>() && !T->isVoid()) {
        addTypeAnnotation(builder, T);
        builder.setResultTypes(T);
        builder.setTypeContext(expectedTypeContext, CurrDeclContext);
        break;
      }
    }
  }
}

void CompletionLookup::addObjCPoundKeywordCompletions(bool needPound) {
  if (!Ctx.LangOpts.EnableObjCInterop)
    return;

  // If the expected type is ObjectiveC.Selector, add #selector. If
  // it's String, add #keyPath.
  bool addedSelector = false;
  bool addedKeyPath = false;

  for (auto T : expectedTypeContext.getPossibleTypes()) {
    T = T->lookThroughAllOptionalTypes();
    if (auto structDecl = T->getStructOrBoundGenericStruct()) {
      if (!addedSelector && structDecl->getName() == Ctx.Id_Selector &&
          structDecl->getParentModule()->getName() == Ctx.Id_ObjectiveC) {
        addPoundSelector(needPound);
        if (addedKeyPath)
          break;
        addedSelector = true;
        continue;
      }
    }

    if (!addedKeyPath && T->isString()) {
      addPoundKeyPath(needPound);
      if (addedSelector)
        break;
      addedKeyPath = true;
      continue;
    }
  }
}

void CompletionLookup::getMacroCompletions(CodeCompletionMacroRoles roles) {
  RequestedCachedResults.insert(
      RequestedResultsTy::topLevelResults(getCompletionFilter(roles)));
}

void CompletionLookup::getValueCompletionsInDeclContext(SourceLoc Loc,
                                                        DeclFilter Filter,
                                                        bool LiteralCompletions,
                                                        bool ModuleQualifier) {
  ExprType = Type();
  Kind = LookupKind::ValueInDeclContext;
  NeedLeadingDot = false;

  AccessFilteringDeclConsumer AccessFilteringConsumer(CurrDeclContext, *this);
  FilteredDeclConsumer FilteringConsumer(AccessFilteringConsumer, Filter);

  lookupVisibleDecls(FilteringConsumer, CurrDeclContext,
                     /*IncludeTopLevel=*/false, Loc);

  CodeCompletionFilter filter{CodeCompletionFilterFlag::Expr,
                              CodeCompletionFilterFlag::Type};
  if (ModuleQualifier) {
    filter |= CodeCompletionFilterFlag::Module;
  }
  RequestedCachedResults.insert(RequestedResultsTy::topLevelResults(filter));

  if (CompletionContext) {
    // FIXME: this is an awful simplification that says all and only enums can
    // use implicit member syntax (leading dot). Computing the accurate answer
    // using lookup (e.g. getUnresolvedMemberCompletions) is too expensive,
    // and for some clients this approximation is good enough.
    CompletionContext->MayUseImplicitMemberExpr =
        llvm::any_of(expectedTypeContext.getPossibleTypes(), [](Type T) {
          if (auto *NTD = T->getAnyNominal())
            return isa<EnumDecl>(NTD);
          return false;
        });
  }

  if (LiteralCompletions) {
    addValueLiteralCompletions();
  }

  addObjCPoundKeywordCompletions(/*needPound=*/true);
}

bool CompletionLookup::isInitializerOnOptional(Type T, ValueDecl *VD) {
  bool IsOptionalType = false;
  IsOptionalType |= static_cast<bool>(T->getOptionalObjectType());
  if (auto *NTD = T->getAnyNominal()) {
    IsOptionalType |= NTD->getBaseIdentifier() ==
                      VD->getASTContext().Id_OptionalNilComparisonType;
  }
  if (IsOptionalType && VD->getModuleContext()->isStdlibModule() &&
      isa<ConstructorDecl>(VD)) {
    return true;
  } else {
    return false;
  }
}

void CompletionLookup::getUnresolvedMemberCompletions(Type T) {
  if (!T->mayHaveMembers())
    return;

  NeedLeadingDot = !HaveDot;

  if (auto objT = T->getOptionalObjectType()) {
    // Add 'nil' keyword with erasing '.' instruction.
    unsigned bytesToErase = 0;
    auto &SM = CurrDeclContext->getASTContext().SourceMgr;
    if (DotLoc.isValid())
      bytesToErase = SM.getByteDistance(DotLoc, SM.getIDEInspectionTargetLoc());
    addKeyword("nil", T, SemanticContextKind::None,
               CodeCompletionKeywordKind::kw_nil, bytesToErase);
  }

  // We can only say .foo where foo is a static member of the contextual
  // type and has the same type (or if the member is a function, then the
  // same result type) as the contextual type.
  FilteredDeclConsumer consumer(*this,
                                [=](ValueDecl *VD, DeclVisibilityKind Reason,
                                    DynamicLookupInfo dynamicLookupInfo) {
                                  // In optional context, ignore
                                  // '.init(<some>)', 'init(nilLiteral:)',
                                  return !isInitializerOnOptional(T, VD);
                                });

  auto baseType = MetatypeType::get(T);
  llvm::SaveAndRestore<LookupKind> SaveLook(Kind, LookupKind::ValueExpr);
  llvm::SaveAndRestore<Type> SaveType(ExprType, baseType);
  llvm::SaveAndRestore<bool> SaveUnresolved(IsUnresolvedMember, true);
  lookupVisibleMemberDecls(consumer, baseType, DotLoc, CurrDeclContext,
                           /*includeInstanceMembers=*/false,
                           /*includeDerivedRequirements*/ false,
                           /*includeProtocolExtensionMembers*/ true);
}

void CompletionLookup::getEnumElementPatternCompletions(Type T) {
  if (!isa_and_nonnull<EnumDecl>(T->getAnyNominal()))
    return;

  auto baseType = MetatypeType::get(T);
  llvm::SaveAndRestore<LookupKind> SaveLook(Kind, LookupKind::EnumElement);
  llvm::SaveAndRestore<Type> SaveType(ExprType, baseType);
  llvm::SaveAndRestore<bool> SaveUnresolved(IsUnresolvedMember, true);
  lookupVisibleMemberDecls(*this, baseType, DotLoc, CurrDeclContext,
                           /*includeInstanceMembers=*/false,
                           /*includeDerivedRequirements=*/false,
                           /*includeProtocolExtensionMembers=*/true);
}

void CompletionLookup::getUnresolvedMemberCompletions(ArrayRef<Type> Types) {
  NeedLeadingDot = !HaveDot;

  SmallPtrSet<CanType, 4> seenTypes;
  for (auto T : Types) {
    if (!T || !seenTypes.insert(T->getCanonicalType()).second)
      continue;

    if (auto objT = T->getOptionalObjectType()) {
      // If this is optional type, perform completion for the object type.
      // i.e. 'let _: Enum??? = .enumMember' is legal.
      objT = objT->lookThroughAllOptionalTypes();
      if (seenTypes.insert(objT->getCanonicalType()).second)
        getUnresolvedMemberCompletions(objT);
    }
    getUnresolvedMemberCompletions(T);
  }
}

void CompletionLookup::addCallArgumentCompletionResults(
    ArrayRef<PossibleParamInfo> ParamInfos, bool isLabeledTrailingClosure) {
  Type ContextType;
  if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
    ContextType = typeContext->getDeclaredTypeInContext();

  for (auto Info : ParamInfos) {
    const auto *Arg = Info.Param;
    if (!Arg)
      continue;
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        CodeCompletionResultKind::Pattern,
        // FIXME: SemanticContextKind::Local is not correct.
        // Use 'None' (and fix prioritization) or introduce a new context.
        SemanticContextKind::Local);
    Builder.addCallArgument(Arg->getLabel(), Identifier(), Arg->getPlainType(),
                            ContextType, Arg->isVariadic(), Arg->isInOut(),
                            /*IsIUO=*/false, Arg->isAutoClosure(),
                            /*UseUnderscoreLabel=*/true,
                            isLabeledTrailingClosure, /*HasDefault=*/false);
    Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
    auto Ty = Arg->getPlainType();
    if (Arg->isInOut()) {
      Ty = InOutType::get(Ty);
    } else if (Arg->isAutoClosure()) {
      // 'Ty' may be ErrorType.
      if (auto funcTy = Ty->getAs<FunctionType>())
        Ty = funcTy->getResult();
    }
    // The type annotation is the argument type. But the argument label itself
    // does not produce an expression with a result type so we set the result
    // type as being not applicable.
    addTypeAnnotation(Builder, Ty);
    Builder.setResultTypeNotApplicable();
  }
}

void CompletionLookup::getTypeCompletions(Type BaseType) {
  if (tryModuleCompletions(BaseType, CodeCompletionFilterFlag::Type))
    return;
  Kind = LookupKind::Type;
  this->BaseType = BaseType;
  NeedLeadingDot = !HaveDot;
  lookupVisibleMemberDecls(*this, MetatypeType::get(BaseType), DotLoc,
                           CurrDeclContext, IncludeInstanceMembers,
                           /*includeDerivedRequirements*/ false,
                           /*includeProtocolExtensionMembers*/ false);
  if (BaseType->isAnyExistentialType()) {
    addKeyword("Protocol", MetatypeType::get(BaseType));
    addKeyword("Type", ExistentialMetatypeType::get(BaseType));
  } else if (!BaseType->is<ModuleType>()) {
    addKeyword("Type", MetatypeType::get(BaseType));
  }
}

void CompletionLookup::getGenericRequirementCompletions(
    DeclContext *DC, SourceLoc CodeCompletionLoc) {
  auto genericSig = DC->getGenericSignatureOfContext();
  if (!genericSig)
    return;

  for (auto GPT : genericSig.getGenericParams()) {
    addGenericTypeParamRef(GPT->getDecl(), DeclVisibilityKind::GenericParameter,
                           {});
  }

  // For non-protocol nominal type decls, only suggest generic parameters.
  if (auto D = DC->getAsDecl())
    if (isa<NominalTypeDecl>(D) && !isa<ProtocolDecl>(D))
      return;

  auto typeContext = DC->getInnermostTypeContext();
  if (!typeContext)
    return;

  auto selfTy = typeContext->getSelfTypeInContext();
  Kind = LookupKind::GenericRequirement;
  this->BaseType = selfTy;
  NeedLeadingDot = false;
  lookupVisibleMemberDecls(*this, MetatypeType::get(selfTy), DotLoc,
                           CurrDeclContext, IncludeInstanceMembers,
                           /*includeDerivedRequirements*/ false,
                           /*includeProtocolExtensionMembers*/ true);
  // We not only allow referencing nested types/typealiases directly, but also
  // qualified by the current type. Thus also suggest current self type so the
  // user can do a memberwise lookup on it.
  if (auto SelfType = typeContext->getSelfNominalTypeDecl()) {
    addNominalTypeRef(SelfType, DeclVisibilityKind::LocalVariable,
                      DynamicLookupInfo());
  }

  // Self is also valid in all cases in which it can be used in function
  // bodies. Suggest it if applicable.
  getSelfTypeCompletionInDeclContext(CodeCompletionLoc,
                                     /*isForResultType=*/false);
}

bool CompletionLookup::canUseAttributeOnDecl(DeclAttrKind DAK, bool IsInSil,
                                             bool IsConcurrencyEnabled,
                                             Optional<DeclKind> DK,
                                             StringRef Name) {
  if (DeclAttribute::isUserInaccessible(DAK))
    return false;
  if (DeclAttribute::isDeclModifier(DAK))
    return false;
  if (DeclAttribute::shouldBeRejectedByParser(DAK))
    return false;
  if (!IsInSil && DeclAttribute::isSilOnly(DAK))
    return false;
  if (!IsConcurrencyEnabled && DeclAttribute::isConcurrencyOnly(DAK))
    return false;
  if (!DK.has_value())
    return true;
  // Hide underscored attributes even if they are not marked as user
  // inaccessible. This can happen for attributes that are an underscored
  // variant of a user-accessible attribute (like @_backDeployed)
  if (Name.empty() || Name[0] == '_')
    return false;
  return DeclAttribute::canAttributeAppearOnDeclKind(DAK, DK.value());
}

void CompletionLookup::getAttributeDeclCompletions(bool IsInSil,
                                                   Optional<DeclKind> DK) {
  // FIXME: also include user-defined attribute keywords
  StringRef TargetName = "Declaration";
  if (DK.has_value()) {
    switch (DK.value()) {
#define DECL(Id, ...)                                                          \
  case DeclKind::Id:                                                           \
    TargetName = #Id;                                                          \
    break;
#include "swift/AST/DeclNodes.def"
    }
  }
  bool IsConcurrencyEnabled = Ctx.LangOpts.EnableExperimentalConcurrency;
  std::string Description = TargetName.str() + " Attribute";
#define DECL_ATTR_ALIAS(KEYWORD, NAME) DECL_ATTR(KEYWORD, NAME, 0, 0)
#define DECL_ATTR(KEYWORD, NAME, ...)                                          \
  if (canUseAttributeOnDecl(DAK_##NAME, IsInSil, IsConcurrencyEnabled, DK,     \
                            #KEYWORD))                                         \
    addDeclAttrKeyword(#KEYWORD, Description);
#include "swift/AST/Attr.def"
}

void CompletionLookup::getAttributeDeclParamCompletions(
    CustomSyntaxAttributeKind AttrKind, int ParamIndex, bool HasLabel) {
  switch (AttrKind) {
  case CustomSyntaxAttributeKind::Available:
    if (ParamIndex == 0) {
      addDeclAttrParamKeyword("*", /*Parameters=*/{}, "Platform", false);

#define AVAILABILITY_PLATFORM(X, PrettyName)                                   \
  addDeclAttrParamKeyword(swift::platformString(PlatformKind::X),              \
                          /*Parameters=*/{}, "Platform", false);
#include "swift/AST/PlatformKinds.def"

    } else {
      addDeclAttrParamKeyword("unavailable", /*Parameters=*/{}, "", false);
      addDeclAttrParamKeyword("message", /*Parameters=*/{}, "Specify message",
                              true);
      addDeclAttrParamKeyword("renamed", /*Parameters=*/{},
                              "Specify replacing name", true);
      addDeclAttrParamKeyword("introduced", /*Parameters=*/{},
                              "Specify version number", true);
      addDeclAttrParamKeyword("deprecated", /*Parameters=*/{},
                              "Specify version number", true);
    }
    break;
  case CustomSyntaxAttributeKind::FreestandingMacro:
  case CustomSyntaxAttributeKind::AttachedMacro:
    switch (ParamIndex) {
    case 0:
      for (auto role : getAllMacroRoles()) {
        bool isRoleSupported = isMacroSupported(role, Ctx);
        if (AttrKind == CustomSyntaxAttributeKind::FreestandingMacro) {
          isRoleSupported &= isFreestandingMacro(role);
        } else if (AttrKind == CustomSyntaxAttributeKind::AttachedMacro) {
          isRoleSupported &= isAttachedMacro(role);
        }
        if (isRoleSupported) {
          addDeclAttrParamKeyword(getMacroRoleString(role), /*Parameters=*/{},
                                  /*Annotation=*/"", /*NeedsSpecify=*/false);
        }
      }
      break;
    case 1:
      if (HasLabel) {
        for (auto kind : getAllMacroIntroducedDeclNameKinds()) {
          auto name = getMacroIntroducedDeclNameString(kind);
          SmallVector<StringRef, 1> Parameters;
          if (macroIntroducedNameRequiresArgument(kind)) {
            Parameters = {"name"};
          }
          addDeclAttrParamKeyword(name, Parameters, /*Annotation=*/"",
                                  /*NeedsSpecify=*/false);
        }
      } else {
        addDeclAttrParamKeyword("names", /*Parameters=*/{},
                                "Specify declared names",
                                /*NeedsSpecify=*/true);
      }
      break;
    }
    break;
  }
}

void CompletionLookup::getTypeAttributeKeywordCompletions() {
  auto addTypeAttr = [&](StringRef Name) {
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        CodeCompletionResultKind::Keyword, SemanticContextKind::None);
    Builder.addAttributeKeyword(Name, "Type Attribute");
  };
  addTypeAttr("autoclosure");
  addTypeAttr("convention(swift)");
  addTypeAttr("convention(block)");
  addTypeAttr("convention(c)");
  addTypeAttr("convention(thin)");
  addTypeAttr("escaping");
}

void CompletionLookup::collectPrecedenceGroups() {
  assert(CurrDeclContext);

  if (CurrModule) {
    for (auto FU : CurrModule->getFiles()) {
      // We are looking through the current module,
      // inspect only source files.
      if (FU->getKind() != FileUnitKind::Source)
        continue;

      llvm::SmallVector<PrecedenceGroupDecl *, 4> results;
      cast<SourceFile>(FU)->getPrecedenceGroups(results);

      for (auto PG : results)
        addPrecedenceGroupRef(PG);
    }
  }
  for (auto Import : namelookup::getAllImports(CurrDeclContext)) {
    auto Module = Import.importedModule;
    if (Module == CurrModule)
      continue;

    RequestedCachedResults.insert(RequestedResultsTy::fromModule(
        Module, CodeCompletionFilterFlag::PrecedenceGroup));
  }
}

void CompletionLookup::getPrecedenceGroupCompletions(
    CodeCompletionCallbacks::PrecedenceGroupCompletionKind SK) {
  switch (SK) {
  case CodeCompletionCallbacks::PrecedenceGroupCompletionKind::Associativity:
    addKeyword(getAssociativitySpelling(Associativity::None));
    addKeyword(getAssociativitySpelling(Associativity::Left));
    addKeyword(getAssociativitySpelling(Associativity::Right));
    return;
  case CodeCompletionCallbacks::PrecedenceGroupCompletionKind::Assignment:
    addKeyword(getTokenText(tok::kw_false), Type(), SemanticContextKind::None,
               CodeCompletionKeywordKind::kw_false);
    addKeyword(getTokenText(tok::kw_true), Type(), SemanticContextKind::None,
               CodeCompletionKeywordKind::kw_true);
    return;
  case CodeCompletionCallbacks::PrecedenceGroupCompletionKind::AttributeList:
    addKeyword("associativity");
    addKeyword("higherThan");
    addKeyword("lowerThan");
    addKeyword("assignment");
    return;
  case CodeCompletionCallbacks::PrecedenceGroupCompletionKind::Relation:
    collectPrecedenceGroups();
    return;
  }
  llvm_unreachable("not a precedencegroup SyntaxKind");
}

void CompletionLookup::getPoundAvailablePlatformCompletions() {

  // The platform names should be identical to those in @available.
  getAttributeDeclParamCompletions(CustomSyntaxAttributeKind::Available, 0,
                                   /*HasLabel=*/false);
}

void CompletionLookup::getSelfTypeCompletionInDeclContext(
    SourceLoc Loc, bool isForDeclResult) {
  const DeclContext *typeDC = CurrDeclContext->getInnermostTypeContext();
  if (!typeDC)
    return;

  // For protocols, there's a 'Self' generic parameter.
  if (typeDC->getSelfProtocolDecl())
    return;

  Type selfType =
      CurrDeclContext->mapTypeIntoContext(typeDC->getSelfInterfaceType());

  if (typeDC->getSelfClassDecl()) {
    // In classes, 'Self' can be used in result type of func, subscript and
    // computed property, or inside function bodies.
    bool canUseDynamicSelf = false;
    if (isForDeclResult) {
      canUseDynamicSelf = true;
    } else {
      const auto *checkDC = CurrDeclContext;
      if (isa<TypeAliasDecl>(checkDC))
        checkDC = checkDC->getParent();

      if (const auto *AFD = checkDC->getInnermostMethodContext()) {
        canUseDynamicSelf =
            Ctx.SourceMgr.rangeContainsTokenLoc(AFD->getBodySourceRange(), Loc);
      }
    }
    if (!canUseDynamicSelf)
      return;
    // 'Self' in class is a dynamic type.
    selfType = DynamicSelfType::get(selfType, Ctx);
  } else {
    // In enums and structs, 'Self' is just an alias for the nominal type,
    // and can be used anywhere.
  }

  CodeCompletionResultBuilder Builder = makeResultBuilder(
      CodeCompletionResultKind::Keyword, SemanticContextKind::CurrentNominal);
  Builder.addKeyword("Self");
  Builder.setKeywordKind(CodeCompletionKeywordKind::kw_Self);
  addTypeAnnotation(Builder, selfType);
}

void CompletionLookup::getTypeCompletionsInDeclContext(SourceLoc Loc,
                                                       bool ModuleQualifier) {
  Kind = LookupKind::TypeInDeclContext;

  AccessFilteringDeclConsumer AccessFilteringConsumer(CurrDeclContext, *this);
  lookupVisibleDecls(AccessFilteringConsumer, CurrDeclContext,
                     /*IncludeTopLevel=*/false, Loc);

  CodeCompletionFilter filter{CodeCompletionFilterFlag::Type};
  if (ModuleQualifier) {
    filter |= CodeCompletionFilterFlag::Module;
  }
  RequestedCachedResults.insert(RequestedResultsTy::topLevelResults(filter));
}

namespace {

/// A \c VisibleDeclConsumer that stores all decls that are found and is able
/// to forward the to another \c VisibleDeclConsumer later.
class StoringDeclConsumer : public VisibleDeclConsumer {
  struct FoundDecl {
    ValueDecl *VD;
    DeclVisibilityKind Reason;
    DynamicLookupInfo DynamicLookupInfo;
  };

  std::vector<FoundDecl> FoundDecls;

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo DynamicLookupInfo = {}) override {
    FoundDecls.push_back({VD, Reason, DynamicLookupInfo});
  }

public:
  /// Call \c foundDecl for every declaration that this consumer has found.
  void forward(VisibleDeclConsumer &Consumer) {
    for (auto &FoundDecl : FoundDecls) {
      Consumer.foundDecl(FoundDecl.VD, FoundDecl.Reason,
                         FoundDecl.DynamicLookupInfo);
    }
  }
};

} // namespace

void CompletionLookup::getToplevelCompletions(CodeCompletionFilter Filter) {
  Kind = (Filter - CodeCompletionFilterFlag::Module)
                 .containsOnly(CodeCompletionFilterFlag::Type)
             ? LookupKind::TypeInDeclContext
             : LookupKind::ValueInDeclContext;
  NeedLeadingDot = false;

  // If we have 'addinitstotoplevel' enabled, calling `foundDecl` on `this`
  // can cause macros to get expanded, which can then cause new members ot get
  // added to 'TopLevelValues', invalidating iterator over `TopLevelDecls` in
  // `SourceLookupCache::lookupVisibleDecls`.
  //
  // Technically `foundDecl` should not expand macros or discover new top level
  // members in any way because those newly discovered decls will not be added
  // to the code completion results. However, it's preferrable to miss results
  // than to silently invalidate a collection, resulting in hard-to-diagnose
  // crashes.
  // Thus, store all the decls found by `CurrModule->lookupVisibleDecls` in a
  // vector first and only call `this->foundDecl` once we have left the
  // iteration loop over `TopLevelDecls`.
  StoringDeclConsumer StoringConsumer;

  UsableFilteringDeclConsumer UsableFilteringConsumer(
      Ctx.SourceMgr, CurrDeclContext, Ctx.SourceMgr.getIDEInspectionTargetLoc(),
      StoringConsumer);
  AccessFilteringDeclConsumer AccessFilteringConsumer(CurrDeclContext,
                                                      UsableFilteringConsumer);

  CodeCompletionMacroRoles ExpectedRoles = getCompletionMacroRoles(Filter);
  DeclFilter VisibleFilter =
      [ExpectedRoles](ValueDecl *VD, DeclVisibilityKind Kind,
                      DynamicLookupInfo DynamicLookupInfo) {
        CodeCompletionMacroRoles Roles = getCompletionMacroRoles(VD);
        if (!ExpectedRoles)
          return !Roles;
        return (bool)(Roles & ExpectedRoles);
      };

  FilteredDeclConsumer FilteringConsumer(AccessFilteringConsumer,
                                         VisibleFilter);

  CurrModule->lookupVisibleDecls({}, FilteringConsumer,
                                 NLKind::UnqualifiedLookup);

  StoringConsumer.forward(*this);
}

void CompletionLookup::lookupExternalModuleDecls(
    const ModuleDecl *TheModule, ArrayRef<std::string> AccessPath,
    bool ResultsHaveLeadingDot) {
  assert(CurrModule != TheModule && "requested module should be external");

  Kind = LookupKind::ImportFromModule;
  NeedLeadingDot = ResultsHaveLeadingDot;

  ImportPath::Access::Builder builder;
  for (auto Piece : AccessPath) {
    builder.push_back(Ctx.getIdentifier(Piece));
  }

  AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this);
  TheModule->lookupVisibleDecls(builder.get(), FilteringConsumer,
                                NLKind::UnqualifiedLookup);

  llvm::SmallVector<PrecedenceGroupDecl *, 16> precedenceGroups;
  TheModule->getPrecedenceGroups(precedenceGroups);

  for (auto PGD : precedenceGroups)
    addPrecedenceGroupRef(PGD);
}

void CompletionLookup::getStmtLabelCompletions(SourceLoc Loc, bool isContinue) {
  class LabelFinder : public ASTWalker {
    SourceManager &SM;
    SourceLoc TargetLoc;
    bool IsContinue;

  public:
    SmallVector<Identifier, 2> Result;

    /// Walk only the arguments of a macro.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    LabelFinder(SourceManager &SM, SourceLoc TargetLoc, bool IsContinue)
        : SM(SM), TargetLoc(TargetLoc), IsContinue(IsContinue) {}

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      if (SM.isBeforeInBuffer(S->getEndLoc(), TargetLoc))
        return Action::SkipChildren(S);

      if (LabeledStmt *LS = dyn_cast<LabeledStmt>(S)) {
        if (LS->getLabelInfo()) {
          if (!IsContinue || LS->isPossibleContinueTarget()) {
            auto label = LS->getLabelInfo().Name;
            if (!llvm::is_contained(Result, label))
              Result.push_back(label);
          }
        }
      }

      return Action::Continue(S);
    }

    PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
      return Action::Stop();
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (SM.isBeforeInBuffer(E->getEndLoc(), TargetLoc))
        return Action::SkipChildren(E);
      return Action::Continue(E);
    }
  } Finder(CurrDeclContext->getASTContext().SourceMgr, Loc, isContinue);
  const_cast<DeclContext *>(CurrDeclContext)->walkContext(Finder);
  for (auto name : Finder.Result) {
    CodeCompletionResultBuilder Builder = makeResultBuilder(
        CodeCompletionResultKind::Pattern, SemanticContextKind::Local);
    Builder.addTextChunk(name.str());
  }
}

void CompletionLookup::getOptionalBindingCompletions(SourceLoc Loc) {
  ExprType = Type();
  Kind = LookupKind::ValueInDeclContext;
  NeedLeadingDot = false;

  AccessFilteringDeclConsumer AccessFilteringConsumer(CurrDeclContext, *this);

  // Suggest only 'Optional' type var decls (incl. parameters)
  FilteredDeclConsumer FilteringConsumer(
      AccessFilteringConsumer,
      [&](ValueDecl *VD, DeclVisibilityKind Reason,
          DynamicLookupInfo dynamicLookupInfo) -> bool {
        auto *VarD = dyn_cast<VarDecl>(VD);
        if (!VarD)
          return false;

        auto Ty = getTypeOfMember(VD, dynamicLookupInfo);
        return Ty->isOptional();
      });

  // FIXME: Currently, it doesn't include top level decls for performance
  // reason. Enabling 'IncludeTopLevel' pulls everything including imported
  // modules. For suggesting top level results, we need a way to filter cached
  // results.

  lookupVisibleDecls(FilteringConsumer, CurrDeclContext,
                     /*IncludeTopLevel=*/false, Loc);
}

void CompletionLookup::getWithoutConstraintTypes() {
  // FIXME: Once we have a typealias declaration for copyable, we should be
  // returning that instead of a keyword (rdar://109107817).
  addKeyword("Copyable");
}

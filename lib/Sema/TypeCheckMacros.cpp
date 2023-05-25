//===--- TypeCheckMacros.cpp -  Macro Handling ----------------------------===//
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
// This file implements support for the evaluation of macros.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckMacros.h"
#include "../AST/InlinableText.h"
#include "TypeChecker.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/CASTBridging.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Expr.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Parse/Lexer.h"
#include "swift/Subsystems.h"
#include "llvm/Config/config.h"

using namespace swift;

extern "C" void *swift_ASTGen_resolveMacroType(const void *macroType);
extern "C" void swift_ASTGen_destroyMacro(void *macro);

extern "C" void *swift_ASTGen_resolveExecutableMacro(
    const char *moduleName, ptrdiff_t moduleNameLength,
    const char *typeName, ptrdiff_t typeNameLength,
    void * opaquePluginHandle);
extern "C" void swift_ASTGen_destroyExecutableMacro(void *macro);

extern "C" ptrdiff_t swift_ASTGen_checkMacroDefinition(
    void *diagEngine,
    void *sourceFile,
    const void *macroSourceLocation,
    char **expansionSourcePtr,
    ptrdiff_t *expansionSourceLength,
    ptrdiff_t **replacementsPtr,
    ptrdiff_t *numReplacements
);

extern "C" ptrdiff_t swift_ASTGen_expandFreestandingMacro(
    void *diagEngine, void *macro, uint8_t externalKind,
    const char *discriminator, ptrdiff_t discriminatorLength, void *sourceFile,
    const void *sourceLocation, const char **evaluatedSource,
    ptrdiff_t *evaluatedSourceLength);

extern "C" ptrdiff_t swift_ASTGen_expandAttachedMacro(
    void *diagEngine, void *macro, uint8_t externalKind,
    const char *discriminator, ptrdiff_t discriminatorLength,
    uint8_t rawMacroRole,
    void *customAttrSourceFile, const void *customAttrSourceLocation,
    void *declarationSourceFile, const void *declarationSourceLocation,
    void *parentDeclSourceFile, const void *parentDeclSourceLocation,
    const char **evaluatedSource, ptrdiff_t *evaluatedSourceLength);

extern "C" bool swift_ASTGen_initializePlugin(void *handle, void *diagEngine);
extern "C" void swift_ASTGen_deinitializePlugin(void *handle);
extern "C" bool swift_ASTGen_pluginServerLoadLibraryPlugin(
    void *handle, const char *libraryPath, const char *moduleName,
    void *diagEngine);

#if SWIFT_SWIFT_PARSER
/// Look for macro's type metadata given its external module and type name.
static void const *
lookupMacroTypeMetadataByExternalName(ASTContext &ctx, StringRef moduleName,
                                      StringRef typeName,
                                      LoadedLibraryPlugin *plugin) {
  // Look up the type metadata accessor as a struct, enum, or class.
  const Demangle::Node::Kind typeKinds[] = {
    Demangle::Node::Kind::Structure,
    Demangle::Node::Kind::Enum,
    Demangle::Node::Kind::Class
  };

  void *accessorAddr = nullptr;
  for (auto typeKind : typeKinds) {
    auto symbolName = Demangle::mangledNameForTypeMetadataAccessor(
        moduleName, typeName, typeKind);
    accessorAddr = plugin->getAddressOfSymbol(symbolName.c_str());
    if (accessorAddr)
      break;
  }

  if (!accessorAddr)
    return nullptr;

  // Call the accessor to form type metadata.
  using MetadataAccessFunc = const void *(MetadataRequest);
  auto accessor = reinterpret_cast<MetadataAccessFunc*>(accessorAddr);
  return accessor(MetadataRequest(MetadataState::Complete));
}
#endif

/// Translate an argument provided as a string literal into an identifier,
/// or return \c None and emit an error if it cannot be done.
Optional<Identifier> getIdentifierFromStringLiteralArgument(
    ASTContext &ctx, MacroExpansionExpr *expansion, unsigned index) {
  auto argList = expansion->getArgs();

  // If there's no argument here, an error was diagnosed elsewhere.
  if (!argList || index >= argList->size()) {
    return None;
  }

  auto arg = argList->getExpr(index);
  auto stringLiteral = dyn_cast<StringLiteralExpr>(arg);
  if (!stringLiteral) {
    ctx.Diags.diagnose(
        arg->getLoc(), diag::external_macro_arg_not_type_name, index
    );

    return None;
  }


  auto contents = stringLiteral->getValue();
  if (!Lexer::isIdentifier(contents)) {
    ctx.Diags.diagnose(
        arg->getLoc(), diag::external_macro_arg_not_type_name, index
    );

    return None;
  }

  return ctx.getIdentifier(contents);
}

/// For a macro expansion expression that is known to be #externalMacro,
/// handle the definition.
static MacroDefinition  handleExternalMacroDefinition(
    ASTContext &ctx, MacroExpansionExpr *expansion) {
  // Dig out the module and type name.
  auto moduleName = getIdentifierFromStringLiteralArgument(ctx, expansion, 0);
  if (!moduleName) {
    return MacroDefinition::forInvalid();
  }

  auto typeName = getIdentifierFromStringLiteralArgument(ctx, expansion, 1);
  if (!typeName) {
    return MacroDefinition::forInvalid();
  }

  return MacroDefinition::forExternal(*moduleName, *typeName);
}

MacroDefinition MacroDefinitionRequest::evaluate(
    Evaluator &evaluator, MacroDecl *macro
) const {
  ASTContext &ctx = macro->getASTContext();

  // If no definition was provided, the macro is... undefined, of course.
  auto definition = macro->definition;
  if (!definition)
    return MacroDefinition::forUndefined();

  auto sourceFile = macro->getParentSourceFile();

#if SWIFT_SWIFT_PARSER
  char *externalMacroNamePtr;
  ptrdiff_t externalMacroNameLength;
  ptrdiff_t *replacements;
  ptrdiff_t numReplacements;
  auto checkResult = swift_ASTGen_checkMacroDefinition(
      &ctx.Diags, sourceFile->getExportedSourceFile(),
      macro->getLoc().getOpaquePointerValue(), &externalMacroNamePtr,
      &externalMacroNameLength, &replacements, &numReplacements);

  // Clean up after the call.
  SWIFT_DEFER {
    free(externalMacroNamePtr);
    free(replacements);
  };

  if (checkResult < 0 && ctx.CompletionCallback) {
    // If the macro failed to check and we are in code completion mode, pretend
    // it's an arbitrary macro. This allows us to get call argument completions
    // inside `#externalMacro`.
    checkResult = BridgedMacroDefinitionKind::BridgedExpandedMacro;
  }

  if (checkResult < 0)
    return MacroDefinition::forInvalid();

  switch (static_cast<BridgedMacroDefinitionKind>(checkResult)) {
  case BridgedExpandedMacro:
    // Handle expanded macros below.
    break;

  case BridgedExternalMacro: {
    // An external macro described as ModuleName.TypeName. Get both identifiers.
    assert(!replacements && "External macro doesn't have replacements");
    StringRef externalMacroStr(externalMacroNamePtr, externalMacroNameLength);
    StringRef externalModuleName, externalTypeName;
    std::tie(externalModuleName, externalTypeName) = externalMacroStr.split('.');

    Identifier moduleName = ctx.getIdentifier(externalModuleName);
    Identifier typeName = ctx.getIdentifier(externalTypeName);
    return MacroDefinition::forExternal(moduleName, typeName);
  }

  case BridgedBuiltinExternalMacro:
    return MacroDefinition::forBuiltin(BuiltinMacroKind::ExternalMacro);
  }

  // Type-check the macro expansion.
  Type resultType = macro->mapTypeIntoContext(macro->getResultInterfaceType());

  constraints::ContextualTypeInfo contextualType {
    TypeLoc::withoutLoc(resultType),
    // FIXME: Add a contextual type purpose for macro definition checking.
    ContextualTypePurpose::CTP_CoerceOperand
  };

  PrettyStackTraceDecl debugStack("type checking macro definition", macro);
  Type typeCheckedType = TypeChecker::typeCheckExpression(
      definition, macro, contextualType,
      TypeCheckExprFlags::DisableMacroExpansions);
  if (!typeCheckedType)
    return MacroDefinition::forInvalid();

  // Dig out the macro that was expanded.
  auto expansion = cast<MacroExpansionExpr>(definition);
  auto expandedMacro =
      dyn_cast_or_null<MacroDecl>(expansion->getMacroRef().getDecl());
  if (!expandedMacro)
    return MacroDefinition::forInvalid();

  // Handle external macros after type-checking.
  auto builtinKind = expandedMacro->getBuiltinKind();
  if (builtinKind == BuiltinMacroKind::ExternalMacro)
    return handleExternalMacroDefinition(ctx, expansion);

  // Expansion string text.
  StringRef expansionText(externalMacroNamePtr, externalMacroNameLength);

  // Copy over the replacements.
  SmallVector<ExpandedMacroReplacement, 2> replacementsVec;
  for (unsigned i: range(0, numReplacements)) {
    replacementsVec.push_back(
        { static_cast<unsigned>(replacements[3*i]),
          static_cast<unsigned>(replacements[3*i+1]),
          static_cast<unsigned>(replacements[3*i+2])});
  }

  return MacroDefinition::forExpanded(ctx, expansionText, replacementsVec);
#else
  macro->diagnose(diag::macro_unsupported);
  return MacroDefinition::forInvalid();
#endif
}

static LoadedExecutablePlugin *
initializeExecutablePlugin(ASTContext &ctx,
                           LoadedExecutablePlugin *executablePlugin,
                           StringRef libraryPath, Identifier moduleName) {
  // Lock the plugin while initializing.
  // Note that'executablePlugn' can be shared between multiple ASTContext.
  executablePlugin->lock();
  SWIFT_DEFER { executablePlugin->unlock(); };

  // FIXME: Ideally this should be done right after invoking the plugin.
  // But plugin loading is in libAST and it can't link ASTGen symbols.
  if (!executablePlugin->isInitialized()) {
#if SWIFT_SWIFT_PARSER
    if (!swift_ASTGen_initializePlugin(executablePlugin, &ctx.Diags)) {
      return nullptr;
    }
    executablePlugin->setCleanup([executablePlugin] {
      swift_ASTGen_deinitializePlugin(executablePlugin);
    });
#endif
  }

  // If this is a plugin server, load the library.
  if (!libraryPath.empty()) {
#if SWIFT_SWIFT_PARSER
    llvm::SmallString<128> resolvedLibraryPath;
    auto fs = ctx.SourceMgr.getFileSystem();
    if (auto err = fs->getRealPath(libraryPath, resolvedLibraryPath)) {
      ctx.Diags.diagnose(SourceLoc(), diag::compiler_plugin_not_loaded,
                         executablePlugin->getExecutablePath(), err.message());
      return nullptr;
    }
    std::string resolvedLibraryPathStr(resolvedLibraryPath);
    std::string moduleNameStr(moduleName.str());

    bool loaded = swift_ASTGen_pluginServerLoadLibraryPlugin(
        executablePlugin, resolvedLibraryPathStr.c_str(), moduleNameStr.c_str(),
        &ctx.Diags);
    if (!loaded)
      return nullptr;

    // Set a callback to load the library again on reconnections.
    auto *callback = new std::function<void(void)>(
        [executablePlugin, resolvedLibraryPathStr, moduleNameStr]() {
          (void)swift_ASTGen_pluginServerLoadLibraryPlugin(
              executablePlugin, resolvedLibraryPathStr.c_str(),
              moduleNameStr.c_str(),
              /*diags=*/nullptr);
        });
    executablePlugin->addOnReconnect(callback);

    // Remove the callback and deallocate it when this ASTContext is destructed.
    ctx.addCleanup([executablePlugin, callback]() {
      executablePlugin->removeOnReconnect(callback);
      delete callback;
    });
#endif
  }

  return executablePlugin;
}

LoadedCompilerPlugin
CompilerPluginLoadRequest::evaluate(Evaluator &evaluator, ASTContext *ctx,
                                    Identifier moduleName) const {
  PluginLoader &loader = ctx->getPluginLoader();

  std::string libraryPath;
  std::string executablePath;

  // '-load-plugin-libarary'.
  if (auto found = loader.lookupExplicitLibraryPluginByModuleName(moduleName)) {
    libraryPath = found.value();
  }
  // '-load-plugin-executable'.
  else if (auto found = loader.lookupExecutablePluginByModuleName(moduleName)) {
    executablePath = found->str();
  }
  // '-plugin-path'.
  else if (auto found =
               loader.lookupLibraryPluginInSearchPathByModuleName(moduleName)) {
    libraryPath = found.value();
  }
  // '-external-plugin-path'.
  else if (auto found =
               loader.lookupExternalLibraryPluginByModuleName(moduleName)) {
    std::tie(libraryPath, executablePath) = found.value();
  }

  if (!executablePath.empty()) {
    if (LoadedExecutablePlugin *executablePlugin =
            loader.loadExecutablePlugin(executablePath)) {
      return initializeExecutablePlugin(*ctx, executablePlugin, libraryPath,
                                        moduleName);
    }
  } else if (!libraryPath.empty()) {
    if (LoadedLibraryPlugin *libraryPlugin =
            loader.loadLibraryPlugin(libraryPath)) {
      return libraryPlugin;
    }
  }

  return nullptr;
}

static Optional<ExternalMacroDefinition>
resolveInProcessMacro(ASTContext &ctx, Identifier moduleName,
                      Identifier typeName, LoadedLibraryPlugin *plugin) {
#if SWIFT_SWIFT_PARSER
  /// Look for the type metadata given the external module and type names.
  auto macroMetatype = lookupMacroTypeMetadataByExternalName(
      ctx, moduleName.str(), typeName.str(), plugin);
  if (macroMetatype) {
    // Check whether the macro metatype is in-process.
    if (auto inProcess = swift_ASTGen_resolveMacroType(macroMetatype)) {
      // Make sure we clean up after the macro.
      ctx.addCleanup([inProcess]() {
        swift_ASTGen_destroyMacro(inProcess);
      });

      return ExternalMacroDefinition{
          ExternalMacroDefinition::PluginKind::InProcess, inProcess};
    }
  }
#endif
  return None;
}

static Optional<ExternalMacroDefinition>
resolveExecutableMacro(ASTContext &ctx,
                       LoadedExecutablePlugin *executablePlugin,
                       Identifier moduleName, Identifier typeName) {
#if SWIFT_SWIFT_PARSER
  if (auto *execMacro = swift_ASTGen_resolveExecutableMacro(
          moduleName.str().data(), moduleName.str().size(),
          typeName.str().data(), typeName.str().size(), executablePlugin)) {
    // Make sure we clean up after the macro.
    ctx.addCleanup(
        [execMacro]() { swift_ASTGen_destroyExecutableMacro(execMacro); });
    return ExternalMacroDefinition{
        ExternalMacroDefinition::PluginKind::Executable, execMacro};
  }
#endif
  return None;
}

Optional<ExternalMacroDefinition>
ExternalMacroDefinitionRequest::evaluate(Evaluator &evaluator, ASTContext *ctx,
                                         Identifier moduleName,
                                         Identifier typeName) const {
  // Try to load a plugin module from the plugin search paths. If it
  // succeeds, resolve in-process from that plugin
  CompilerPluginLoadRequest loadRequest{ctx, moduleName};
  LoadedCompilerPlugin loaded =
      evaluateOrDefault(evaluator, loadRequest, nullptr);

  if (auto loadedLibrary = loaded.getAsLibraryPlugin()) {
    if (auto inProcess = resolveInProcessMacro(
            *ctx, moduleName, typeName, loadedLibrary))
      return *inProcess;
  }

  if (auto *executablePlugin = loaded.getAsExecutablePlugin()) {
    if (auto executableMacro = resolveExecutableMacro(*ctx, executablePlugin,
                                                      moduleName, typeName)) {
      return executableMacro;
    }
  }

  return None;
}

/// Adjust the given mangled name for a macro expansion to produce a valid
/// buffer name.
static std::string adjustMacroExpansionBufferName(StringRef name) {
  if (name.empty()) {
    return "<macro-expansion>";
  }
  std::string result;
  if (name.startswith(MANGLING_PREFIX_STR)) {
    result += MACRO_EXPANSION_BUFFER_MANGLING_PREFIX;
    name = name.drop_front(StringRef(MANGLING_PREFIX_STR).size());
  }

  result += name;
  result += ".swift";
  return result;
}

Optional<unsigned>
ExpandMacroExpansionExprRequest::evaluate(Evaluator &evaluator,
                                          MacroExpansionExpr *mee) const {
  ConcreteDeclRef macroRef = mee->getMacroRef();
  assert(macroRef && isa<MacroDecl>(macroRef.getDecl()) &&
         "MacroRef should be set before expansion");

  auto *macro = cast<MacroDecl>(macroRef.getDecl());
  if (macro->getMacroRoles().contains(MacroRole::Expression)) {
    return expandMacroExpr(mee);
  }
  // For a non-expression macro, expand it as a declaration.
  else if (macro->getMacroRoles().contains(MacroRole::Declaration) ||
           macro->getMacroRoles().contains(MacroRole::CodeItem)) {
    if (!mee->getSubstituteDecl()) {
      auto *med = mee->createSubstituteDecl();
      TypeChecker::typeCheckDecl(med);
    }
    // Return the expanded buffer ID.
    return evaluateOrDefault(
        evaluator, ExpandMacroExpansionDeclRequest(mee->getSubstituteDecl()),
        None);
  }

  // Other macro roles may also be encountered here, as they use
  // `MacroExpansionExpr` for resolution. In those cases, do not expand.
  return None;
}

ArrayRef<unsigned> ExpandMemberAttributeMacros::evaluate(Evaluator &evaluator,
                                                         Decl *decl) const {
  if (decl->isImplicit())
    return { };

  auto *parentDecl = decl->getDeclContext()->getAsDecl();
  if (!parentDecl || !isa<IterableDeclContext>(parentDecl))
    return { };

  if (isa<PatternBindingDecl>(decl))
    return { };

  SmallVector<unsigned, 2> bufferIDs;
  parentDecl->forEachAttachedMacro(MacroRole::MemberAttribute,
      [&](CustomAttr *attr, MacroDecl *macro) {
        if (auto bufferID = expandAttributes(attr, macro, decl))
          bufferIDs.push_back(*bufferID);
      });

  return parentDecl->getASTContext().AllocateCopy(bufferIDs);
}

ArrayRef<unsigned> ExpandSynthesizedMemberMacroRequest::evaluate(
    Evaluator &evaluator, Decl *decl
) const {
  SmallVector<unsigned, 2> bufferIDs;
  decl->forEachAttachedMacro(MacroRole::Member,
      [&](CustomAttr *attr, MacroDecl *macro) {
        if (auto bufferID = expandMembers(attr, macro, decl))
          bufferIDs.push_back(*bufferID);
      });

  return decl->getASTContext().AllocateCopy(bufferIDs);
}

ArrayRef<unsigned>
ExpandPeerMacroRequest::evaluate(Evaluator &evaluator, Decl *decl) const {
  SmallVector<unsigned, 2> bufferIDs;
  decl->forEachAttachedMacro(MacroRole::Peer,
      [&](CustomAttr *attr, MacroDecl *macro) {
        if (auto bufferID = expandPeers(attr, macro, decl))
          bufferIDs.push_back(*bufferID);
      });

  return decl->getASTContext().AllocateCopy(bufferIDs);
}

static Identifier makeIdentifier(ASTContext &ctx, StringRef name) {
  return ctx.getIdentifier(name);
}

static Identifier makeIdentifier(ASTContext &ctx, std::nullptr_t) {
  return Identifier();
}

/// Diagnose macro expansions that produce any of the following declarations:
///   - Import declarations
///   - Operator and precedence group declarations
///   - Macro declarations
///   - Extensions
///   - Types with `@main` attributes
///   - Top-level default literal type overrides
///   - Value decls with names not covered by the macro declaration.
static void validateMacroExpansion(SourceFile *expansionBuffer,
                                   MacroDecl *macro,
                                   ValueDecl *attachedTo,
                                   MacroRole role) {
  // Gather macro-introduced names
  llvm::SmallVector<DeclName, 2> introducedNames;
  macro->getIntroducedNames(role, attachedTo, introducedNames);

  llvm::SmallDenseSet<DeclName, 2> coversName(introducedNames.begin(),
                                              introducedNames.end());

  for (auto *decl : expansionBuffer->getTopLevelDecls()) {
    auto &ctx = decl->getASTContext();

    // Certain macro roles can generate special declarations.
    if ((isa<AccessorDecl>(decl) && role == MacroRole::Accessor) ||
        (isa<ExtensionDecl>(decl) && role == MacroRole::Conformance)) {
      continue;
    }

    // Diagnose invalid declaration kinds.
    if (isa<ImportDecl>(decl) ||
        isa<OperatorDecl>(decl) ||
        isa<PrecedenceGroupDecl>(decl) ||
        isa<MacroDecl>(decl) ||
        isa<ExtensionDecl>(decl)) {
      decl->diagnose(diag::invalid_decl_in_macro_expansion,
                     decl->getDescriptiveKind());
      decl->setInvalid();

      if (auto *extension = dyn_cast<ExtensionDecl>(decl)) {
        extension->setExtendedNominal(nullptr);
      }

      continue;
    }

    // Diagnose `@main` types.
    if (auto *mainAttr = decl->getAttrs().getAttribute<MainTypeAttr>()) {
      ctx.Diags.diagnose(mainAttr->getLocation(),
                         diag::invalid_main_type_in_macro_expansion);
      mainAttr->setInvalid();
    }

    // Diagnose default literal type overrides.
    if (auto *typeAlias = dyn_cast<TypeAliasDecl>(decl)) {
      auto name = typeAlias->getBaseIdentifier();
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(_, __, typeName,     \
                                                  supportsOverride)    \
      if (supportsOverride && name == makeIdentifier(ctx, typeName)) { \
        typeAlias->diagnose(diag::literal_type_in_macro_expansion,     \
                            makeIdentifier(ctx, typeName));            \
        typeAlias->setInvalid();                                       \
        continue;                                                      \
      }
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME
    }

    // Diagnose value decls with names not covered by the macro
    if (auto *value = dyn_cast<ValueDecl>(decl)) {
      auto name = value->getName();

      // Unique names are always permitted.
      if (MacroDecl::isUniqueMacroName(name.getBaseName().userFacingName()))
        continue;

      if (coversName.count(name) ||
          coversName.count(name.getBaseName()) ||
          coversName.count(MacroDecl::getArbitraryName())) {
        continue;
      }

      value->diagnose(diag::invalid_macro_introduced_name,
                      name, macro->getBaseName());
    }
  }
}

/// Determine whether the given source file is from an expansion of the given
/// macro.
static bool isFromExpansionOfMacro(SourceFile *sourceFile, MacroDecl *macro,
                                   MacroRole role) {
  while (sourceFile) {
    auto expansion = sourceFile->getMacroExpansion();
    if (!expansion)
      return false;

    if (auto expansionExpr = dyn_cast_or_null<MacroExpansionExpr>(
            expansion.dyn_cast<Expr *>())) {
      if (expansionExpr->getMacroRef().getDecl() == macro)
        return true;
    } else if (auto expansionDecl = dyn_cast_or_null<MacroExpansionDecl>(
            expansion.dyn_cast<Decl *>())) {
      if (expansionDecl->getMacroRef().getDecl() == macro)
        return true;
    } else if (auto *macroAttr = sourceFile->getAttachedMacroAttribute()) {
      auto *decl = expansion.dyn_cast<Decl *>();
      auto *macroDecl = decl->getResolvedMacro(macroAttr);
      if (!macroDecl)
        return false;

      return macroDecl == macro &&
             sourceFile->getFulfilledMacroRole() == role;
    } else {
      llvm_unreachable("Unknown macro expansion node kind");
    }

    sourceFile = sourceFile->getEnclosingSourceFile();
  }

  return false;
}

/// Expand a macro definition.
static std::string expandMacroDefinition(
    ExpandedMacroDefinition def, MacroDecl *macro, ArgumentList *args) {
  ASTContext &ctx = macro->getASTContext();

  std::string expandedResult;

  StringRef originalText = def.getExpansionText();
  unsigned startIdx = 0;
  for (const auto replacement: def.getReplacements()) {
    // Add the original text up to the first replacement.
    expandedResult.append(
        originalText.begin() + startIdx,
        originalText.begin() + replacement.startOffset);

    // Add the replacement text.
    auto argExpr = args->getArgExprs()[replacement.parameterIndex];
    SmallString<32> argTextBuffer;
    auto argText = extractInlinableText(ctx.SourceMgr, argExpr, argTextBuffer);
    expandedResult.append(argText);

    // Update the starting position.
    startIdx = replacement.endOffset;
  }

  // Add the remaining text.
  expandedResult.append(
      originalText.begin() + startIdx,
      originalText.end());

  return expandedResult;
}

Optional<unsigned>
swift::expandMacroExpr(MacroExpansionExpr *mee) {
  DeclContext *dc = mee->getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;
  ConcreteDeclRef macroRef = mee->getMacroRef();
  Type expandedType = mee->getType();

  auto moduleDecl = dc->getParentModule();
  auto sourceFile = moduleDecl->getSourceFileContainingLocation(mee->getLoc());
  if (!sourceFile)
    return None;

  MacroDecl *macro = cast<MacroDecl>(macroRef.getDecl());

  if (isFromExpansionOfMacro(sourceFile, macro, MacroRole::Expression)) {
    ctx.Diags.diagnose(mee->getLoc(), diag::macro_recursive, macro->getName());
    return None;
  }

  // Evaluate the macro.
  std::unique_ptr<llvm::MemoryBuffer> evaluatedSource;

  /// The discriminator used for the macro.
  LazyValue<std::string> discriminator([&]() -> std::string {
#if SWIFT_SWIFT_PARSER
    Mangle::ASTMangler mangler;
    return mangler.mangleMacroExpansion(mee);
#else
    return "";
#endif
  });

  auto macroDef = macro->getDefinition();
  switch (macroDef.kind) {
  case MacroDefinition::Kind::Undefined:
  case MacroDefinition::Kind::Invalid:
    // Already diagnosed as an error elsewhere.
    return None;

  case MacroDefinition::Kind::Builtin: {
    switch (macroDef.getBuiltinKind()) {
    case BuiltinMacroKind::ExternalMacro:
      ctx.Diags.diagnose(
          mee->getLoc(), diag::external_macro_outside_macro_definition);
      return None;
    }
  }

  case MacroDefinition::Kind::Expanded: {
    // Expand the definition with the given arguments.
    auto result = expandMacroDefinition(
        macroDef.getExpanded(), macro, mee->getArgs());
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        result, adjustMacroExpansionBufferName(*discriminator));
    break;
  }

  case MacroDefinition::Kind::External: {
    // Retrieve the external definition of the macro.
    auto external = macroDef.getExternalMacro();
    ExternalMacroDefinitionRequest request{
      &ctx, external.moduleName, external.macroTypeName
    };
    auto externalDef = evaluateOrDefault(ctx.evaluator, request, None);
    if (!externalDef) {
      ctx.Diags.diagnose(
          mee->getLoc(), diag::external_macro_not_found,
          external.moduleName.str(),
          external.macroTypeName.str(),
          macro->getName()
      );
      macro->diagnose(diag::decl_declared_here, macro->getName());
      return None;
    }

#if SWIFT_SWIFT_PARSER
    PrettyStackTraceExpr debugStack(ctx, "expanding macro", mee);

    // Builtin macros are handled via ASTGen.
    auto *astGenSourceFile = sourceFile->getExportedSourceFile();
    if (!astGenSourceFile)
      return None;

    const char *evaluatedSourceAddress;
    ptrdiff_t evaluatedSourceLength;
    swift_ASTGen_expandFreestandingMacro(
        &ctx.Diags, externalDef->opaqueHandle,
        static_cast<uint32_t>(externalDef->kind), discriminator->data(),
        discriminator->size(), astGenSourceFile,
        mee->getStartLoc().getOpaquePointerValue(), &evaluatedSourceAddress,
        &evaluatedSourceLength);
    if (!evaluatedSourceAddress)
      return None;
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        {evaluatedSourceAddress, (size_t)evaluatedSourceLength},
        adjustMacroExpansionBufferName(*discriminator));
    free((void *)evaluatedSourceAddress);
    break;
#else
    ctx.Diags.diagnose(mee->getLoc(), diag::macro_unsupported);
    return None;
#endif
  }
  }

  // Dump macro expansions to standard output, if requested.
  if (ctx.LangOpts.DumpMacroExpansions) {
    llvm::errs() << evaluatedSource->getBufferIdentifier() << " as "
                 << expandedType.getString()
                 << "\n------------------------------\n"
                 << evaluatedSource->getBuffer()
                 << "\n------------------------------\n";
  }

  // Create a new source buffer with the contents of the expanded macro.
  unsigned macroBufferID =
      sourceMgr.addNewSourceBuffer(std::move(evaluatedSource));
  auto macroBufferRange = sourceMgr.getRangeForBuffer(macroBufferID);
  GeneratedSourceInfo sourceInfo{
    GeneratedSourceInfo::ExpressionMacroExpansion,
    Lexer::getCharSourceRangeFromSourceRange(
      sourceMgr, mee->getSourceRange()),
    macroBufferRange,
    ASTNode(mee).getOpaqueValue(),
    dc
  };
  sourceMgr.setGeneratedSourceInfo(macroBufferID, sourceInfo);

  // Create a source file to hold the macro buffer. This is automatically
  // registered with the enclosing module.
  auto macroSourceFile = new (ctx) SourceFile(
      *dc->getParentModule(), SourceFileKind::MacroExpansion, macroBufferID,
      /*parsingOpts=*/{}, /*isPrimary=*/false);
  macroSourceFile->setImports(sourceFile->getImports());

  // Retrieve the parsed expression from the list of top-level items.
  auto topLevelItems = macroSourceFile->getTopLevelItems();
  Expr *expandedExpr = nullptr;
  if (topLevelItems.size() != 1) {
    ctx.Diags.diagnose(
        macroBufferRange.getStart(), diag::expected_macro_expansion_expr);
    return macroBufferID;
  }

  auto codeItem = topLevelItems.front();
  if (auto *expr = codeItem.dyn_cast<Expr *>())
    expandedExpr = expr;

  if (!expandedExpr) {
    ctx.Diags.diagnose(
        macroBufferRange.getStart(), diag::expected_macro_expansion_expr);
    return macroBufferID;
  }

  // Type-check the expanded expression.
  // FIXME: Would like to pass through type checking options like "discarded"
  // that are captured by TypeCheckExprOptions.
  constraints::ContextualTypeInfo contextualType {
    TypeLoc::withoutLoc(expandedType),
    // FIXME: Add a contextual type purpose for macro expansion.
    ContextualTypePurpose::CTP_CoerceOperand
  };

  PrettyStackTraceExpr debugStack(
      ctx, "type checking expanded macro", expandedExpr);
  Type realExpandedType = TypeChecker::typeCheckExpression(
      expandedExpr, dc, contextualType);
  if (!realExpandedType)
    return macroBufferID;

  assert((expandedType->isEqual(realExpandedType) ||
          realExpandedType->hasError()) &&
         "Type checking changed the result type?");

  mee->setRewritten(expandedExpr);

  return macroBufferID;
}

/// Expands the given macro expansion declaration.
Optional<unsigned>
swift::expandFreestandingMacro(MacroExpansionDecl *med) {
  auto *dc = med->getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;

  auto moduleDecl = dc->getParentModule();
  auto sourceFile = moduleDecl->getSourceFileContainingLocation(med->getLoc());
  if (!sourceFile)
    return None;

  MacroDecl *macro = cast<MacroDecl>(med->getMacroRef().getDecl());
  auto macroRoles = macro->getMacroRoles();
  assert(macroRoles.contains(MacroRole::Declaration) ||
         macroRoles.contains(MacroRole::CodeItem));

  if (isFromExpansionOfMacro(sourceFile, macro, MacroRole::Expression) ||
      isFromExpansionOfMacro(sourceFile, macro, MacroRole::Declaration) ||
      isFromExpansionOfMacro(sourceFile, macro, MacroRole::CodeItem)) {
    med->diagnose(diag::macro_recursive, macro->getName());
    return None;
  }

  // Evaluate the macro.
  std::unique_ptr<llvm::MemoryBuffer> evaluatedSource;

  /// The discriminator used for the macro.
  LazyValue<std::string> discriminator([&]() -> std::string {
#if SWIFT_SWIFT_PARSER
    Mangle::ASTMangler mangler;
    return mangler.mangleMacroExpansion(med);
#else
    return "";
#endif
  });

  auto macroDef = macro->getDefinition();
  switch (macroDef.kind) {
  case MacroDefinition::Kind::Undefined:
  case MacroDefinition::Kind::Invalid:
    // Already diagnosed as an error elsewhere.
    return None;

  case MacroDefinition::Kind::Builtin: {
    switch (macroDef.getBuiltinKind()) {
    case BuiltinMacroKind::ExternalMacro:
      // FIXME: Error here.
      return None;
    }
  }

  case MacroDefinition::Kind::Expanded: {
    // Expand the definition with the given arguments.
    auto result = expandMacroDefinition(
        macroDef.getExpanded(), macro, med->getArgs());
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        result, adjustMacroExpansionBufferName(*discriminator));
    break;
  }

  case MacroDefinition::Kind::External: {
    // Retrieve the external definition of the macro.
    auto external = macroDef.getExternalMacro();
    ExternalMacroDefinitionRequest request{
        &ctx, external.moduleName, external.macroTypeName
    };
    auto externalDef = evaluateOrDefault(ctx.evaluator, request, None);
    if (!externalDef) {
      med->diagnose(diag::external_macro_not_found,
                    external.moduleName.str(),
                    external.macroTypeName.str(),
                    macro->getName()
      );
      macro->diagnose(diag::decl_declared_here, macro->getName());
      return None;
    }

    // Currently only expression macros are enabled by default. Declaration
    // macros need the `FreestandingMacros` feature flag, and code item macros
    // need both `FreestandingMacros` and `CodeItemMacros`.
    if (!macroRoles.contains(MacroRole::Expression)) {
      if (!macroRoles.contains(MacroRole::Declaration) &&
          !ctx.LangOpts.hasFeature(Feature::CodeItemMacros)) {
        med->diagnose(diag::macro_experimental, "code item", "CodeItemMacros");
        return None;
      }
    }

#if SWIFT_SWIFT_PARSER
    PrettyStackTraceDecl debugStack("expanding declaration macro", med);

    // Builtin macros are handled via ASTGen.
    auto *astGenSourceFile = sourceFile->getExportedSourceFile();
    if (!astGenSourceFile)
      return None;

    const char *evaluatedSourceAddress;
    ptrdiff_t evaluatedSourceLength;
    swift_ASTGen_expandFreestandingMacro(
        &ctx.Diags, externalDef->opaqueHandle,
        static_cast<uint32_t>(externalDef->kind), discriminator->data(),
        discriminator->size(), astGenSourceFile,
        med->getStartLoc().getOpaquePointerValue(), &evaluatedSourceAddress,
        &evaluatedSourceLength);
    if (!evaluatedSourceAddress)
      return None;
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        {evaluatedSourceAddress, (size_t)evaluatedSourceLength},
        adjustMacroExpansionBufferName(*discriminator));
    free((void *)evaluatedSourceAddress);
    break;
#else
    med->diagnose(diag::macro_unsupported);
    return None;
#endif
  }
  }

  // Dump macro expansions to standard output, if requested.
  if (ctx.LangOpts.DumpMacroExpansions) {
    llvm::errs() << evaluatedSource->getBufferIdentifier()
                 << "\n------------------------------\n"
                 << evaluatedSource->getBuffer()
                 << "\n------------------------------\n";
  }

  // Create a new source buffer with the contents of the expanded macro.
  unsigned macroBufferID =
      sourceMgr.addNewSourceBuffer(std::move(evaluatedSource));
  auto macroBufferRange = sourceMgr.getRangeForBuffer(macroBufferID);
  GeneratedSourceInfo sourceInfo{
      GeneratedSourceInfo::FreestandingDeclMacroExpansion,
      Lexer::getCharSourceRangeFromSourceRange(
        sourceMgr, med->getSourceRange()),
      macroBufferRange,
      ASTNode(med).getOpaqueValue(),
      dc
  };
  sourceMgr.setGeneratedSourceInfo(macroBufferID, sourceInfo);

  // Create a source file to hold the macro buffer. This is automatically
  // registered with the enclosing module.
  auto macroSourceFile = new (ctx) SourceFile(
      *dc->getParentModule(), SourceFileKind::MacroExpansion, macroBufferID,
      /*parsingOpts=*/{}, /*isPrimary=*/false);
  macroSourceFile->setImports(sourceFile->getImports());

  validateMacroExpansion(macroSourceFile, macro,
                         /*attachedTo*/nullptr,
                         MacroRole::Declaration);

  PrettyStackTraceDecl debugStack(
      "type checking expanded declaration macro", med);

  auto topLevelItems = macroSourceFile->getTopLevelItems();
  for (auto item : topLevelItems) {
    if (auto *decl = item.dyn_cast<Decl *>())
      decl->setDeclContext(dc);
  }
  return macroBufferID;
}

// If this storage declaration is a variable with an explicit initializer,
// return the range from the `=` to the end of the explicit initializer.
static Optional<SourceRange> getExplicitInitializerRange(
    AbstractStorageDecl *storage) {
  auto var = dyn_cast<VarDecl>(storage);
  if (!var)
    return None;

  auto pattern = var->getParentPatternBinding();
  if (!pattern)
    return None;

  unsigned index = pattern->getPatternEntryIndexForVarDecl(var);
  SourceLoc equalLoc = pattern->getEqualLoc(index);
  SourceRange initRange = pattern->getOriginalInitRange(index);
  if (equalLoc.isInvalid() || initRange.End.isInvalid())
    return None;

  return SourceRange(equalLoc, initRange.End);
}

static SourceFile *
evaluateAttachedMacro(MacroDecl *macro, Decl *attachedTo, CustomAttr *attr,
                      bool passParentContext, MacroRole role) {
  DeclContext *dc;
  if (role == MacroRole::Peer) {
    dc = attachedTo->getDeclContext();
  } else if (role == MacroRole::Conformance) {
    // Conformance macros always expand to extensions at file-scope.
    dc = attachedTo->getDeclContext()->getParentSourceFile();
  } else {
    dc = attachedTo->getInnermostDeclContext();
  }

  ASTContext &ctx = dc->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;

  auto moduleDecl = dc->getParentModule();

  auto attrSourceFile =
    moduleDecl->getSourceFileContainingLocation(attr->AtLoc);
  if (!attrSourceFile)
    return nullptr;

  auto declSourceFile =
      moduleDecl->getSourceFileContainingLocation(attachedTo->getStartLoc());
  if (!declSourceFile)
    return nullptr;

  Decl *parentDecl = nullptr;
  SourceFile *parentDeclSourceFile = nullptr;
  if (passParentContext) {
    parentDecl = attachedTo->getDeclContext()->getAsDecl();
    if (!parentDecl)
      return nullptr;

    parentDeclSourceFile =
      moduleDecl->getSourceFileContainingLocation(parentDecl->getLoc());
    if (!parentDeclSourceFile)
      return nullptr;
  }

  if (isFromExpansionOfMacro(attrSourceFile, macro, role) ||
      isFromExpansionOfMacro(declSourceFile, macro, role) ||
      isFromExpansionOfMacro(parentDeclSourceFile, macro, role)) {
    attachedTo->diagnose(diag::macro_recursive, macro->getName());
    return nullptr;
  }

  // Evaluate the macro.
  std::unique_ptr<llvm::MemoryBuffer> evaluatedSource;

  /// The discriminator used for the macro.
  LazyValue<std::string> discriminator([&]() -> std::string {
#if SWIFT_SWIFT_PARSER
    Mangle::ASTMangler mangler;
    return mangler.mangleAttachedMacroExpansion(attachedTo, attr, role);
#else
    return "";
#endif
  });

  auto macroDef = macro->getDefinition();
  switch (macroDef.kind) {
  case MacroDefinition::Kind::Undefined:
  case MacroDefinition::Kind::Invalid:
    // Already diagnosed as an error elsewhere.
    return nullptr;

  case MacroDefinition::Kind::Builtin: {
    switch (macroDef.getBuiltinKind()) {
    case BuiltinMacroKind::ExternalMacro:
      // FIXME: Error here.
      return nullptr;
    }
  }

  case MacroDefinition::Kind::Expanded: {
    // Expand the definition with the given arguments.
    auto result = expandMacroDefinition(
        macroDef.getExpanded(), macro, attr->getArgs());
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        result, adjustMacroExpansionBufferName(*discriminator));
    break;
  }

  case MacroDefinition::Kind::External: {
    // Retrieve the external definition of the macro.
    auto external = macroDef.getExternalMacro();
    ExternalMacroDefinitionRequest request{
        &ctx, external.moduleName, external.macroTypeName
    };
    auto externalDef = evaluateOrDefault(ctx.evaluator, request, None);
    if (!externalDef) {
      attachedTo->diagnose(diag::external_macro_not_found,
                        external.moduleName.str(),
                        external.macroTypeName.str(),
                        macro->getName()
      );
      macro->diagnose(diag::decl_declared_here, macro->getName());
      return nullptr;
    }

#if SWIFT_SWIFT_PARSER
    PrettyStackTraceDecl debugStack("expanding attached macro", attachedTo);

    auto *astGenAttrSourceFile = attrSourceFile->getExportedSourceFile();
    if (!astGenAttrSourceFile)
      return nullptr;

    auto *astGenDeclSourceFile = declSourceFile->getExportedSourceFile();
    if (!astGenDeclSourceFile)
      return nullptr;

    void *astGenParentDeclSourceFile = nullptr;
    const void *parentDeclLoc = nullptr;
    if (passParentContext) {
      astGenParentDeclSourceFile =
          parentDeclSourceFile->getExportedSourceFile();
      if (!astGenParentDeclSourceFile)
        return nullptr;

      parentDeclLoc = parentDecl->getStartLoc().getOpaquePointerValue();
    }

    Decl *searchDecl = attachedTo;
    if (auto var = dyn_cast<VarDecl>(attachedTo))
      searchDecl = var->getParentPatternBinding();

    const char *evaluatedSourceAddress;
    ptrdiff_t evaluatedSourceLength;
    swift_ASTGen_expandAttachedMacro(
        &ctx.Diags, externalDef->opaqueHandle,
        static_cast<uint32_t>(externalDef->kind), discriminator->data(),
        discriminator->size(), static_cast<uint32_t>(role),
        astGenAttrSourceFile, attr->AtLoc.getOpaquePointerValue(),
        astGenDeclSourceFile, searchDecl->getStartLoc().getOpaquePointerValue(),
        astGenParentDeclSourceFile, parentDeclLoc, &evaluatedSourceAddress,
        &evaluatedSourceLength);
    if (!evaluatedSourceAddress)
      return nullptr;
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        {evaluatedSourceAddress, (size_t)evaluatedSourceLength},
        adjustMacroExpansionBufferName(*discriminator));
    free((void *)evaluatedSourceAddress);
    break;
#else
    attachedTo->diagnose(diag::macro_unsupported);
    return nullptr;
#endif
  }
  }

  // Dump macro expansions to standard output, if requested.
  if (ctx.LangOpts.DumpMacroExpansions) {
    llvm::errs() << evaluatedSource->getBufferIdentifier()
                 << "\n------------------------------\n"
                 << evaluatedSource->getBuffer()
                 << "\n------------------------------\n";
  }

  CharSourceRange generatedOriginalSourceRange;
  GeneratedSourceInfo::Kind generatedSourceKind;
  switch (role) {
  case MacroRole::Accessor: {
    generatedSourceKind = GeneratedSourceInfo::AccessorMacroExpansion;

    // Compute the location where the accessors will be added.
    auto storage = cast<AbstractStorageDecl>(attachedTo);
    auto bracesRange = storage->getBracesRange();
    if (bracesRange.Start.isValid()) {
      // We have braces already, so insert them inside the leading '{'.
      generatedOriginalSourceRange = CharSourceRange(
         Lexer::getLocForEndOfToken(sourceMgr, bracesRange.Start), 0);
    } else if (auto initRange = getExplicitInitializerRange(storage)) {
      // The accessor had an initializer, so the initializer (including
      // the `=`) is replaced by the accessors.
      generatedOriginalSourceRange =
          Lexer::getCharSourceRangeFromSourceRange(sourceMgr, *initRange);
    } else {
      // The accessors go at the end.
      SourceLoc endLoc = storage->getEndLoc();
      if (auto var = dyn_cast<VarDecl>(storage)) {
        if (auto pattern = var->getParentPattern())
          endLoc = pattern->getEndLoc();
      }

      generatedOriginalSourceRange = CharSourceRange(
         Lexer::getLocForEndOfToken(sourceMgr, endLoc), 0);
    }

    break;
  }

  case MacroRole::MemberAttribute: {
    generatedSourceKind = GeneratedSourceInfo::MemberAttributeMacroExpansion;
    SourceLoc startLoc;
    if (auto valueDecl = dyn_cast<ValueDecl>(attachedTo))
      startLoc = valueDecl->getAttributeInsertionLoc(/*forModifier=*/false);
    else
      startLoc = attachedTo->getStartLoc();

    generatedOriginalSourceRange = CharSourceRange(startLoc, 0);
    break;
  }

  case MacroRole::Member: {
    generatedSourceKind = GeneratedSourceInfo::MemberMacroExpansion;

    // Semantically, we insert members right before the closing brace.
    SourceLoc rightBraceLoc;
    if (auto nominal = dyn_cast<NominalTypeDecl>(attachedTo)) {
      rightBraceLoc = nominal->getBraces().End;
    } else {
      auto ext = cast<ExtensionDecl>(attachedTo);
      rightBraceLoc = ext->getBraces().End;
    }

    generatedOriginalSourceRange = CharSourceRange(rightBraceLoc, 0);
    break;
  }

  case MacroRole::Peer: {
    generatedSourceKind = GeneratedSourceInfo::PeerMacroExpansion;
    SourceLoc afterDeclLoc =
        Lexer::getLocForEndOfToken(sourceMgr, attachedTo->getEndLoc());
    generatedOriginalSourceRange = CharSourceRange(afterDeclLoc, 0);
    break;
  }

  case MacroRole::Conformance: {
    generatedSourceKind = GeneratedSourceInfo::ConformanceMacroExpansion;
    SourceLoc afterDeclLoc =
        Lexer::getLocForEndOfToken(sourceMgr, attachedTo->getEndLoc());
    generatedOriginalSourceRange = CharSourceRange(afterDeclLoc, 0);
    break;
  }

  case MacroRole::Expression:
  case MacroRole::Declaration:
  case MacroRole::CodeItem:
    llvm_unreachable("freestanding macro in attached macro evaluation");
  }

  // Create a new source buffer with the contents of the expanded macro.
  unsigned macroBufferID =
      sourceMgr.addNewSourceBuffer(std::move(evaluatedSource));
  auto macroBufferRange = sourceMgr.getRangeForBuffer(macroBufferID);
  GeneratedSourceInfo sourceInfo{
      generatedSourceKind,
      generatedOriginalSourceRange,
      macroBufferRange,
      ASTNode(attachedTo).getOpaqueValue(),
      dc,
      attr
  };
  sourceMgr.setGeneratedSourceInfo(macroBufferID, sourceInfo);

  // Create a source file to hold the macro buffer. This is automatically
  // registered with the enclosing module.
  auto macroSourceFile = new (ctx) SourceFile(
      *dc->getParentModule(), SourceFileKind::MacroExpansion, macroBufferID,
      /*parsingOpts=*/{}, /*isPrimary=*/false);
  macroSourceFile->setImports(declSourceFile->getImports());

  validateMacroExpansion(macroSourceFile, macro,
                         dyn_cast<ValueDecl>(attachedTo), role);
  return macroSourceFile;
}

Optional<unsigned> swift::expandAccessors(
    AbstractStorageDecl *storage, CustomAttr *attr, MacroDecl *macro
) {
  // Evaluate the macro.
  auto macroSourceFile = evaluateAttachedMacro(macro, storage, attr,
                                               /*passParentContext*/false,
                                               MacroRole::Accessor);
  if (!macroSourceFile)
    return None;

  PrettyStackTraceDecl debugStack(
      "type checking expanded declaration macro", storage);

  // Trigger parsing of the sequence of accessor declarations. This has the
  // side effect of registering those accessor declarations with the storage
  // declaration, so there is nothing further to do.
  for (auto decl : macroSourceFile->getTopLevelItems()) {
    auto accessor = dyn_cast_or_null<AccessorDecl>(decl.dyn_cast<Decl *>());
    if (!accessor)
      continue;

    if (accessor->isObservingAccessor())
      continue;

    // If any non-observing accessor was added, mark the initializer as
    // subsumed.
    if (auto var = dyn_cast<VarDecl>(storage)) {
      if (auto binding = var->getParentPatternBinding()) {
        unsigned index = binding->getPatternEntryIndexForVarDecl(var);
        binding->setInitializerSubsumed(index);
        break;
      }
    }
  }

  return macroSourceFile->getBufferID();
}

ArrayRef<unsigned> ExpandAccessorMacros::evaluate(
    Evaluator &evaluator, AbstractStorageDecl *storage
) const {
  llvm::SmallVector<unsigned, 1> bufferIDs;
  storage->forEachAttachedMacro(MacroRole::Accessor,
      [&](CustomAttr *customAttr, MacroDecl *macro) {
        if (auto bufferID = expandAccessors(
                storage, customAttr, macro))
          bufferIDs.push_back(*bufferID);
      });

  return storage->getASTContext().AllocateCopy(bufferIDs);
}

Optional<unsigned>
swift::expandAttributes(CustomAttr *attr, MacroDecl *macro, Decl *member) {
  // Evaluate the macro.
  auto macroSourceFile = evaluateAttachedMacro(macro, member, attr,
                                               /*passParentContext*/true,
                                               MacroRole::MemberAttribute);
  if (!macroSourceFile)
    return None;

  PrettyStackTraceDecl debugStack(
      "type checking expanded declaration macro", member);

  auto topLevelDecls = macroSourceFile->getTopLevelDecls();
  for (auto decl : topLevelDecls) {
    // Add the new attributes to the semantic attribute list.
    SmallVector<DeclAttribute *, 2> attrs(decl->getAttrs().begin(),
                                          decl->getAttrs().end());
    for (auto *attr : attrs) {
      member->getAttrs().add(attr);
    }
  }

  return macroSourceFile->getBufferID();
}

Optional<unsigned>
swift::expandMembers(CustomAttr *attr, MacroDecl *macro, Decl *decl) {
  // Evaluate the macro.
  auto macroSourceFile = evaluateAttachedMacro(macro, decl, attr,
                                               /*passParentContext*/false,
                                               MacroRole::Member);
  if (!macroSourceFile)
    return None;

  PrettyStackTraceDecl debugStack(
      "type checking expanded declaration macro", decl);

  auto topLevelDecls = macroSourceFile->getTopLevelDecls();
  for (auto member : topLevelDecls) {
    // Note that synthesized members are not considered implicit. They have
    // proper source ranges that should be validated, and ASTScope does not
    // expand implicit scopes to the parent scope tree.

    if (auto *nominal = dyn_cast<NominalTypeDecl>(decl)) {
      nominal->addMember(member);
    } else if (auto *extension = dyn_cast<ExtensionDecl>(decl)) {
      extension->addMember(member);
    }
  }

  return macroSourceFile->getBufferID();
}

Optional<unsigned>
swift::expandPeers(CustomAttr *attr, MacroDecl *macro, Decl *decl) {
  auto macroSourceFile = evaluateAttachedMacro(macro, decl, attr,
                                               /*passParentContext*/false,
                                               MacroRole::Peer);
  if (!macroSourceFile)
    return None;

  PrettyStackTraceDecl debugStack("applying expanded peer macro", decl);
  return macroSourceFile->getBufferID();
}

ArrayRef<unsigned>
ExpandConformanceMacros::evaluate(Evaluator &evaluator,
                                  NominalTypeDecl *nominal) const {
  SmallVector<unsigned, 2> bufferIDs;
  nominal->forEachAttachedMacro(MacroRole::Conformance,
      [&](CustomAttr *attr, MacroDecl *macro) {
        if (auto bufferID = expandConformances(attr, macro, nominal))
          bufferIDs.push_back(*bufferID);
      });

  return nominal->getASTContext().AllocateCopy(bufferIDs);
}

Optional<unsigned>
swift::expandConformances(CustomAttr *attr, MacroDecl *macro,
                          NominalTypeDecl *nominal) {
  auto macroSourceFile =
      evaluateAttachedMacro(macro, nominal, attr,
                            /*passParentContext*/false,
                            MacroRole::Conformance);

  if (!macroSourceFile)
    return None;

  PrettyStackTraceDecl debugStack(
      "applying expanded conformance macro", nominal);

  auto topLevelDecls = macroSourceFile->getTopLevelDecls();
  for (auto *decl : topLevelDecls) {
    auto *extension = dyn_cast<ExtensionDecl>(decl);
    if (!extension)
      continue;

    // Bind the extension to the original nominal type.
    extension->setExtendedNominal(nominal);
    nominal->addExtension(extension);

    // Make it accessible to getTopLevelDecls()
    if (auto file = dyn_cast<FileUnit>(
            decl->getDeclContext()->getModuleScopeContext()))
      file->getOrCreateSynthesizedFile().addTopLevelDecl(extension);
  }

  return macroSourceFile->getBufferID();
}

ConcreteDeclRef ResolveMacroRequest::evaluate(Evaluator &evaluator,
                                              UnresolvedMacroReference macroRef,
                                              DeclContext *dc) const {
  // Macro expressions and declarations have their own stored macro
  // reference. Use it if it's there.
  if (auto *expr = macroRef.getExpr()) {
    if (auto ref = expr->getMacroRef())
      return ref;
  } else if (auto decl = macroRef.getDecl()) {
    if (auto ref = decl->getMacroRef())
      return ref;
  }

  auto &ctx = dc->getASTContext();
  auto roles = macroRef.getMacroRoles();

  // When a macro is not found for a custom attribute, it may be a non-macro.
  // So bail out to prevent diagnostics from the contraint system.
  if (macroRef.getAttr()) {
    auto foundMacros = TypeChecker::lookupMacros(
        dc, macroRef.getMacroName(), SourceLoc(), roles);
    if (foundMacros.empty())
      return ConcreteDeclRef();
  }

  // If we already have a MacroExpansionExpr, use that. Otherwise,
  // create one.
  MacroExpansionExpr *macroExpansion;
  if (auto *expr = macroRef.getExpr()) {
    macroExpansion = expr;
  } else if (auto *decl = macroRef.getDecl()) {
    macroExpansion = new (ctx) MacroExpansionExpr(
        dc, decl->getExpansionInfo(), roles);
  } else {
    SourceRange genericArgsRange = macroRef.getGenericArgsRange();
    macroExpansion = new (ctx) MacroExpansionExpr(
      dc, macroRef.getSigilLoc(), macroRef.getMacroName(),
      macroRef.getMacroNameLoc(), genericArgsRange.Start,
      macroRef.getGenericArgs(), genericArgsRange.End,
      macroRef.getArgs(), roles);
  }

  Expr *result = macroExpansion;
  TypeChecker::typeCheckExpression(
      result, dc, {}, TypeCheckExprFlags::DisableMacroExpansions);

  // If we couldn't resolve a macro decl, the attribute is invalid.
  if (!macroExpansion->getMacroRef() && macroRef.getAttr())
    macroRef.getAttr()->setInvalid();

  // Macro expressions and declarations have their own stored macro
  // reference. If we got a reference, store it there, too.
  // FIXME: This duplication of state is really unfortunate.
  if (auto ref = macroExpansion->getMacroRef()) {
    if (auto *expr = macroRef.getExpr()) {
      expr->setMacroRef(ref);
    } else if (auto decl = macroRef.getDecl()) {
      decl->setMacroRef(ref);
    }
  }

  return macroExpansion->getMacroRef();
}

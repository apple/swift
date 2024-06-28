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
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTBridging.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Expr.h"
#include "swift/AST/FreestandingMacroExpansion.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/BasicBridging.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Bridging/ASTGen.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Subsystems.h"
#include "llvm/Config/config.h"

using namespace swift;

/// Translate an argument provided as a string literal into an identifier,
/// or return \c None and emit an error if it cannot be done.
std::optional<Identifier> getIdentifierFromStringLiteralArgument(
    ASTContext &ctx, MacroExpansionExpr *expansion, unsigned index) {
  auto argList = expansion->getArgs();

  // If there's no argument here, an error was diagnosed elsewhere.
  if (!argList || index >= argList->size()) {
    return std::nullopt;
  }

  auto arg = argList->getExpr(index);
  auto stringLiteral = dyn_cast<StringLiteralExpr>(arg);
  if (!stringLiteral) {
    ctx.Diags.diagnose(
        arg->getLoc(), diag::external_macro_arg_not_type_name, index
    );

    return std::nullopt;
  }


  auto contents = stringLiteral->getValue();
  if (!Lexer::isIdentifier(contents)) {
    ctx.Diags.diagnose(
        arg->getLoc(), diag::external_macro_arg_not_type_name, index
    );

    return std::nullopt;
  }

  return ctx.getIdentifier(contents);
}

/// For a macro expansion expression that is known to be #externalMacro,
/// handle the definition.
#if SWIFT_BUILD_SWIFT_SYNTAX
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
#endif // SWIFT_BUILD_SWIFT_SYNTAX

MacroDefinition MacroDefinitionRequest::evaluate(
    Evaluator &evaluator, MacroDecl *macro
) const {
  // If no definition was provided, the macro is... undefined, of course.
  auto definition = macro->definition;
  if (!definition)
    return MacroDefinition::forUndefined();

#if SWIFT_BUILD_SWIFT_SYNTAX
  ASTContext &ctx = macro->getASTContext();
  auto sourceFile = macro->getParentSourceFile();

  BridgedStringRef externalMacroName{nullptr, 0};
  ptrdiff_t *replacements = nullptr;
  ptrdiff_t numReplacements = 0;
  ptrdiff_t *genericReplacements = nullptr;
  ptrdiff_t numGenericReplacements = 0;

  // Parse 'macro' decl in swift_ASTGen_checkMacroDefinition.
  // NOTE: We don't use source->getExportedSourceFile() because it parses the
  // entire source buffer, which we don't want. Usually 'macro' decl is not in
  // the same file as the expansion, so we only want to parse only the decl.
  // FIXME: When we migrate to SwiftParser, use the parsed syntax tree.
  auto &SM = ctx.SourceMgr;
  StringRef sourceFileText =
      SM.getEntireTextForBuffer(*sourceFile->getBufferID());
  StringRef macroDeclText =
      SM.extractText(Lexer::getCharSourceRangeFromSourceRange(
          SM, macro->getSourceRangeIncludingAttrs()));

  auto checkResult = swift_ASTGen_checkMacroDefinition(
      &ctx.Diags, sourceFileText, macroDeclText, &externalMacroName,
      &replacements, &numReplacements, &genericReplacements,
      &numGenericReplacements);

  // Clean up after the call.
  SWIFT_DEFER {
    swift_ASTGen_freeBridgedString(externalMacroName);
    swift_ASTGen_freeExpansionReplacements(replacements, numReplacements);
    // swift_ASTGen_freeExpansionGenericReplacements(genericReplacements, numGenericReplacements); // FIXME: !!!!!!
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
    assert(!genericReplacements && "External macro doesn't have genericReplacements");
    StringRef externalMacroStr = externalMacroName.unbridged();
    StringRef externalModuleName, externalTypeName;
    std::tie(externalModuleName, externalTypeName) = externalMacroStr.split('.');

    Identifier moduleName = ctx.getIdentifier(externalModuleName);
    Identifier typeName = ctx.getIdentifier(externalTypeName);
    return MacroDefinition::forExternal(moduleName, typeName);
  }

  case BridgedBuiltinExternalMacro:
    return MacroDefinition::forBuiltin(BuiltinMacroKind::ExternalMacro);

  case BridgedBuiltinIsolationMacro:
    return MacroDefinition::forBuiltin(BuiltinMacroKind::IsolationMacro);
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
  StringRef expansionText = externalMacroName.unbridged();

  // Copy over the replacements.
  SmallVector<ExpandedMacroReplacement, 2> replacementsVec;
  for (unsigned i: range(0, numReplacements)) {
    replacementsVec.push_back(
        { static_cast<unsigned>(replacements[3*i]),
          static_cast<unsigned>(replacements[3*i+1]),
          static_cast<unsigned>(replacements[3*i+2])});
  }
  // Copy over the genericReplacements.
  SmallVector<ExpandedMacroReplacement, 2> genericReplacementsVec;
  for (unsigned i: range(0, numGenericReplacements)) {
    genericReplacementsVec.push_back(
        { static_cast<unsigned>(genericReplacements[3*i]),
          static_cast<unsigned>(genericReplacements[3*i+1]),
          static_cast<unsigned>(genericReplacements[3*i+2])});
  }

  return MacroDefinition::forExpanded(ctx, expansionText, replacementsVec, genericReplacementsVec);
#else
  macro->diagnose(diag::macro_unsupported);
  return MacroDefinition::forInvalid();
#endif
}

static llvm::Expected<CompilerPlugin *>
initializePlugin(ASTContext &ctx, CompilerPlugin *plugin, StringRef libraryPath,
                 Identifier moduleName) {
  // Lock the plugin while initializing.
  // Note that 'executablePlugin' can be shared between multiple ASTContext.
  plugin->lock();
  SWIFT_DEFER { plugin->unlock(); };

  // FIXME: Ideally this should be done right after invoking the plugin.
  // But plugin loading is in libAST and it can't link ASTGen symbols.
  if (!plugin->isInitialized()) {
#if SWIFT_BUILD_SWIFT_SYNTAX
    if (!swift_ASTGen_initializePlugin(plugin, &ctx.Diags)) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "'%s' produced malformed response",
                                     plugin->getPath().data());
    }

    // Resend the compiler capability on reconnect.
    auto *callback = new std::function<void(void)>([plugin]() {
      (void)swift_ASTGen_initializePlugin(plugin, /*diags=*/nullptr);
    });
    plugin->addOnReconnect(callback);

    plugin->setCleanup([plugin] { swift_ASTGen_deinitializePlugin(plugin); });
#endif
  }

  // If this is a plugin server, load the library.
  if (!libraryPath.empty()) {
#if SWIFT_BUILD_SWIFT_SYNTAX
    llvm::SmallString<128> resolvedLibraryPath;
    auto fs = ctx.ClangImporterOpts.HasClangIncludeTreeRoot
                  ? llvm::vfs::getRealFileSystem()
                  : ctx.SourceMgr.getFileSystem();
    if (auto err = fs->getRealPath(libraryPath, resolvedLibraryPath)) {
      return llvm::createStringError(err, err.message());
    }

    std::string resolvedLibraryPathStr(resolvedLibraryPath);
    std::string moduleNameStr(moduleName.str());

    BridgedStringRef bridgedErrorOut{nullptr, 0};
    bool loaded = swift_ASTGen_pluginServerLoadLibraryPlugin(
        plugin, resolvedLibraryPathStr.c_str(), moduleNameStr.c_str(),
        &bridgedErrorOut);

    auto errorOut = bridgedErrorOut.unbridged();
    if (!loaded) {
      SWIFT_DEFER { swift_ASTGen_freeBridgedString(errorOut); };
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "failed to load library plugin '%s' in plugin server '%s'; %s",
          resolvedLibraryPathStr.c_str(), plugin->getPath().data(),
          errorOut.data());
    }

    assert(errorOut.data() == nullptr);

    // Set a callback to load the library again on reconnections.
    auto *callback = new std::function<void(void)>(
        [plugin, resolvedLibraryPathStr, moduleNameStr]() {
          (void)swift_ASTGen_pluginServerLoadLibraryPlugin(
              plugin, resolvedLibraryPathStr.c_str(), moduleNameStr.c_str(),
              /*errorOut=*/nullptr);
        });
    plugin->addOnReconnect(callback);

    // Remove the callback and deallocate it when this ASTContext is destructed.
    ctx.addCleanup([plugin, callback]() {
      plugin->removeOnReconnect(callback);
      delete callback;
    });
#endif
  }

  return plugin;
}

CompilerPluginLoadResult
CompilerPluginLoadRequest::evaluate(Evaluator &evaluator, ASTContext *ctx,
                                    Identifier moduleName) const {
  PluginLoader &loader = ctx->getPluginLoader();
  const auto &entry = loader.lookupPluginByModuleName(moduleName);

  SmallString<0> errorMessage;

  if (!entry.executablePath.empty()) {
    llvm::Expected<CompilerPlugin *> plugin =
        loader.loadExecutablePlugin(entry.executablePath);
    if (plugin) {
      if (ctx->LangOpts.EnableMacroLoadingRemarks) {
        unsigned tag = entry.libraryPath.empty() ? 1 : 2;
        ctx->Diags.diagnose(SourceLoc(), diag::macro_loaded, moduleName, tag,
                            entry.executablePath, entry.libraryPath);
      }

      plugin =
          initializePlugin(*ctx, plugin.get(), entry.libraryPath, moduleName);
    }
    if (plugin)
      return plugin.get();
    llvm::handleAllErrors(plugin.takeError(),
                          [&](const llvm::ErrorInfoBase &err) {
                            if (!errorMessage.empty())
                              errorMessage += ", ";
                            errorMessage += err.message();
                          });
  } else if (!entry.libraryPath.empty()) {

    llvm::Expected<CompilerPlugin *> plugins = loader.getInProcessPlugins();
    if (plugins) {
      plugins =
          initializePlugin(*ctx, plugins.get(), entry.libraryPath, moduleName);
      if (plugins) {
        if (ctx->LangOpts.EnableMacroLoadingRemarks) {
          ctx->Diags.diagnose(SourceLoc(), diag::macro_loaded, moduleName, 0,
                              entry.libraryPath, StringRef());
        }
        return plugins.get();
      }
    }

    if (!plugins) {
      llvm::handleAllErrors(plugins.takeError(),
                            [&](const llvm::ErrorInfoBase &err) {
                              if (!errorMessage.empty())
                                errorMessage += ", ";
                              errorMessage += err.message();
                            });
    }
  }
  if (!errorMessage.empty()) {
    NullTerminatedStringRef err(errorMessage, *ctx);
    return CompilerPluginLoadResult::error(err);
  } else {
    NullTerminatedStringRef errMsg(
        "plugin for module '" + moduleName.str() + "' not found", *ctx);
    return CompilerPluginLoadResult::error(errMsg);
  }
}

static ExternalMacroDefinition resolveExternalMacro(ASTContext &ctx,
                                                    CompilerPlugin *plugin,
                                                    Identifier moduleName,
                                                    Identifier typeName) {
#if SWIFT_BUILD_SWIFT_SYNTAX
  if (auto *macro = swift_ASTGen_resolveExternalMacro(moduleName.get(),
                                                      typeName.get(), plugin)) {
    // Make sure we clean up after the macro.
    ctx.addCleanup([macro]() { swift_ASTGen_destroyExternalMacro(macro); });
    return ExternalMacroDefinition::success(macro);
  }
  // NOTE: this is not reachable because executable macro resolution always
  // succeeds.
  NullTerminatedStringRef err("'" + moduleName.str() + "." + typeName.str() +
                                  "' could not be found in executable plugin" +
                                  StringRef(plugin->getPath()),
                              ctx);
  return ExternalMacroDefinition::error(err);
#endif
  return ExternalMacroDefinition::error(
      "the current compiler was not built with macro support");
}

ExternalMacroDefinition
ExternalMacroDefinitionRequest::evaluate(Evaluator &evaluator, ASTContext *ctx,
                                         Identifier moduleName,
                                         Identifier typeName) const {
  // Try to load a plugin module from the plugin search paths. If it
  // succeeds, resolve in-process from that plugin
  CompilerPluginLoadRequest loadRequest{ctx, moduleName};
  CompilerPluginLoadResult loaded = evaluateOrDefault(
      evaluator, loadRequest, CompilerPluginLoadResult::error("request error"));

  if (auto plugin = loaded.get()) {
    return resolveExternalMacro(*ctx, plugin, moduleName, typeName);
  }

  return ExternalMacroDefinition::error(loaded.getErrorMessage());
}

/// Adjust the given mangled name for a macro expansion to produce a valid
/// buffer name.
static std::string adjustMacroExpansionBufferName(StringRef name) {
  if (name.empty()) {
    return "<macro-expansion>";
  }
  std::string result;
  if (name.starts_with(MANGLING_PREFIX_STR)) {
    result += MACRO_EXPANSION_BUFFER_MANGLING_PREFIX;
    name = name.drop_front(StringRef(MANGLING_PREFIX_STR).size());
  }

  result += name;
  result += ".swift";
  return result;
}

std::optional<unsigned>
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
      (void)mee->createSubstituteDecl();
    }
    // Return the expanded buffer ID.
    return evaluateOrDefault(
        evaluator, ExpandMacroExpansionDeclRequest(mee->getSubstituteDecl()),
        std::nullopt);
  }

  // Other macro roles may also be encountered here, as they use
  // `MacroExpansionExpr` for resolution. In those cases, do not expand.
  return std::nullopt;
}

ArrayRef<unsigned> ExpandMemberAttributeMacros::evaluate(Evaluator &evaluator,
                                                         Decl *decl) const {
  if (decl->isImplicit())
    return { };

  // Member attribute macros do not apply to accessors.
  if (isa<AccessorDecl>(decl))
    return { };

  // Member attribute macros do not apply to macro-expanded members.
  if (decl->isInMacroExpansionInContext())
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

bool swift::isInvalidAttachedMacro(MacroRole role,
                                   Decl *attachedTo) {
  switch (role) {
#define FREESTANDING_MACRO_ROLE(Name, Description) case MacroRole::Name:
#define ATTACHED_MACRO_ROLE(Name, Description, MangledChar)
#include "swift/Basic/MacroRoles.def"
    llvm_unreachable("Invalid macro role for attached macro");

  case MacroRole::Accessor:
    // Only var decls and subscripts have accessors.
    if (isa<AbstractStorageDecl>(attachedTo) && !isa<ParamDecl>(attachedTo))
      return false;

    break;

  case MacroRole::MemberAttribute:
  case MacroRole::Member:
    // Nominal types and extensions can have members.
    if (isa<NominalTypeDecl>(attachedTo) || isa<ExtensionDecl>(attachedTo))
      return false;

    break;

  case MacroRole::Peer:
    // Peer macros are allowed on everything except parameters.
    if (!isa<ParamDecl>(attachedTo))
      return false;

    break;

  case MacroRole::Conformance:
  case MacroRole::Extension:
    // Only primary declarations of nominal types
    if (isa<NominalTypeDecl>(attachedTo))
      return false;

    break;

  case MacroRole::Preamble:
  case MacroRole::Body:
    // Only function declarations.
    if (isa<AbstractFunctionDecl>(attachedTo))
      return false;

    break;
  }

  return true;
}

static void diagnoseInvalidDecl(Decl *decl,
                                MacroDecl *macro,
                                llvm::function_ref<bool(DeclName)> coversName) {
  auto &ctx = decl->getASTContext();

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

    return;
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
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(_, __, typeName,   \
                                                supportsOverride)    \
    if (supportsOverride && name == makeIdentifier(ctx, typeName)) { \
      typeAlias->diagnose(diag::literal_type_in_macro_expansion,     \
                          makeIdentifier(ctx, typeName));            \
      typeAlias->setInvalid();                                       \
      return;                                                        \
    }
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME
  }

  // Diagnose value decls with names not covered by the macro
  if (auto *value = dyn_cast<ValueDecl>(decl)) {
    auto name = value->getName();

    // Unique names are always permitted.
    if (MacroDecl::isUniqueMacroName(name.getBaseName().userFacingName()))
      return;

    if (coversName(name)) {
      return;
    }

    value->diagnose(diag::invalid_macro_introduced_name,
                    name, macro->getBaseName());
  }
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

  llvm::SmallDenseSet<DeclName, 2> introducedNameSet(
      introducedNames.begin(), introducedNames.end());

  auto coversName = [&](DeclName name) -> bool {
    return (introducedNameSet.count(name) ||
            introducedNameSet.count(name.getBaseName()) ||
            introducedNameSet.count(MacroDecl::getArbitraryName()));
  };

  for (auto item : expansionBuffer->getTopLevelItems()) {
    auto &ctx = expansionBuffer->getASTContext();
    auto *decl = item.dyn_cast<Decl *>();
    if (!decl) {
      if (role != MacroRole::CodeItem &&
          role != MacroRole::Preamble &&
          role != MacroRole::Body) {
        ctx.Diags.diagnose(item.getStartLoc(),
                           diag::expected_macro_expansion_decls);
      }

      continue;
    }

    // Certain macro roles can generate special declarations.
    if (isa<AccessorDecl>(decl) && role == MacroRole::Accessor) {
      auto *var = dyn_cast<VarDecl>(attachedTo);
      if (var && var->isLet()) {
        ctx.Diags.diagnose(var->getLoc(),
                           diag::let_accessor_expansion)
          .warnUntilSwiftVersion(6);
      }

      continue;
    }

    if (isa<ExtensionDecl>(decl) && role == MacroRole::Conformance) {
      continue;
    }

    if (role == MacroRole::Extension) {
      auto *extension = dyn_cast<ExtensionDecl>(decl);

      for (auto *member : extension->getMembers()) {
        diagnoseInvalidDecl(member, macro, coversName);
      }

      continue;
    }

    diagnoseInvalidDecl(decl, macro, coversName);
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
    ExpandedMacroDefinition def, MacroDecl *macro,
    SubstitutionMap subs,
    ArgumentList *args) {
  ASTContext &ctx = macro->getASTContext();

  std::string expandedResult;

  StringRef originalText = def.getExpansionText();

  unsigned startIdx = 0;
  unsigned replacementsIdx = 0;
  unsigned genericReplacementsIdx = 0;
  auto totalReplacementsCount =
      def.getReplacements().size() + def.getGenericReplacements().size();

  while (replacementsIdx + genericReplacementsIdx < totalReplacementsCount) {
    ExpandedMacroReplacement replacement;
    bool isExpressionReplacement = true;

    // Pick the "next" replacement, in order as they appear in the source text
    auto canPickExpressionReplacement = replacementsIdx < def.getReplacements().size();
    auto canPickGenericReplacement = genericReplacementsIdx < def.getGenericReplacements().size();
    if (canPickExpressionReplacement && canPickGenericReplacement) {
      auto expressionReplacement = def.getReplacements()[replacementsIdx];
      auto genericReplacement =
          def.getGenericReplacements()[genericReplacementsIdx];
      isExpressionReplacement =
          expressionReplacement.startOffset < genericReplacement.startOffset;
      replacement =
          isExpressionReplacement ? expressionReplacement : genericReplacement;
    } else if (canPickExpressionReplacement) {
      isExpressionReplacement = true;
      replacement = def.getReplacements()[replacementsIdx];
    } else if (canPickGenericReplacement) {
      isExpressionReplacement = false;
      replacement = def.getGenericReplacements()[replacementsIdx];
    } else {
      assert(false && "should always select a requirement explicitly rather "
                      "than fall through");
    }

    replacementsIdx += isExpressionReplacement ? 1 : 0;
    genericReplacementsIdx += isExpressionReplacement ? 0 : 1;

    // Add the original text up to the first replacement.
    expandedResult.append(originalText.begin() + startIdx,
                          originalText.begin() + replacement.startOffset);

    // Add the replacement text.
    if (isExpressionReplacement) {
      auto argExpr = args->getArgExprs()[replacement.parameterIndex];
      SmallString<32> argTextBuffer;
      auto argText =
          extractInlinableText(ctx.SourceMgr, argExpr, argTextBuffer);
      expandedResult.append(argText);
    } else {
      auto typeArgType = subs.getReplacementTypes()[replacement.parameterIndex];
      std::string typeNameString;
      llvm::raw_string_ostream os(typeNameString);
      typeArgType.print(os);
      expandedResult.append(typeNameString);
    }

    // Update the starting position.
    startIdx = replacement.endOffset;
  }

  // Add the remaining text.
  expandedResult.append(
      originalText.begin() + startIdx,
      originalText.end());

  return expandedResult;
}

static GeneratedSourceInfo::Kind getGeneratedSourceInfoKind(MacroRole role) {
  switch (role) {
#define MACRO_ROLE(Name, Description)                 \
  case MacroRole::Name:                               \
    return GeneratedSourceInfo::Name##MacroExpansion;
#include "swift/Basic/MacroRoles.def"
  }
}

// If this storage declaration is a variable with an explicit initializer,
// return the range from the `=` to the end of the explicit initializer.
static std::optional<SourceRange>
getExplicitInitializerRange(AbstractStorageDecl *storage) {
  auto var = dyn_cast<VarDecl>(storage);
  if (!var)
    return std::nullopt;

  auto pattern = var->getParentPatternBinding();
  if (!pattern)
    return std::nullopt;

  unsigned index = pattern->getPatternEntryIndexForVarDecl(var);
  SourceLoc equalLoc = pattern->getEqualLoc(index);
  SourceRange initRange = pattern->getOriginalInitRange(index);
  if (equalLoc.isInvalid() || initRange.End.isInvalid())
    return std::nullopt;

  return SourceRange(equalLoc, initRange.End);
}

/// Compute the original source range for expanding a preamble macro.
static CharSourceRange getPreambleMacroOriginalRange(AbstractFunctionDecl *fn) {
  ASTContext &ctx = fn->getASTContext();

  SourceLoc insertionLoc;

  // If there is a body macro, start at the beginning of it.
  if (auto bodyBufferID = evaluateOrDefault(
          ctx.evaluator, ExpandBodyMacroRequest{fn}, std::nullopt)) {
    insertionLoc = ctx.SourceMgr.getRangeForBuffer(*bodyBufferID).getStart();
  } else {
    // If there is a parsed body, start at the beginning of it.
    SourceRange range = fn->getBodySourceRange();
    if (range.isValid()) {
      insertionLoc = range.Start;
    } else {
      insertionLoc = fn->getEndLoc();
    }
  }

  return CharSourceRange(insertionLoc, 0);
}

static CharSourceRange getExpansionInsertionRange(MacroRole role,
                                                  ASTNode target,
                                                  SourceManager &sourceMgr) {
  switch (role) {
  case MacroRole::Accessor: {
    auto storage = cast<AbstractStorageDecl>(target.get<Decl *>());
    auto bracesRange = storage->getBracesRange();

    // Compute the location where the accessors will be added.
    if (bracesRange.Start.isValid()) {
      // We have braces already, so insert them inside the leading '{'.
      return CharSourceRange(
          Lexer::getLocForEndOfToken(sourceMgr, bracesRange.Start), 0);
    } else if (auto initRange = getExplicitInitializerRange(storage)) {
      // The accessor had an initializer, so the initializer (including
      // the `=`) is replaced by the accessors.
      return Lexer::getCharSourceRangeFromSourceRange(sourceMgr, *initRange);
    } else {
      // The accessors go at the end.
      SourceLoc endLoc = storage->getEndLoc();
      if (auto var = dyn_cast<VarDecl>(storage)) {
        if (auto pattern = var->getParentPattern())
          endLoc = pattern->getEndLoc();
      }

      return CharSourceRange(Lexer::getLocForEndOfToken(sourceMgr, endLoc), 0);
    }
  }
  case MacroRole::MemberAttribute: {
    SourceLoc startLoc;
    if (auto valueDecl = dyn_cast<ValueDecl>(target.get<Decl *>()))
      startLoc = valueDecl->getAttributeInsertionLoc(/*forModifier=*/true);
    else
      startLoc = target.getStartLoc();

    return CharSourceRange(startLoc, 0);
  }
  case MacroRole::Member: {
    // Semantically, we insert members right before the closing brace.
    SourceLoc rightBraceLoc;
    if (auto nominal = dyn_cast<NominalTypeDecl>(target.get<Decl *>())) {
      rightBraceLoc = nominal->getBraces().End;
    } else {
      auto ext = cast<ExtensionDecl>(target.get<Decl *>());
      rightBraceLoc = ext->getBraces().End;
    }

    return CharSourceRange(rightBraceLoc, 0);
  }
  case MacroRole::Peer: {
    SourceLoc endLoc = target.getEndLoc();
    if (auto var = dyn_cast<VarDecl>(target.get<Decl *>())) {
      if (auto binding = var->getParentPatternBinding())
        endLoc = binding->getEndLoc();
    }
    SourceLoc afterDeclLoc = Lexer::getLocForEndOfToken(sourceMgr, endLoc);
    return CharSourceRange(afterDeclLoc, 0);
    break;
  }

  case MacroRole::Conformance: {
    SourceLoc afterDeclLoc =
        Lexer::getLocForEndOfToken(sourceMgr, target.getEndLoc());
    return CharSourceRange(afterDeclLoc, 0);
  }

  case MacroRole::Extension: {
    SourceLoc afterDeclLoc =
        Lexer::getLocForEndOfToken(sourceMgr, target.getEndLoc());
    return CharSourceRange(afterDeclLoc, 0);
  }

  case MacroRole::Preamble: {
    if (auto fn = dyn_cast<AbstractFunctionDecl>(target.get<Decl *>())) {
      return getPreambleMacroOriginalRange(fn);
    }

    return CharSourceRange(Lexer::getLocForEndOfToken(sourceMgr, target.getEndLoc()), 0);
  }

  case MacroRole::Body: {
    SourceLoc afterDeclLoc =
        Lexer::getLocForEndOfToken(sourceMgr, target.getEndLoc());
    return CharSourceRange(afterDeclLoc, 0);
  }

  case MacroRole::Expression:
  case MacroRole::Declaration:
  case MacroRole::CodeItem:
    return Lexer::getCharSourceRangeFromSourceRange(sourceMgr,
                                                    target.getSourceRange());
  }
  llvm_unreachable("unhandled MacroRole");
}

static SourceFile *
createMacroSourceFile(std::unique_ptr<llvm::MemoryBuffer> buffer,
                      MacroRole role, ASTNode target, DeclContext *dc,
                      CustomAttr *attr) {
  ASTContext &ctx = dc->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;

  // Dump macro expansions to standard output, if requested.
  if (ctx.LangOpts.DumpMacroExpansions) {
    llvm::errs() << buffer->getBufferIdentifier()
                 << "\n------------------------------\n"
                 << buffer->getBuffer()
                 << "\n------------------------------\n";
  }

  CharSourceRange generatedOriginalSourceRange =
      getExpansionInsertionRange(role, target, sourceMgr);
  GeneratedSourceInfo::Kind generatedSourceKind =
      getGeneratedSourceInfoKind(role);

  // Create a new source buffer with the contents of the expanded macro.
  unsigned macroBufferID = sourceMgr.addNewSourceBuffer(std::move(buffer));
  auto macroBufferRange = sourceMgr.getRangeForBuffer(macroBufferID);
  GeneratedSourceInfo sourceInfo{generatedSourceKind,
                                 generatedOriginalSourceRange,
                                 macroBufferRange,
                                 target.getOpaqueValue(),
                                 dc,
                                 attr};
  sourceMgr.setGeneratedSourceInfo(macroBufferID, sourceInfo);

  // Create a source file to hold the macro buffer. This is automatically
  // registered with the enclosing module.
  auto macroSourceFile = new (ctx) SourceFile(
      *dc->getParentModule(), SourceFileKind::MacroExpansion, macroBufferID,
      /*parsingOpts=*/{}, /*isPrimary=*/false);
  macroSourceFile->setImports(dc->getParentSourceFile()->getImports());
  return macroSourceFile;
}

#if SWIFT_BUILD_SWIFT_SYNTAX
static uint8_t getRawMacroRole(MacroRole role) {
  switch (role) {
  case MacroRole::Expression: return 0;
  case MacroRole::Declaration: return 1;
  case MacroRole::Accessor: return 2;
  case MacroRole::MemberAttribute: return 3;
  case MacroRole::Member: return 4;
  case MacroRole::Peer: return 5;
  case MacroRole::CodeItem: return 7;
  // Use the same raw macro role for conformance and extension
  // in ASTGen.
  case MacroRole::Conformance:
  case MacroRole::Extension: return 8;
  case MacroRole::Preamble: return 9;
  case MacroRole::Body: return 10;
  }
}
#endif // SWIFT_BUILD_SWIFT_SYNTAX

/// Evaluate the given freestanding macro expansion.
static SourceFile *
evaluateFreestandingMacro(FreestandingMacroExpansion *expansion,
                          StringRef discriminatorStr = "") {
  auto *dc = expansion->getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  SourceLoc loc = expansion->getPoundLoc();

  auto moduleDecl = dc->getParentModule();
  auto sourceFile = moduleDecl->getSourceFileContainingLocation(loc);
  if (!sourceFile)
    return nullptr;

  MacroDecl *macro = cast<MacroDecl>(expansion->getMacroRef().getDecl());
  auto macroRoles = macro->getMacroRoles();
  assert(macroRoles.contains(MacroRole::Expression) ||
         macroRoles.contains(MacroRole::Declaration) ||
         macroRoles.contains(MacroRole::CodeItem));

  if (isFromExpansionOfMacro(sourceFile, macro, MacroRole::Expression) ||
      isFromExpansionOfMacro(sourceFile, macro, MacroRole::Declaration) ||
      isFromExpansionOfMacro(sourceFile, macro, MacroRole::CodeItem)) {
    ctx.Diags.diagnose(loc, diag::macro_recursive, macro->getName());
    return nullptr;
  }

  // Evaluate the macro.
  std::unique_ptr<llvm::MemoryBuffer> evaluatedSource;

  /// The discriminator used for the macro.
  LazyValue<std::string> discriminator([&]() -> std::string {
    if (!discriminatorStr.empty())
      return discriminatorStr.str();
#if SWIFT_BUILD_SWIFT_SYNTAX
    Mangle::ASTMangler mangler;
    return mangler.mangleMacroExpansion(expansion);
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
      ctx.Diags.diagnose(loc, diag::external_macro_outside_macro_definition);
      return nullptr;

    case BuiltinMacroKind::IsolationMacro:
      // Create a buffer full of scratch space; this will be populated
      // much later.
      std::string scratchSpace(128, ' ');
      evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
          scratchSpace,
          adjustMacroExpansionBufferName(*discriminator));
      break;
    }
    break;
  }

  case MacroDefinition::Kind::Expanded: {
    // Expand the definition with the given arguments.
    auto result = expandMacroDefinition(macroDef.getExpanded(), macro,
                                        expansion->getMacroRef().getSubstitutions(),
                                        expansion->getArgs());
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        result, adjustMacroExpansionBufferName(*discriminator));
    break;
  }

  case MacroDefinition::Kind::External: {
    // Retrieve the external definition of the macro.
    auto external = macroDef.getExternalMacro();
    ExternalMacroDefinitionRequest request{&ctx, external.moduleName,
                                           external.macroTypeName};
    auto externalDef =
        evaluateOrDefault(ctx.evaluator, request,
                          ExternalMacroDefinition::error("request error"));
    if (externalDef.isError()) {
      ctx.Diags.diagnose(loc, diag::external_macro_not_found,
                         external.moduleName.str(),
                         external.macroTypeName.str(), macro->getName(),
                         externalDef.getErrorMessage());
      macro->diagnose(diag::decl_declared_here, macro);
      return nullptr;
    }

    // Code item macros require `CodeItemMacros` feature flag.
    if (macroRoles.contains(MacroRole::CodeItem) &&
        !ctx.LangOpts.hasFeature(Feature::CodeItemMacros)) {
      ctx.Diags.diagnose(loc, diag::macro_experimental, "code item",
                         "CodeItemMacros");
      return nullptr;
    }

#if SWIFT_BUILD_SWIFT_SYNTAX
    // Only one freestanding macro role is permitted, so look at the roles to
    // figure out which one to use.
    MacroRole macroRole =
      macroRoles.contains(MacroRole::Expression) ? MacroRole::Expression
      : macroRoles.contains(MacroRole::Declaration) ? MacroRole::Declaration
      : MacroRole::CodeItem;

    PrettyStackTraceFreestandingMacroExpansion debugStack(
        "expanding freestanding macro", expansion);

    // Builtin macros are handled via ASTGen.
    auto *astGenSourceFile = sourceFile->getExportedSourceFile();
    if (!astGenSourceFile)
      return nullptr;

    BridgedStringRef evaluatedSourceOut{nullptr, 0};
    assert(!externalDef.isError());
    swift_ASTGen_expandFreestandingMacro(
        &ctx.Diags, externalDef.get(), discriminator->c_str(),
        getRawMacroRole(macroRole), astGenSourceFile,
        expansion->getSourceRange().Start.getOpaquePointerValue(),
        &evaluatedSourceOut);
    if (!evaluatedSourceOut.unbridged().data())
      return nullptr;
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        evaluatedSourceOut.unbridged(),
        adjustMacroExpansionBufferName(*discriminator));
    swift_ASTGen_freeBridgedString(evaluatedSourceOut);
    break;
#else
    ctx.Diags.diagnose(loc, diag::macro_unsupported);
    return nullptr;
#endif
  }
  }

  return createMacroSourceFile(std::move(evaluatedSource),
                               isa<MacroExpansionDecl>(expansion)
                                   ? MacroRole::Declaration
                                   : MacroRole::Expression,
                               expansion->getASTNode(), dc,
                               /*attr=*/nullptr);
}

std::optional<unsigned> swift::expandMacroExpr(MacroExpansionExpr *mee) {
  SourceFile *macroSourceFile = ::evaluateFreestandingMacro(mee);
  if (!macroSourceFile)
    return std::nullopt;

  DeclContext *dc = mee->getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;

  auto macroBufferID = *macroSourceFile->getBufferID();
  auto macroBufferRange = sourceMgr.getRangeForBuffer(macroBufferID);

  // Handle builtin macro definitions by producing the expression we
  // need without parsing the buffer.
  auto expandedType = mee->getType();
  Expr *expandedExpr = nullptr;
  auto macro = cast<MacroDecl>(mee->getMacroRef().getDecl());
  switch (auto definition = macro->getDefinition()) {
  case MacroDefinition::Kind::Expanded:
  case MacroDefinition::Kind::External:
  case MacroDefinition::Kind::Invalid:
  case MacroDefinition::Kind::Undefined:
    break;

  case MacroDefinition::Kind::Builtin:
    switch (definition.getBuiltinKind()) {
    case BuiltinMacroKind::ExternalMacro:
      break;

    case BuiltinMacroKind::IsolationMacro:
      expandedExpr = new (ctx) CurrentContextIsolationExpr(
          macroBufferRange.getStart(), expandedType);
      break;
    }
  }

  if (!expandedExpr) {
    // Parse the macro source file and retrieve the parsed expression
    // from the list of top-level items.
    auto topLevelItems = macroSourceFile->getTopLevelItems();
    if (topLevelItems.size() != 1) {
      ctx.Diags.diagnose(
          macroBufferRange.getStart(), diag::expected_macro_expansion_expr);
      return macroBufferID;
    }

    auto codeItem = topLevelItems.front();
    if (auto *expr = codeItem.dyn_cast<Expr *>())
      expandedExpr = expr;
  } else {
    // We produced the expanded expression via a builtin macro above,
    // so there is nothing more we need to do.
  }

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
std::optional<unsigned>
swift::expandFreestandingMacro(MacroExpansionDecl *med) {
  SourceFile *macroSourceFile = ::evaluateFreestandingMacro(med);
  if (!macroSourceFile)
    return std::nullopt;

  MacroDecl *macro = cast<MacroDecl>(med->getMacroRef().getDecl());
  auto macroRoles = macro->getMacroRoles();
  assert(macroRoles.contains(MacroRole::Declaration) ||
         macroRoles.contains(MacroRole::CodeItem));
  DeclContext *dc = med->getDeclContext();

  validateMacroExpansion(macroSourceFile, macro,
                         /*attachedTo*/nullptr,
                         macroRoles.contains(MacroRole::Declaration) ?
                             MacroRole::Declaration :
                             MacroRole::CodeItem);

  PrettyStackTraceDecl debugStack(
      "type checking expanded declaration macro", med);

  auto topLevelItems = macroSourceFile->getTopLevelItems();
  for (auto item : topLevelItems) {
    if (auto *decl = item.dyn_cast<Decl *>())
      decl->setDeclContext(dc);
  }
  return *macroSourceFile->getBufferID();
}

static SourceFile *evaluateAttachedMacro(MacroDecl *macro, Decl *attachedTo,
                                         CustomAttr *attr,
                                         bool passParentContext, MacroRole role,
                                         ArrayRef<ProtocolDecl *> conformances = {},
                                         StringRef discriminatorStr = "") {
  DeclContext *dc;
  if (role == MacroRole::Peer) {
    dc = attachedTo->getDeclContext();
  } else if (role == MacroRole::Conformance || role == MacroRole::Extension) {
    // Conformance macros always expand to extensions at file-scope.
    dc = attachedTo->getDeclContext()->getParentSourceFile();
  } else {
    dc = attachedTo->getInnermostDeclContext();
  }

  // FIXME: compatibility hack for the transition from property wrapper
  // to macro for TaskLocal.
  //
  // VarDecls with `@_projectedValueProperty` have already had the property
  // wrapper transform applied. This only impacts swiftinterfaces, and if
  // a swiftinterface was produced against a Concurrency library that does
  // not declare TaskLocal as a macro, we need to ignore the macro to avoid
  // producing duplicate declarations. This is only needed temporarily until
  // all swiftinterfaces have been built against the Concurrency library
  // containing the new macro declaration.
  if (auto *var = dyn_cast<VarDecl>(attachedTo)) {
    if (var->getAttrs().getAttribute<ProjectedValuePropertyAttr>()) {
      return nullptr;
    }
  }

  ASTContext &ctx = dc->getASTContext();

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
    if (!discriminatorStr.empty())
      return discriminatorStr.str();
#if SWIFT_BUILD_SWIFT_SYNTAX
    Mangle::ASTMangler mangler;
    return mangler.mangleAttachedMacroExpansion(attachedTo, attr, role);
#else
    return "";
#endif
  });

  std::string extendedType;
  {
    llvm::raw_string_ostream OS(extendedType);
    if (role == MacroRole::Extension || role == MacroRole::Conformance) {
      auto *nominal = dyn_cast<NominalTypeDecl>(attachedTo);
      PrintOptions options;
      options.FullyQualifiedExtendedTypesIfAmbiguous = true;
      nominal->getDeclaredType()->print(OS, options);
    } else {
      OS << "";
    }
  }

  std::string conformanceList;
  {
    llvm::raw_string_ostream OS(conformanceList);
    if (role == MacroRole::Extension || role == MacroRole::Member) {
      llvm::interleave(
          conformances,
          [&](const ProtocolDecl *protocol) {
            protocol->getDeclaredType()->print(OS);
          },
          [&] { OS << ", "; }
      );
    } else {
      OS << "";
    }
  }

  auto macroDef = macro->getDefinition();
  switch (macroDef.kind) {
  case MacroDefinition::Kind::Undefined:
  case MacroDefinition::Kind::Invalid:
    // Already diagnosed as an error elsewhere.
    return nullptr;

  case MacroDefinition::Kind::Builtin: {
    switch (macroDef.getBuiltinKind()) {
    case BuiltinMacroKind::ExternalMacro:
    case BuiltinMacroKind::IsolationMacro:
      // FIXME: Error here.
      return nullptr;
    }
  }

  case MacroDefinition::Kind::Expanded: {
    // Expand the definition with the given arguments.
    auto result = expandMacroDefinition(
        macroDef.getExpanded(), macro,
        /*genericArgs=*/{}, // attached macros don't have generic parameters
        attr->getArgs());
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
    auto externalDef =
        evaluateOrDefault(ctx.evaluator, request,
                          ExternalMacroDefinition::error("failed request"));
    if (externalDef.isError()) {
      attachedTo->diagnose(diag::external_macro_not_found,
                           external.moduleName.str(),
                           external.macroTypeName.str(), macro->getName(),
                           externalDef.getErrorMessage());
      macro->diagnose(diag::decl_declared_here, macro);
      return nullptr;
    }

#if SWIFT_BUILD_SWIFT_SYNTAX
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

    BridgedStringRef evaluatedSourceOut{nullptr, 0};
    assert(!externalDef.isError());
    swift_ASTGen_expandAttachedMacro(
        &ctx.Diags, externalDef.get(), discriminator->c_str(),
        extendedType.c_str(), conformanceList.c_str(), getRawMacroRole(role),
        astGenAttrSourceFile, attr->AtLoc.getOpaquePointerValue(),
        astGenDeclSourceFile, searchDecl->getStartLoc().getOpaquePointerValue(),
        astGenParentDeclSourceFile, parentDeclLoc, &evaluatedSourceOut);
    if (!evaluatedSourceOut.unbridged().data())
      return nullptr;
    evaluatedSource = llvm::MemoryBuffer::getMemBufferCopy(
        evaluatedSourceOut.unbridged(),
        adjustMacroExpansionBufferName(*discriminator));
    swift_ASTGen_freeBridgedString(evaluatedSourceOut);
    break;
#else
    attachedTo->diagnose(diag::macro_unsupported);
    return nullptr;
#endif
  }
  }

  SourceFile *macroSourceFile = createMacroSourceFile(
      std::move(evaluatedSource), role, attachedTo, dc, attr);

  validateMacroExpansion(macroSourceFile, macro,
                         dyn_cast<ValueDecl>(attachedTo), role);
  return macroSourceFile;
}

bool swift::accessorMacroOnlyIntroducesObservers(
    MacroDecl *macro,
    const MacroRoleAttr *attr
) {
  // Will this macro introduce observers?
  bool foundObserver = false;
  for (auto name : attr->getNames()) {
    if (name.getKind() == MacroIntroducedDeclNameKind::Named &&
        (name.getName().getBaseName().userFacingName() == "willSet" ||
         name.getName().getBaseName().userFacingName() == "didSet" ||
         name.getName().getBaseName().isConstructor())) {
      foundObserver = true;
    } else {
      // Introduces something other than an observer.
      return false;
    }
  }

  if (foundObserver)
    return true;

  // WORKAROUND: Older versions of the Observation library make
  // `ObservationIgnored` an accessor macro that implies that it makes a
  // stored property computed. Override that, because we know it produces
  // nothing.
  if (macro->getName().getBaseName().userFacingName() == "ObservationIgnored") {
    return true;
  }

  return false;
}

bool swift::accessorMacroIntroducesInitAccessor(
    MacroDecl *macro, const MacroRoleAttr *attr
) {
  for (auto name : attr->getNames()) {
    if (name.getKind() == MacroIntroducedDeclNameKind::Named &&
        (name.getName().getBaseName().isConstructor()))
      return true;
  }

  return false;
}

std::optional<unsigned> swift::expandAccessors(AbstractStorageDecl *storage,
                                               CustomAttr *attr,
                                               MacroDecl *macro) {
  if (auto var = dyn_cast<VarDecl>(storage)) {
    // Check that the variable is part of a single-variable pattern.
    auto binding = var->getParentPatternBinding();
    if (binding && binding->getSingleVar() != var) {
      var->diagnose(diag::accessor_macro_not_single_var, macro->getName());
      return std::nullopt;
    }
  }

  // Evaluate the macro.
  auto macroSourceFile =
      ::evaluateAttachedMacro(macro, storage, attr,
                              /*passParentContext=*/false, MacroRole::Accessor);
  if (!macroSourceFile)
    return std::nullopt;

  PrettyStackTraceDecl debugStack(
      "type checking expanded accessor macro", storage);

  // Trigger parsing of the sequence of accessor declarations. This has the
  // side effect of registering those accessor declarations with the storage
  // declaration, so there is nothing further to do.
  AccessorDecl *foundNonObservingAccessor = nullptr;
  AccessorDecl *foundNonObservingAccessorInMacro = nullptr;
  AccessorDecl *foundInitAccessor = nullptr;
  for (auto accessor : storage->getAllAccessors()) {
    if (accessor->isInitAccessor()) {
      if (!foundInitAccessor)
        foundInitAccessor = accessor;
      continue;
    }

    if (!accessor->isObservingAccessor()) {
      if (!foundNonObservingAccessor)
        foundNonObservingAccessor = accessor;

      if (!foundNonObservingAccessorInMacro &&
          accessor->isInMacroExpansionInContext())
        foundNonObservingAccessorInMacro = accessor;
    }
  }

  auto roleAttr = macro->getMacroRoleAttr(MacroRole::Accessor);
  bool expectObservers = accessorMacroOnlyIntroducesObservers(macro, roleAttr);
  if (foundNonObservingAccessorInMacro) {
    // If any non-observing accessor was added, mark the initializer as
    // subsumed unless it has init accessor, because the initializer in
    // such cases could be used for memberwise initialization.
    if (auto var = dyn_cast<VarDecl>(storage)) {
      if (auto binding = var->getParentPatternBinding();
          !var->getAccessor(AccessorKind::Init)) {
        unsigned index = binding->getPatternEntryIndexForVarDecl(var);
        binding->setInitializerSubsumed(index);
      }
    }

    // Also remove didSet and willSet, because they are subsumed by a
    // macro expansion that turns a stored property into a computed one.
    if (auto accessor = storage->getParsedAccessor(AccessorKind::WillSet))
      storage->removeAccessor(accessor);
    if (auto accessor = storage->getParsedAccessor(AccessorKind::DidSet))
      storage->removeAccessor(accessor);
  }

  // If the macro told us to expect only observing accessors, but the macro
  // produced a non-observing accessor, it could have converted a stored
  // property into a computed one without telling us pre-expansion. Produce
  // an error to prevent this.
  if (expectObservers && foundNonObservingAccessorInMacro) {
    storage->diagnose(
        diag::macro_nonobserver_unexpected_in_expansion, macro->getName(),
        foundNonObservingAccessorInMacro->getDescriptiveKind());
  }

  // We expected to get a non-observing accessor, but there isn't one (from
  // the macro or elsewhere), meaning that we counted on this macro to make
  // this stored property into a a computed property... but it didn't.
  // Produce an error.
  if (!expectObservers && !foundNonObservingAccessor) {
    storage->diagnose(
        diag::macro_nonobserving_accessor_missing_from_expansion,
        macro->getName());
  }

  // 'init' accessors must be documented in the macro role attribute.
  if (foundInitAccessor &&
      !accessorMacroIntroducesInitAccessor(macro, roleAttr)) {
    storage->diagnose(
        diag::macro_init_accessor_not_documented, macro->getName());
    // FIXME: Add the appropriate "names: named(init)".
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

ArrayRef<unsigned> ExpandPreambleMacroRequest::evaluate(
    Evaluator &evaluator, AbstractFunctionDecl *fn) const {
  llvm::SmallVector<unsigned, 1> bufferIDs;
  fn->forEachAttachedMacro(MacroRole::Preamble,
      [&](CustomAttr *customAttr, MacroDecl *macro) {
        auto macroSourceFile = ::evaluateAttachedMacro(
            macro, fn, customAttr, false, MacroRole::Preamble);
        if (!macroSourceFile)
          return;

        if (auto bufferID = macroSourceFile->getBufferID())
          bufferIDs.push_back(*bufferID);
      });

  std::reverse(bufferIDs.begin(), bufferIDs.end());
  return fn->getASTContext().AllocateCopy(bufferIDs);
}

std::optional<unsigned>
ExpandBodyMacroRequest::evaluate(Evaluator &evaluator,
                                 AbstractFunctionDecl *fn) const {
  std::optional<unsigned> bufferID;
  fn->forEachAttachedMacro(MacroRole::Body,
      [&](CustomAttr *customAttr, MacroDecl *macro) {
        // FIXME: Should we complain if we already expanded a body macro?
        if (bufferID)
          return;

        auto macroSourceFile = ::evaluateAttachedMacro(
            macro, fn, customAttr, false, MacroRole::Body);
        if (!macroSourceFile)
          return;

        bufferID = macroSourceFile->getBufferID();
      });

  return bufferID;
}

std::optional<unsigned>
swift::expandAttributes(CustomAttr *attr, MacroDecl *macro, Decl *member) {
  // Evaluate the macro.
  auto macroSourceFile = ::evaluateAttachedMacro(macro, member, attr,
                                                 /*passParentContext=*/true,
                                                 MacroRole::MemberAttribute);
  if (!macroSourceFile)
    return std::nullopt;

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

// Collect the protocol conformances that the macro asked about but were
// not already present on the declaration.
static TinyPtrVector<ProtocolDecl *> getIntroducedConformances(
    NominalTypeDecl *nominal, MacroRole role, MacroDecl *macro,
    SmallVectorImpl<ProtocolDecl *> *potentialConformances = nullptr) {
  SmallVector<ProtocolDecl *, 2> potentialConformancesBuffer;
  if (!potentialConformances)
    potentialConformances = &potentialConformancesBuffer;
  macro->getIntroducedConformances(nominal, role, *potentialConformances);

  TinyPtrVector<ProtocolDecl *> introducedConformances;
  for (auto protocol : *potentialConformances) {
    SmallVector<ProtocolConformance *, 2> existingConformances;
    nominal->lookupConformance(protocol, existingConformances);

    bool hasExistingConformance = llvm::any_of(
        existingConformances,
        [&](ProtocolConformance *conformance) {
          return conformance->getSourceKind() !=
              ConformanceEntryKind::PreMacroExpansion;
        });

    if (!hasExistingConformance) {
      introducedConformances.push_back(protocol);
    }
  }

  return introducedConformances;
}

std::optional<unsigned> swift::expandMembers(CustomAttr *attr, MacroDecl *macro,
                                             Decl *decl) {
  auto nominal = dyn_cast<NominalTypeDecl>(decl);
  if (!nominal) {
    auto ext = dyn_cast<ExtensionDecl>(decl);
    if (!ext)
      return std::nullopt;

    nominal = ext->getExtendedNominal();
    if (!nominal)
      return std::nullopt;
  }
  auto introducedConformances = getIntroducedConformances(
      nominal, MacroRole::Member, macro);

  // Evaluate the macro.
  auto macroSourceFile =
      ::evaluateAttachedMacro(macro, decl, attr,
                              /*passParentContext=*/false, MacroRole::Member,
                              introducedConformances);
  if (!macroSourceFile)
    return std::nullopt;

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

std::optional<unsigned> swift::expandPeers(CustomAttr *attr, MacroDecl *macro,
                                           Decl *decl) {
  auto macroSourceFile =
      ::evaluateAttachedMacro(macro, decl, attr,
                              /*passParentContext=*/false, MacroRole::Peer);
  if (!macroSourceFile)
    return std::nullopt;

  PrettyStackTraceDecl debugStack("applying expanded peer macro", decl);
  return macroSourceFile->getBufferID();
}

ArrayRef<unsigned>
ExpandExtensionMacros::evaluate(Evaluator &evaluator,
                                NominalTypeDecl *nominal) const {
  SmallVector<unsigned, 2> bufferIDs;
  for (auto customAttrConst :
       nominal->getExpandedAttrs().getAttributes<CustomAttr>()) {
    auto customAttr = const_cast<CustomAttr *>(customAttrConst);
    auto *macro = nominal->getResolvedMacro(customAttr);

    if (!macro)
      continue;

    // Prefer the extension role
    MacroRole role;
    if (macro->getMacroRoles().contains(MacroRole::Extension)) {
      role = MacroRole::Extension;
    } else if (macro->getMacroRoles().contains(MacroRole::Conformance)) {
      role = MacroRole::Conformance;
    } else {
      continue;
    }

    if (auto bufferID = expandExtensions(customAttr, macro,
                                         role, nominal))
      bufferIDs.push_back(*bufferID);
  }

  return nominal->getASTContext().AllocateCopy(bufferIDs);
}

std::optional<unsigned> swift::expandExtensions(CustomAttr *attr,
                                                MacroDecl *macro,
                                                MacroRole role,
                                                NominalTypeDecl *nominal) {
  if (nominal->getDeclContext()->isLocalContext()) {
    nominal->diagnose(diag::local_extension_macro);
    return std::nullopt;
  }

  SmallVector<ProtocolDecl *, 2> potentialConformances;
  auto introducedConformances = getIntroducedConformances(
      nominal, MacroRole::Extension, macro, &potentialConformances);
  auto macroSourceFile = ::evaluateAttachedMacro(macro, nominal, attr,
                                                 /*passParentContext=*/false,
                                                 role, introducedConformances);

  if (!macroSourceFile)
    return std::nullopt;

  PrettyStackTraceDecl debugStack(
      "applying expanded extension macro", nominal);

  auto topLevelDecls = macroSourceFile->getTopLevelDecls();
  for (auto *decl : topLevelDecls) {
    auto *extension = dyn_cast<ExtensionDecl>(decl);
    if (!extension)
      continue;

    // Bind the extension to the original nominal type.
    extension->setExtendedNominal(nominal);
    nominal->addExtension(extension);

    // Most other macro-generated declarations are visited through calling
    // 'visitAuxiliaryDecls' on the original declaration the macro is attached
    // to. We don't do this for macro-generated extensions, because the
    // extension is not a peer of the original declaration. Instead of
    // requiring all callers of 'visitAuxiliaryDecls' to understand the
    // hoisting behavior of macro-generated extensions, we make the
    // extension accessible through 'getTopLevelDecls()'.
    if (auto file = dyn_cast<FileUnit>(
            decl->getDeclContext()->getModuleScopeContext()))
      file->getOrCreateSynthesizedFile().addTopLevelDecl(extension);

    // Don't validate documented conformances for the 'conformance' role.
    if (role == MacroRole::Conformance)
      continue;

    // Extension macros can only add conformances that are documented by
    // the `@attached(extension)` attribute.
    auto inheritedTypes = extension->getInherited();
    for (auto i : inheritedTypes.getIndices()) {
      auto constraint =
          TypeResolution::forInterface(
              extension->getDeclContext(),
              TypeResolverContext::GenericRequirement,
              /*unboundTyOpener*/ nullptr,
              /*placeholderHandler*/ nullptr,
              /*packElementOpener*/ nullptr)
          .resolveType(inheritedTypes.getTypeRepr(i));

      // Already diagnosed or will be diagnosed later.
      if (constraint->is<ErrorType>() || !constraint->isConstraintType())
        continue;

      std::function<bool(Type)> isUndocumentedConformance =
          [&](Type constraint) -> bool {
            if (auto *proto = constraint->getAs<ParameterizedProtocolType>())
              return !llvm::is_contained(potentialConformances,
                                         proto->getProtocol());

            if (auto *proto = constraint->getAs<ProtocolType>())
              return !llvm::is_contained(potentialConformances,
                                         proto->getDecl());

            return llvm::any_of(
                constraint->castTo<ProtocolCompositionType>()->getMembers(),
                isUndocumentedConformance);
          };

      if (isUndocumentedConformance(constraint)) {
        extension->diagnose(
            diag::undocumented_conformance_in_expansion,
            constraint, macro->getBaseName());

        extension->setInvalid();
      }
    }
  }

  return macroSourceFile->getBufferID();
}

/// Emits an error and returns \c true if the maro reference may
/// introduce arbitrary names at global scope.
static bool diagnoseArbitraryGlobalNames(DeclContext *dc,
                                         UnresolvedMacroReference macroRef,
                                         MacroRole macroRole) {
  auto &ctx = dc->getASTContext();
  assert(macroRole == MacroRole::Declaration ||
         macroRole == MacroRole::Peer);

  if (!dc->isModuleScopeContext())
    return false;

  bool isInvalid = false;
  namelookup::forEachPotentialResolvedMacro(
      dc, macroRef.getMacroName(), macroRole,
      [&](MacroDecl *decl, const MacroRoleAttr *attr) {
        if (!isInvalid &&
            attr->hasNameKind(MacroIntroducedDeclNameKind::Arbitrary)) {
          ctx.Diags.diagnose(macroRef.getSigilLoc(),
                             diag::global_arbitrary_name,
                             getMacroRoleString(macroRole));
          isInvalid = true;

          // If this is an attached macro, mark the attribute as invalid
          // to avoid diagnosing an unknown attribute later.
          if (auto *attr = macroRef.getAttr()) {
            attr->setInvalid();
          }
        }
      });

  return isInvalid;
}

ConcreteDeclRef ResolveMacroRequest::evaluate(Evaluator &evaluator,
                                              UnresolvedMacroReference macroRef,
                                              DeclContext *dc) const {
  // Macro expressions and declarations have their own stored macro
  // reference. Use it if it's there.
  if (auto *expansion = macroRef.getFreestanding()) {
    if (auto ref = expansion->getMacroRef())
      return ref;
  }

  auto &ctx = dc->getASTContext();
  auto roles = macroRef.getMacroRoles();

  // When a macro is not found for a custom attribute, it may be a non-macro.
  // So bail out to prevent diagnostics from the contraint system.
  if (macroRef.getAttr()) {
    auto foundMacros = namelookup::lookupMacros(dc, macroRef.getModuleName(),
                                                macroRef.getMacroName(), roles);
    if (foundMacros.empty())
      return ConcreteDeclRef();
  }

  // Freestanding and peer macros applied at top-level scope cannot introduce
  // arbitrary names. Introducing arbitrary names means that any lookup
  // into this scope must expand the macro. This is a problem, because
  // resolving the macro can invoke type checking other declarations, e.g.
  // anything that the macro arguments depend on. If _anything_ the macro
  // depends on performs name unqualified name lookup, e.g. type resolution,
  // we'll get circularity errors. It's better to prevent this by banning
  // these macros at global scope if any of the macro candidates introduce
  // arbitrary names.
  if (diagnoseArbitraryGlobalNames(dc, macroRef, MacroRole::Declaration) ||
      diagnoseArbitraryGlobalNames(dc, macroRef, MacroRole::Peer))
    return ConcreteDeclRef();

  // If we already have a MacroExpansionExpr, use that. Otherwise,
  // create one.
  MacroExpansionExpr *macroExpansion;
  if (auto *expansion = macroRef.getFreestanding()) {
    if (auto *expr = dyn_cast<MacroExpansionExpr>(expansion)) {
      macroExpansion = expr;
    } else {
      macroExpansion = new (ctx) MacroExpansionExpr(
          dc, expansion->getExpansionInfo(), roles);
    }
  } else {
    SourceRange genericArgsRange = macroRef.getGenericArgsRange();
    macroExpansion = MacroExpansionExpr::create(
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
    if (auto *expansion = macroRef.getFreestanding()) {
      expansion->setMacroRef(ref);
    }
  }

  return macroExpansion->getMacroRef();
}

ArrayRef<Type>
ResolveMacroConformances::evaluate(Evaluator &evaluator,
                                   const MacroRoleAttr *attr,
                                   const Decl *decl) const {
  auto *dc = decl->getDeclContext();
  auto &ctx = dc->getASTContext();

  SmallVector<Type, 2> protocols;
  for (auto *typeExpr : attr->getConformances()) {
    if (auto *typeRepr = typeExpr->getTypeRepr()) {
      auto resolved =
          TypeResolution::forInterface(
              dc, TypeResolverContext::GenericRequirement,
              /*unboundTyOpener*/ nullptr,
              /*placeholderHandler*/ nullptr,
              /*packElementOpener*/ nullptr)
          .resolveType(typeRepr);

      if (resolved->is<ErrorType>())
        continue;

      if (!resolved->isConstraintType()) {
        diagnoseAndRemoveAttr(
            decl, attr,
            diag::extension_macro_invalid_conformance,
            resolved);
        continue;
      }

      typeExpr->setType(MetatypeType::get(resolved));
      protocols.push_back(resolved);
    } else {
      // If there's no type repr, we already have a resolved instance
      // type, e.g. because the type expr was deserialized.
      protocols.push_back(typeExpr->getInstanceType());
    }
  }

  return ctx.AllocateCopy(protocols);
}

// MARK: for IDE.

SourceFile *swift::evaluateAttachedMacro(MacroDecl *macro, Decl *attachedTo,
                                         CustomAttr *attr,
                                         bool passParentContext, MacroRole role,
                                         StringRef discriminator) {
  return ::evaluateAttachedMacro(macro, attachedTo, attr, passParentContext,
                                 role, {}, discriminator);
}

SourceFile *
swift::evaluateFreestandingMacro(FreestandingMacroExpansion *expansion,
                                 StringRef discriminator) {
  return ::evaluateFreestandingMacro(expansion, discriminator);
}

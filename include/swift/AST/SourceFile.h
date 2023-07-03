//===--- SourceFile.h - The contents of a source file -----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SOURCEFILE_H
#define SWIFT_AST_SOURCEFILE_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/Import.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"

namespace swift {

class PersistentParserState;

/// Kind of import affecting how a decl can be reexported.
///
/// This is sorted in order of priority in case the same module is imported
/// differently. e.g. a normal import (None) offers more visibility than
/// an @_spiOnly import, which offers more visibility than an
/// @_implementationOnly import. The logic of \c getRestrictedImportKind relies
/// on the order of this enum.
///
/// This is a subset of \c DisallowedOriginKind.
///
/// \sa getRestrictedImportKind
enum class RestrictedImportKind {
  MissingImport,
  ImplementationOnly,
  SPIOnly,
  None // No restriction, i.e. the module is imported publicly.
};

/// Import that limits the access level of imported entities.
using ImportAccessLevel = llvm::Optional<AttributedImport<ImportedModule>>;

/// A file containing Swift source code.
///
/// This is a .swift or .sil file (or a virtual file, such as the contents of
/// the REPL). Since it contains raw source, it must be type checked for IR
/// generation.
class SourceFile final : public FileUnit {
  friend class ParseSourceFileRequest;

public:
  /// Flags that direct how the source file is parsed.
  enum class ParsingFlags : uint8_t {
    /// Whether to disable delayed parsing for nominal type, extension, and
    /// function bodies.
    ///
    /// If set, type and function bodies will be parsed eagerly. Otherwise they
    /// will be lazily parsed when their contents is queried. This lets us avoid
    /// building AST nodes when they're not needed.
    ///
    /// This is set for primary files, since we want to type check all
    /// declarations and function bodies anyway, so there's no benefit in lazy
    /// parsing.
    DisableDelayedBodies = 1 << 0,

    /// Whether to disable evaluating the conditions of #if decls.
    ///
    /// If set, #if decls are parsed as-is. Otherwise, the bodies of any active
    /// clauses are hoisted such that they become sibling nodes with the #if
    /// decl.
    ///
    /// FIXME: When condition evaluation moves to a later phase, remove this
    /// and the associated language option.
    DisablePoundIfEvaluation = 1 << 1,

    /// Whether to save the file's parsed tokens.
    CollectParsedTokens = 1 << 2,

    /// Whether to compute the interface hash of the file.
    EnableInterfaceHash = 1 << 3,

    /// Whether to suppress warnings when parsing. This is set for secondary
    /// files, as they get parsed multiple times.
    SuppressWarnings = 1 << 4,

    /// Ensure that the SwiftSyntax tree round trips correctly.
    RoundTrip = 1 << 5,

    /// Validate the new SwiftSyntax parser diagnostics.
    ValidateNewParserDiagnostics = 1 << 6,
  };
  using ParsingOptions = OptionSet<ParsingFlags>;

  /// Retrieve the parsing options specified in the LangOptions.
  static ParsingOptions getDefaultParsingOptions(const LangOptions &langOpts);

private:
  std::unique_ptr<SourceLookupCache> Cache;
  SourceLookupCache &getCache() const;

  /// This is the list of modules that are imported by this module.
  ///
  /// This is \c None until it is filled in by the import resolution phase.
  llvm::Optional<ArrayRef<AttributedImport<ImportedModule>>> Imports;

  /// Which imports have made use of @preconcurrency.
  llvm::SmallDenseSet<AttributedImport<ImportedModule>>
      PreconcurrencyImportsUsed;

  /// A unique identifier representing this file; used to mark private decls
  /// within the file to keep them from conflicting with other files in the
  /// same module.
  mutable Identifier PrivateDiscriminator;

  /// The root TypeRefinementContext for this SourceFile.
  ///
  /// This is set during type checking.
  TypeRefinementContext *TRC = nullptr;

  /// Either the class marked \@NS/UIApplicationMain or the synthesized FuncDecl
  /// that calls main on the type marked @main.
  ValueDecl *MainDecl = nullptr;

  /// The source location of the main type.
  SourceLoc MainDeclDiagLoc;

  /// A hash of all interface-contributing tokens that have been lexed for
  /// this source file.
  ///
  /// We only collect interface hash for primary input files.
  llvm::Optional<StableHasher> InterfaceHasher;

  /// The ID for the memory buffer containing this file's source.
  ///
  /// May be -1, to indicate no association with a buffer.
  int BufferID;

  /// The parsing options for the file.
  ParsingOptions ParsingOpts;

  /// Whether this is a primary source file which we'll be generating code for.
  bool IsPrimary;

  /// The scope map that describes this source file.
  NullablePtr<ASTScope> Scope = nullptr;

   /// The set of parsed decls with opaque return types that have not yet
   /// been validated.
   llvm::SetVector<ValueDecl *> UnvalidatedDeclsWithOpaqueReturnTypes;
  
  /// The set of validated opaque return type decls in the source file.
  llvm::SmallVector<OpaqueTypeDecl *, 4> OpaqueReturnTypes;
  llvm::StringMap<OpaqueTypeDecl *> ValidatedOpaqueReturnTypes;
  /// The set of opaque type decls that have not yet been validated.
  ///
  /// \note This is populated as opaque type decls are created. Validation
  /// requires mangling the naming decl, which would lead to circularity
  /// if it were done from OpaqueResultTypeRequest.
  llvm::SetVector<OpaqueTypeDecl *> UnvalidatedOpaqueReturnTypes;

  /// The set of declarations with valid runtime discoverable attributes
  /// located in the source file.
  llvm::SetVector<ValueDecl *> DeclsWithRuntimeDiscoverableAttrs;

  /// The list of top-level items in the source file. This is \c None if
  /// they have not yet been parsed.
  /// FIXME: Once addTopLevelDecl/prependTopLevelDecl
  /// have been removed, this can become an optional ArrayRef.
  llvm::Optional<std::vector<ASTNode>> Items;

  /// The list of hoisted declarations. See Decl::isHoisted().
  /// This is only used by lldb.
  std::vector<Decl *> Hoisted;

  using SeparatelyImportedOverlayMap =
    llvm::SmallDenseMap<ModuleDecl *, llvm::SmallPtrSet<ModuleDecl *, 1>>;

  /// Keys are modules which are shadowed by one or more separately-imported
  /// overlays; values are the list of overlays shadowing them.
  ///
  /// This is used by cross-import overlays to make their members appear to
  /// be part of the underlying module. (ClangImporter overlays use a different
  /// mechanism which is not SourceFile-dependent.)
  SeparatelyImportedOverlayMap separatelyImportedOverlays;

  /// A pointer to PersistentParserState with a function reference to its
  /// deleter to handle the fact that it's forward declared.
  using ParserStatePtr =
      std::unique_ptr<PersistentParserState, void (*)(PersistentParserState *)>;

  /// Stores delayed parser state that code completion needs to be able to
  /// resume parsing at the code completion token in the file.
  ParserStatePtr DelayedParserState =
      ParserStatePtr(/*ptr*/ nullptr, /*deleter*/ nullptr);

  friend class HasImportsMatchingFlagRequest;

  /// Indicates which import options have valid caches. Storage for
  /// \c HasImportsMatchingFlagRequest.
  ImportOptions validCachedImportOptions;

  /// The cached computation of which import flags are present in the file.
  /// Storage for \c HasImportsMatchingFlagRequest.
  ImportOptions cachedImportOptions;

  friend ASTContext;

public:
  /// Appends the given declaration to the end of the top-level decls list. Do
  /// not add any additional uses of this function.
  void addTopLevelDecl(Decl *d);

  /// Prepends a declaration to the top-level decls list.
  ///
  /// FIXME: This entrypoint exists to support LLDB. Calls to this function are
  /// always a mistake, and additional uses should not be added.
  ///
  /// See rdar://58355191
  void prependTopLevelDecl(Decl *d);

  /// Add a hoisted declaration. See Decl::isHoisted().
  void addHoistedDecl(Decl *d);

  /// Add a declaration with any number of runtime disoverable attributes
  /// associated with it.
  void addDeclWithRuntimeDiscoverableAttrs(ValueDecl *);

  /// Retrieves an immutable view of the list of top-level items in this file.
  ArrayRef<ASTNode> getTopLevelItems() const;

  /// Retrieves an immutable view of the list of top-level decls in this file.
  ///
  /// NOTE: Please use getTopLevelItems() instead.
  ArrayRef<Decl *> getTopLevelDecls() const;

  /// Retrieves an immutable view of the list of hoisted decls in this file.
  /// See Decl::isHoisted().
  ArrayRef<Decl *> getHoistedDecls() const;

  /// Retrieves an immutable view of the set of all declaration with runtime
  /// discoverable attributes located in this file.
  ArrayRef<ValueDecl *> getDeclsWithRuntimeDiscoverableAttrs() const;

  /// Retrieves an immutable view of the top-level items if they have already
  /// been parsed, or \c None if they haven't. Should only be used for dumping.
  llvm::Optional<ArrayRef<ASTNode>> getCachedTopLevelItems() const {
    if (!Items)
      return llvm::None;
    return llvm::makeArrayRef(*Items);
  }

  /// Retrieve the parsing options for the file.
  ParsingOptions getParsingOptions() const { return ParsingOpts; }

  /// Whether this source file is a primary file, meaning that we're generating
  /// code for it. Note this method returns \c false in WMO.
  bool isPrimary() const { return IsPrimary; }

  /// Retrieve the \c ExportedSourceFile instance produced by ASTGen, which
  /// includes the SourceFileSyntax node corresponding to this source file.
  void *getExportedSourceFile() const;

  /// The list of local type declarations in the source file.
  llvm::SetVector<TypeDecl *> LocalTypeDecls;

  /// The list of functions defined in this file whose bodies have yet to be
  /// typechecked. They must be held in this list instead of eagerly validated
  /// because their bodies may force us to perform semantic checks of arbitrary
  /// complexity, and we currently cannot handle those checks in isolation. E.g.
  /// we cannot, in general, perform witness matching on singular requirements
  /// unless the entire conformance has been evaluated.
  std::vector<AbstractFunctionDecl *> DelayedFunctions;

  /// A mapping from Objective-C selectors to the methods that have
  /// those selectors.
  llvm::DenseMap<ObjCSelector, llvm::TinyPtrVector<AbstractFunctionDecl *>>
    ObjCMethods;

  /// List of Objective-C methods, which is used for checking unintended
  /// Objective-C overrides.
  std::vector<AbstractFunctionDecl *> ObjCMethodList;

  /// An unsatisfied, optional @objc requirement in a protocol conformance.
  using ObjCUnsatisfiedOptReq = std::pair<DeclContext *, AbstractFunctionDecl *>;

  /// List of optional @objc protocol requirements that have gone
  /// unsatisfied, which might conflict with other Objective-C methods.
  std::vector<ObjCUnsatisfiedOptReq> ObjCUnsatisfiedOptReqs;

  /// A selector that is used by two different declarations in the same class.
  struct ObjCMethodConflict {
    NominalTypeDecl *typeDecl;
    ObjCSelector selector;
    bool isInstanceMethod;

    ObjCMethodConflict(NominalTypeDecl *typeDecl, ObjCSelector selector,
                       bool isInstanceMethod)
        : typeDecl(typeDecl), selector(selector),
          isInstanceMethod(isInstanceMethod)
    {}
  };

  /// List of Objective-C member conflicts we have found during type checking.
  llvm::SetVector<ObjCMethodConflict> ObjCMethodConflicts;

  /// List of attributes added by access notes, used to emit remarks for valid
  /// ones.
  llvm::DenseMap<ValueDecl *, std::vector<DeclAttribute *>>
      AttrsAddedByAccessNotes;

  /// Describes what kind of file this is, which can affect some type checking
  /// and other behavior.
  const SourceFileKind Kind;

  enum ASTStage_t {
    /// The source file has not had its imports resolved or been type checked.
    Unprocessed,
    /// Import resolution has completed.
    ImportsResolved,
    /// Type checking has completed.
    TypeChecked
  };

  /// Defines what phases of parsing and semantic analysis are complete for a
  /// source file.
  ///
  /// Only files that have been fully processed (i.e. type-checked) will be
  /// forwarded on to IRGen.
  ASTStage_t ASTStage = Unprocessed;

  /// Virtual file paths declared by \c #sourceLocation(file:) declarations in
  /// this source file.
  llvm::SmallVector<Located<StringRef>, 0> VirtualFilePaths;

  /// Returns information about the file paths used for diagnostics and magic
  /// identifiers in this source file, including virtual filenames introduced by
  /// \c #sourceLocation(file:) declarations.
  llvm::StringMap<SourceFilePathInfo> getInfoForUsedFilePaths() const;

  SourceFile(ModuleDecl &M, SourceFileKind K, llvm::Optional<unsigned> bufferID,
             ParsingOptions parsingOpts = {}, bool isPrimary = false);

  ~SourceFile();

  bool hasImports() const {
    return Imports.has_value();
  }

  /// Retrieve an immutable view of the source file's imports.
  ArrayRef<AttributedImport<ImportedModule>> getImports() const {
    return *Imports;
  }

  /// Set the imports for this source file. This gets called by import
  /// resolution.
  void setImports(ArrayRef<AttributedImport<ImportedModule>> imports);

  /// Whether the given import has used @preconcurrency.
  bool hasImportUsedPreconcurrency(
      AttributedImport<ImportedModule> import) const;

  /// Note that the given import has used @preconcurrency/
  void setImportUsedPreconcurrency(
      AttributedImport<ImportedModule> import);

  enum ImportQueryKind {
    /// Return the results for testable or private imports.
    TestableAndPrivate,
    /// Return the results only for testable imports.
    TestableOnly,
    /// Return the results only for private imports.
    PrivateOnly
  };

  bool
  hasTestableOrPrivateImport(AccessLevel accessLevel, const ValueDecl *ofDecl,
                             ImportQueryKind kind = TestableAndPrivate) const;

  /// Does this source file have any imports with \c flag?
  /// If not, we can fast-path module checks.
  bool hasImportsWithFlag(ImportFlags flag) const;

  /// Get the most permissive restriction applied to the imports of \p module.
  RestrictedImportKind getRestrictedImportKind(const ModuleDecl *module) const;

  /// Return the import of \p targetModule from this file with the most
  /// permissive access level.
  ImportAccessLevel getImportAccessLevel(const ModuleDecl *targetModule) const;

  /// Find all SPI names imported from \p importedModule by this file,
  /// collecting the identifiers in \p spiGroups.
  virtual void
  lookupImportedSPIGroups(
                const ModuleDecl *importedModule,
                llvm::SmallSetVector<Identifier, 4> &spiGroups) const override;

  /// Is \p module imported as \c @_weakLinked by this file?
  bool importsModuleAsWeakLinked(const ModuleDecl *module) const override;

  // Is \p targetDecl accessible as an explicitly imported SPI from this file?
  bool isImportedAsSPI(const ValueDecl *targetDecl) const;

  bool shouldCrossImport() const;

  /// Register a separately-imported overlay as shadowing the module that
  /// declares it.
  ///
  /// \returns true if the overlay was added; false if it already existed.
  bool addSeparatelyImportedOverlay(ModuleDecl *overlay,
                                    ModuleDecl *declaring) {
    return std::get<1>(separatelyImportedOverlays[declaring].insert(overlay));
  }

  /// Retrieves a list of separately imported overlays which are shadowing
  /// \p declaring. If any \p overlays are returned, qualified lookups into
  /// \p declaring should be performed into \p overlays instead; since they
  /// are overlays, they will re-export \p declaring, but will also augment it
  /// with additional symbols.
  void getSeparatelyImportedOverlays(
      ModuleDecl *declaring, SmallVectorImpl<ModuleDecl *> &overlays) {
    auto i = separatelyImportedOverlays.find(declaring);
    if (i == separatelyImportedOverlays.end()) return;

    auto &value = std::get<1>(*i);
    overlays.append(value.begin(), value.end());
  }

  SWIFT_DEBUG_DUMPER(dumpSeparatelyImportedOverlays());

  llvm::SmallDenseSet<ImportedModule> MissingImportedModules;

  void addMissingImportedModule(ImportedModule module) const {
     const_cast<SourceFile *>(this)->MissingImportedModules.insert(module);
  }

  void getMissingImportedModules(
         SmallVectorImpl<ImportedModule> &imports) const override;

  void cacheVisibleDecls(SmallVectorImpl<ValueDecl *> &&globals) const;
  const SmallVectorImpl<ValueDecl *> &getCachedVisibleDecls() const;

  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           OptionSet<ModuleLookupFlags> Flags,
                           SmallVectorImpl<ValueDecl*> &result) const override;

  virtual void lookupVisibleDecls(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer,
                                  NLKind lookupKind) const override;

  virtual void lookupClassMembers(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer) const override;
  virtual void
  lookupClassMember(ImportPath::Access accessPath, DeclName name,
                    SmallVectorImpl<ValueDecl*> &results) const override;

  void lookupObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const override;

protected:
  virtual void
  lookupOperatorDirect(Identifier name, OperatorFixity fixity,
                       TinyPtrVector<OperatorDecl *> &results) const override;

  virtual void lookupPrecedenceGroupDirect(
      Identifier name,
      TinyPtrVector<PrecedenceGroupDecl *> &results) const override;

public:
  virtual void getTopLevelDecls(SmallVectorImpl<Decl*> &results) const override;

  virtual void
  getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) const override;

  virtual void
  getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl*> &results) const override;

  virtual TypeDecl *lookupLocalType(llvm::StringRef MangledName) const override;

  virtual void
  getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &results) const override;
  virtual void
  getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &results) const override;

  virtual void
  getImportedModules(SmallVectorImpl<ImportedModule> &imports,
                     ModuleDecl::ImportFilter filter) const override;

  virtual void
  collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const override;

  Identifier getDiscriminatorForPrivateDecl(const Decl *D) const override;
  Identifier getPrivateDiscriminator(bool createIfMissing = false) const;
  llvm::Optional<ExternalSourceLocs::RawLocs>
  getExternalRawLocsForDecl(const Decl *D) const override;

  virtual bool walk(ASTWalker &walker) override;

  /// The buffer ID for the file that was imported, or None if there
  /// is no associated buffer.
  llvm::Optional<unsigned> getBufferID() const {
    if (BufferID == -1)
      return llvm::None;
    return BufferID;
  }

  /// For source files created to hold the source code created by expanding
  /// a macro, this is the AST node that describes the macro expansion.
  ///
  /// The source location of this AST node is the place in the source that
  /// triggered the creation of the macro expansion whose resulting source
  /// code is in this source file. This will only produce a non-null value when
  /// the \c SourceFileKind is \c MacroExpansion.
  ASTNode getMacroExpansion() const;

  /// For source files created to hold the source code created by expanding
  /// an attached macro, this is the custom attribute that describes the macro
  /// expansion.
  ///
  /// The source location of this attribute is the place in the source that
  /// triggered the creation of the macro expansion whose resulting source
  /// code is in this source file. This will only produce a non-null value when
  /// the \c SourceFileKind is \c MacroExpansion , and the macro is an attached
  /// macro.
  CustomAttr *getAttachedMacroAttribute() const;

  /// For source files created to hold the source code created by expanding
  /// an attached macro, this is the macro role that the expansion fulfills.
  ///
  /// \Returns the fulfilled macro role, or \c None if this source file is not
  /// for a macro expansion.
  llvm::Optional<MacroRole> getFulfilledMacroRole() const;

  /// When this source file is enclosed within another source file, for example
  /// because it describes a macro expansion, return the source file it was
  /// enclosed in.
  SourceFile *getEnclosingSourceFile() const;

  /// If this buffer corresponds to a file on disk, returns the path.
  /// Otherwise, return an empty string.
  StringRef getFilename() const;

  /// Retrieve the scope that describes this source file.
  ASTScope &getScope();

  void clearScope() {
    Scope = nullptr;
  }

  /// Retrieves the previously set delayed parser state, asserting that it
  /// exists.
  PersistentParserState *getDelayedParserState() {
    // Force parsing of the top-level decls, which will set DelayedParserState
    // if necessary.
    // FIXME: Ideally the parser state should be an output of
    // ParseSourceFileRequest, but the evaluator doesn't currently support
    // move-only outputs for cached requests.
    (void)getTopLevelItems();

    auto *state = DelayedParserState.get();
    assert(state && "Didn't set any delayed parser state!");
    return state;
  }

  /// Record delayed parser state for the source file. This is needed for code
  /// completion's second pass.
  void setDelayedParserState(ParserStatePtr &&state) {
    DelayedParserState = std::move(state);
  }

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os, bool parseIfNeeded = false) const;

  /// Pretty-print the contents of this source file.
  ///
  /// \param Printer The AST printer used for printing the contents.
  /// \param PO Options controlling the printing process.
  void print(ASTPrinter &Printer, const PrintOptions &PO);
  void print(raw_ostream &OS, const PrintOptions &PO);

  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::Source;
  }
  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }

  /// True if this is a "script mode" source file that admits top-level code.
  bool isScriptMode() const {
    switch (Kind) {
    case SourceFileKind::Main:
      return true;

    case SourceFileKind::Library:
    case SourceFileKind::Interface:
    case SourceFileKind::SIL:
    case SourceFileKind::MacroExpansion:
      return false;
    }
    llvm_unreachable("bad SourceFileKind");
  }

  ValueDecl *getMainDecl() const override { return MainDecl; }
  SourceLoc getMainDeclDiagLoc() const {
    assert(hasMainDecl());
    return MainDeclDiagLoc;
  }
  SourceLoc getMainClassDiagLoc() const {
    assert(hasMainClass());
    return getMainDeclDiagLoc();
  }

  /// Register a "main" class for the module, complaining if there is more than
  /// one.
  ///
  /// Should only be called during type-checking.
  bool registerMainDecl(ValueDecl *mainDecl, SourceLoc diagLoc);

  /// True if this source file has an application entry point.
  ///
  /// This is true if the source file either is in script mode or contains
  /// a designated main class.
  bool hasEntryPoint() const override {
    return isScriptMode() || hasMainDecl();
  }

  /// Get the root refinement context for the file. The root context may be
  /// null if the context hierarchy has not been built yet. Use
  /// TypeChecker::getOrBuildTypeRefinementContext() to get a built
  /// root of the hierarchy.
  TypeRefinementContext *getTypeRefinementContext() const;

  /// Set the root refinement context for the file.
  void setTypeRefinementContext(TypeRefinementContext *TRC);

  /// Whether this file can compute an interface hash.
  bool hasInterfaceHash() const {
    return ParsingOpts.contains(ParsingFlags::EnableInterfaceHash);
  }

  /// Retrieve a fingerprint value that summarizes the declarations in this
  /// source file.
  ///
  /// Note that the interface hash merely summarizes the top-level declarations
  /// in this file. Type body fingerprints are currently implemented such that
  /// they divert tokens away from the hasher used for fingerprints. That is,
  /// changes to the bodies of types and extensions will not result in a change
  /// to the interface hash.
  ///
  /// In order for the interface hash to be enabled, this source file must be a
  /// primary and the compiler must be set in incremental mode. If this is not
  /// the case, this function will try to signal with an assert. It is useful
  /// to guard requests for the interface hash with \c hasInterfaceHash().
  Fingerprint getInterfaceHash() const;

  void dumpInterfaceHash(llvm::raw_ostream &out) {
    out << getInterfaceHash() << '\n';
  }

  /// Get this file's interface hash including the type members in the file.
  Fingerprint getInterfaceHashIncludingTypeMembers() const;

  /// If this source file has been told to collect its parsed tokens, retrieve
  /// those tokens.
  ArrayRef<Token> getAllTokens() const;

  /// Whether the parsed tokens of this source file should be saved, allowing
  /// them to be accessed from \c getAllTokens.
  bool shouldCollectTokens() const;

  /// Whether the bodies of types and functions within this file can be lazily
  /// parsed.
  bool hasDelayedBodyParsing() const;

  OpaqueTypeDecl *lookupOpaqueResultType(StringRef MangledName) override;

  /// Do not call when inside an inactive clause (\c
  /// InInactiveClauseEnvironment)) because it will later on result in a lookup
  /// to something that won't be in the ASTScope tree.
  void addUnvalidatedDeclWithOpaqueResultType(ValueDecl *vd) {
    UnvalidatedDeclsWithOpaqueReturnTypes.insert(vd);
  }

  void addOpaqueResultTypeDecl(OpaqueTypeDecl *decl) {
    UnvalidatedOpaqueReturnTypes.insert(decl);
  }

  ArrayRef<OpaqueTypeDecl *> getOpaqueReturnTypeDecls();

  /// Returns true if the source file contains concurrency in the top-level
  bool isAsyncTopLevelSourceFile() const;

private:

  /// If not \c None, the underlying vector contains the parsed tokens of this
  /// source file.
  llvm::Optional<ArrayRef<Token>> AllCollectedTokens;
};

inline SourceFile::ParsingOptions operator|(SourceFile::ParsingFlags lhs,
                                            SourceFile::ParsingFlags rhs) {
  return SourceFile::ParsingOptions(lhs) | rhs;
}

inline SourceFile &ModuleDecl::getMainSourceFile() const {
  assert(!Files.empty() && "No files added yet");
  return *cast<SourceFile>(Files.front());
}

inline FileUnit *ModuleDecl::EntryPointInfoTy::getEntryPointFile() const {
  return storage.getPointer();
}
inline void ModuleDecl::EntryPointInfoTy::setEntryPointFile(FileUnit *file) {
  assert(!storage.getPointer());
  storage.setPointer(file);
}

inline bool ModuleDecl::EntryPointInfoTy::hasEntryPoint() const {
  return storage.getPointer();
}

inline bool ModuleDecl::EntryPointInfoTy::markDiagnosedMultipleMainClasses() {
  bool res = storage.getInt().contains(Flags::DiagnosedMultipleMainClasses);
  storage.setInt(storage.getInt() | Flags::DiagnosedMultipleMainClasses);
  return !res;
}

inline bool ModuleDecl::EntryPointInfoTy::markDiagnosedMainClassWithScript() {
  bool res = storage.getInt().contains(Flags::DiagnosedMainClassWithScript);
  storage.setInt(storage.getInt() | Flags::DiagnosedMainClassWithScript);
  return !res;
}

inline void simple_display(llvm::raw_ostream &out, const SourceFile *SF) {
  assert(SF && "Cannot display null source file!");

  out << "source_file " << '\"' << SF->getFilename() << '\"';
}
} // end namespace swift

namespace llvm {

template<>
struct DenseMapInfo<swift::SourceFile::ObjCMethodConflict> {
  using ObjCMethodConflict = swift::SourceFile::ObjCMethodConflict;

  static inline ObjCMethodConflict getEmptyKey() {
    return ObjCMethodConflict(nullptr, {}, false);
  }
  static inline ObjCMethodConflict getTombstoneKey() {
    return ObjCMethodConflict(nullptr, {}, true);
  }
  static inline unsigned getHashValue(ObjCMethodConflict a) {
    return hash_combine(hash_value(a.typeDecl),
                  DenseMapInfo<swift::ObjCSelector>::getHashValue(a.selector),
                  hash_value(a.isInstanceMethod));
  }
  static bool isEqual(ObjCMethodConflict a, ObjCMethodConflict b) {
    return a.typeDecl == b.typeDecl && a.selector == b.selector &&
           a.isInstanceMethod == b.isInstanceMethod;
  }
};

}


#endif

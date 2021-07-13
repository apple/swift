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

#include "swift/AST/FileUnit.h"
#include "swift/AST/Import.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace swift {

class PersistentParserState;

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

    /// Whether to build a syntax tree.
    BuildSyntaxTree = 1 << 2,

    /// Whether to save the file's parsed tokens.
    CollectParsedTokens = 1 << 3,

    /// Whether to compute the interface hash of the file.
    EnableInterfaceHash = 1 << 4,

    /// Whether to suppress warnings when parsing. This is set for secondary
    /// files, as they get parsed multiple times.
    SuppressWarnings = 1 << 5,
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
  Optional<ArrayRef<AttributedImport<ImportedModule>>> Imports;

  /// A unique identifier representing this file; used to mark private decls
  /// within the file to keep them from conflicting with other files in the
  /// same module.
  mutable Identifier PrivateDiscriminator;

  /// A synthesized file corresponding to this file, created on-demand.
  SynthesizedFileUnit *SynthesizedFile = nullptr;

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

  /// The set of validated opaque return type decls in the source file.
  llvm::SmallVector<OpaqueTypeDecl *, 4> OpaqueReturnTypes;
  llvm::StringMap<OpaqueTypeDecl *> ValidatedOpaqueReturnTypes;
  /// The set of parsed decls with opaque return types that have not yet
  /// been validated.
  llvm::SetVector<ValueDecl *> UnvalidatedDeclsWithOpaqueReturnTypes;

  /// The list of top-level declarations in the source file. This is \c None if
  /// they have not yet been parsed.
  /// FIXME: Once addTopLevelDecl/prependTopLevelDecl
  /// have been removed, this can become an optional ArrayRef.
  Optional<std::vector<Decl *>> Decls;

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

  friend ASTContext;

public:
  /// Appends the given declaration to the end of the top-level decls list. Do
  /// not add any additional uses of this function.
  void addTopLevelDecl(Decl *d) {
    // Force decl parsing if we haven't already.
    (void)getTopLevelDecls();
    Decls->push_back(d);
  }

  /// Add a hoisted declaration. See Decl::isHoisted().
  void addHoistedDecl(Decl *d);

  /// Retrieves an immutable view of the list of top-level decls in this file.
  ArrayRef<Decl *> getTopLevelDecls() const;

  /// Retrieves an immutable view of the list of hoisted decls in this file.
  /// See Decl::isHoisted().
  ArrayRef<Decl *> getHoistedDecls() const;

  /// Retrieves an immutable view of the top-level decls if they have already
  /// been parsed, or \c None if they haven't. Should only be used for dumping.
  Optional<ArrayRef<Decl *>> getCachedTopLevelDecls() const {
    if (!Decls)
      return None;
    return llvm::makeArrayRef(*Decls);
  }

  /// Retrieve the parsing options for the file.
  ParsingOptions getParsingOptions() const { return ParsingOpts; }

  /// Whether this source file is a primary file, meaning that we're generating
  /// code for it. Note this method returns \c false in WMO.
  bool isPrimary() const { return IsPrimary; }

  /// A cache of syntax nodes that can be reused when creating the syntax tree
  /// for this file.
  swift::SyntaxParsingCache *SyntaxParsingCache = nullptr;

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
  /// Fields: classDecl, selector, isInstanceMethod.
  using ObjCMethodConflict = std::tuple<ClassDecl *, ObjCSelector, bool>;

  /// List of Objective-C member conflicts we have found during type checking.
  std::vector<ObjCMethodConflict> ObjCMethodConflicts;

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

  SourceFile(ModuleDecl &M, SourceFileKind K, Optional<unsigned> bufferID,
             ParsingOptions parsingOpts = {}, bool isPrimary = false);

  ~SourceFile();

  /// Retrieve an immutable view of the source file's imports.
  ArrayRef<AttributedImport<ImportedModule>> getImports() const {
    return *Imports;
  }

  /// Set the imports for this source file. This gets called by import
  /// resolution.
  void setImports(ArrayRef<AttributedImport<ImportedModule>> imports);

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

  /// Does this source file have any implementation-only imports?
  /// If not, we can fast-path module checks.
  bool hasImplementationOnlyImports() const;

  bool isImportedImplementationOnly(const ModuleDecl *module) const;

  /// Find all SPI names imported from \p importedModule by this file,
  /// collecting the identifiers in \p spiGroups.
  virtual void
  lookupImportedSPIGroups(
                const ModuleDecl *importedModule,
                llvm::SmallSetVector<Identifier, 4> &spiGroups) const override;

  // Is \p targetDecl accessible as an explictly imported SPI from this file?
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

  void cacheVisibleDecls(SmallVectorImpl<ValueDecl *> &&globals) const;
  const SmallVectorImpl<ValueDecl *> &getCachedVisibleDecls() const;

  virtual void lookupValue(DeclName name, NLKind lookupKind,
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

  Identifier getDiscriminatorForPrivateValue(const ValueDecl *D) const override;
  Identifier getPrivateDiscriminator() const { return PrivateDiscriminator; }
  Optional<ExternalSourceLocs::RawLocs>
  getExternalRawLocsForDecl(const Decl *D) const override;

  /// Returns the synthesized file for this source file, if it exists.
  SynthesizedFileUnit *getSynthesizedFile() const { return SynthesizedFile; };

  SynthesizedFileUnit &getOrCreateSynthesizedFile();

  virtual bool walk(ASTWalker &walker) override;

  /// The buffer ID for the file that was imported, or None if there
  /// is no associated buffer.
  Optional<unsigned> getBufferID() const {
    if (BufferID == -1)
      return None;
    return BufferID;
  }

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
    (void)getTopLevelDecls();

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
  TypeRefinementContext *getTypeRefinementContext();

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

  bool shouldBuildSyntaxTree() const;

  /// Whether the bodies of types and functions within this file can be lazily
  /// parsed.
  bool hasDelayedBodyParsing() const;

  syntax::SourceFileSyntax getSyntaxRoot() const;

  OpaqueTypeDecl *lookupOpaqueResultType(StringRef MangledName) override;

  /// Do not call when inside an inactive clause (\c
  /// InInactiveClauseEnvironment)) because it will later on result in a lookup
  /// to something that won't be in the ASTScope tree.
  void addUnvalidatedDeclWithOpaqueResultType(ValueDecl *vd) {
    UnvalidatedDeclsWithOpaqueReturnTypes.insert(vd);
  }

  ArrayRef<OpaqueTypeDecl *> getOpaqueReturnTypeDecls();

private:

  /// If not \c None, the underlying vector contains the parsed tokens of this
  /// source file.
  Optional<ArrayRef<Token>> AllCollectedTokens;

  /// The root of the syntax tree representing the source file.
  std::unique_ptr<syntax::SourceFileSyntax> SyntaxRoot;
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

#endif

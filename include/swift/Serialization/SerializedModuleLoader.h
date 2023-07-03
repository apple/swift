//===--- SerializedModuleLoader.h - Import Swift modules --------*- C++ -*-===//
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

#ifndef SWIFT_SERIALIZATION_MODULELOADER_H
#define SWIFT_SERIALIZATION_MODULELOADER_H

#include "swift/AST/FileUnit.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "llvm/Support/MemoryBuffer.h"

namespace swift {
class ModuleFile;
class PathObfuscator;
enum class ModuleLoadingBehavior;
namespace file_types {
  enum ID : uint8_t;
}

/// Specifies how to load modules when both a module interface and serialized
/// AST are present, or whether to disallow one format or the other altogether.
enum class ModuleLoadingMode {
  PreferInterface,
  PreferSerialized,
  OnlyInterface,
  OnlySerialized
};

/// Helper type used to pass and compute the sets of related filenames used by
/// \c SerializedModuleLoader subclasses.
struct SerializedModuleBaseName {
  /// The base filename, without any extension.
  SmallString<256> baseName;

  /// Creates a \c SerializedModuleBaseName.
  SerializedModuleBaseName(StringRef baseName) : baseName(baseName) { }

  /// Creates a \c SerializedModuleBaseName by contextualizing an existing one
  /// with a \c parentDir.
  SerializedModuleBaseName(StringRef parentDir,
                           const SerializedModuleBaseName &name);

  /// Gets the filename with a particular extension appended to it.
  std::string getName(file_types::ID fileTy) const;

  /// If the interface with \p baseName exists, returns its path (which may be
  /// the private interface if there is one). Return an empty optional
  /// otherwise.
  llvm::Optional<std::string>
  findInterfacePath(llvm::vfs::FileSystem &fs) const;
};

/// Common functionality shared between \c ImplicitSerializedModuleLoader,
/// \c ModuleInterfaceLoader, \c ExplicitSwiftModuleLoader
/// and \c MemoryBufferSerializedModuleLoader.
class SerializedModuleLoaderBase : public ModuleLoader {
  /// A { module, generation # } pair.
  using LoadedModulePair = std::pair<std::unique_ptr<ModuleFile>, unsigned>;
  std::vector<LoadedModulePair> LoadedModuleFiles;

  SmallVector<std::unique_ptr<ModuleFile>, 2> OrphanedModuleFiles;

protected:
  ASTContext &Ctx;
  ModuleLoadingMode LoadMode;
  bool IgnoreSwiftSourceInfoFile;
  SerializedModuleLoaderBase(ASTContext &ctx, DependencyTracker *tracker,
                             ModuleLoadingMode LoadMode,
                             bool IgnoreSwiftSourceInfoFile);

  void collectVisibleTopLevelModuleNamesImpl(SmallVectorImpl<Identifier> &names,
                                             StringRef extension) const;

  virtual bool findModule(
      ImportPath::Element moduleID, SmallVectorImpl<char> *moduleInterfacePath,
      SmallVectorImpl<char> *moduleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
      bool skipBuildingInterface, bool isTestableDependencyLookup,
      bool &isFramework, bool &isSystemModule);

  /// Attempts to search the provided directory for a loadable serialized
  /// .swiftmodule with the provided `ModuleFilename`. Subclasses must
  /// override this method to perform their custom module lookup behavior.
  ///
  /// If such a module could not be loaded, the subclass must return a
  /// `std::error_code` indicating the failure. There are two specific error
  /// codes that will be treated specially:
  /// - `errc::no_such_file_or_directory`: The module loader will stop looking
  ///   for loadable modules and will diagnose the lookup failure.
  /// - `errc::not_supported`: The module loader will stop looking for loadable
  ///   modules and will defer to the remaining module loaders to look up this
  ///   module.
  virtual std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool isTestableDependencyLookup = false) = 0;

  std::error_code
  openModuleFile(
       ImportPath::Element ModuleID,
       const SerializedModuleBaseName &BaseName,
       std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer);

  std::error_code
  openModuleDocFileIfPresent(
      ImportPath::Element ModuleID,
      const SerializedModuleBaseName &BaseName,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer);

  std::error_code
  openModuleSourceInfoFileIfPresent(
      ImportPath::Element ModuleID,
      const SerializedModuleBaseName &BaseName,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer);

  /// If the module loader subclass knows that all options have been tried for
  /// loading an architecture-specific file out of a swiftmodule bundle, try
  /// to list the architectures that \e are present.
  ///
  /// \returns true if an error diagnostic was emitted
  virtual bool maybeDiagnoseTargetMismatch(
      SourceLoc sourceLocation,
      StringRef moduleName,
      const SerializedModuleBaseName &BaseName) {
    return false;
  }

  /// Determines if the provided path is a cached artifact for dependency
  /// tracking purposes.
  virtual bool isCached(StringRef DepPath) {
    return false;
  }

  /// Scan the given serialized module file to determine dependencies.
  llvm::ErrorOr<ModuleDependencyInfo> scanModuleFile(Twine modulePath, bool isFramework);

  struct BinaryModuleImports {
    llvm::StringSet<> moduleImports;
    llvm::StringSet<> headerImports;
  };

  static llvm::ErrorOr<BinaryModuleImports>
  getImportsOfModule(Twine modulePath,
                     ModuleLoadingBehavior transitiveBehavior,
                     bool isFramework,
                     bool isRequiredOSSAModules,
                     StringRef SDKName,
                     StringRef packageName,
                     llvm::vfs::FileSystem *fileSystem,
                     PathObfuscator &recoverer);

  /// Load the module file into a buffer and also collect its module name.
  static std::unique_ptr<llvm::MemoryBuffer>
  getModuleName(ASTContext &Ctx, StringRef modulePath, std::string &Name);
public:
  virtual ~SerializedModuleLoaderBase();
  SerializedModuleLoaderBase(const SerializedModuleLoaderBase &) = delete;
  SerializedModuleLoaderBase(SerializedModuleLoaderBase &&) = delete;
  SerializedModuleLoaderBase &operator=(const SerializedModuleLoaderBase &) = delete;
  SerializedModuleLoaderBase &operator=(SerializedModuleLoaderBase &&) = delete;

  /// Attempt to load a serialized AST into the given module.
  ///
  /// If the AST cannot be loaded and \p diagLoc is present, a diagnostic is
  /// printed. (Note that \p diagLoc is allowed to be invalid.)
  LoadedFile *
  loadAST(ModuleDecl &M, llvm::Optional<SourceLoc> diagLoc,
          StringRef moduleInterfacePath, StringRef moduleInterfaceSourcePath,
          std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
          std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
          std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer,
          bool isFramework);

  bool isRequiredOSSAModules() const;

  /// Check whether the module with a given name can be imported without
  /// importing it.
  ///
  /// Note that even if this check succeeds, errors may still occur if the
  /// module is loaded in full.
  ///
  /// If a non-null \p versionInfo is provided, the module version will be
  /// parsed and populated.
  virtual bool canImportModule(ImportPath::Module named,
                               ModuleVersionInfo *versionInfo,
                               bool isTestableDependencyLookup = false) override;

  /// Import a module with the given module path.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns a FailedImportModule object.
  virtual ModuleDecl *
  loadModule(SourceLoc importLoc,
             ImportPath::Module path,
             bool AllowMemoryCache) override;


  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) override;

  virtual void loadObjCMethods(
                 NominalTypeDecl *typeDecl,
                 ObjCSelector selector,
                 bool isInstanceMethod,
                 unsigned previousGeneration,
                 llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) override;

  virtual void loadDerivativeFunctionConfigurations(
      AbstractFunctionDecl *originalAFD, unsigned previousGeneration,
      llvm::SetVector<AutoDiffConfig> &results) override;

  virtual void verifyAllModules() override;

  virtual llvm::Optional<const ModuleDependencyInfo *>
  getModuleDependencies(StringRef moduleName, ModuleDependenciesCache &cache,
                        InterfaceSubContextDelegate &delegate,
                        bool isTestableImport) override;
};

/// Imports serialized Swift modules into an ASTContext.
class ImplicitSerializedModuleLoader : public SerializedModuleLoaderBase {

  ImplicitSerializedModuleLoader(ASTContext &ctx, DependencyTracker *tracker,
                         ModuleLoadingMode loadMode, bool IgnoreSwiftSourceInfo)
    : SerializedModuleLoaderBase(ctx, tracker, loadMode, IgnoreSwiftSourceInfo)
  {}

  std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool isTestableDependencyLookup = false) override;

  bool maybeDiagnoseTargetMismatch(
      SourceLoc sourceLocation,
      StringRef moduleName,
      const SerializedModuleBaseName &BaseName) override;

public:
  virtual ~ImplicitSerializedModuleLoader();

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;

  /// Create a new importer that can load serialized Swift modules
  /// into the given ASTContext.
  static std::unique_ptr<ImplicitSerializedModuleLoader>
  create(ASTContext &ctx, DependencyTracker *tracker = nullptr,
         ModuleLoadingMode loadMode = ModuleLoadingMode::PreferSerialized,
         bool IgnoreSwiftSourceInfo = false) {
    return std::unique_ptr<ImplicitSerializedModuleLoader>{
      new ImplicitSerializedModuleLoader(ctx, tracker, loadMode, IgnoreSwiftSourceInfo)
    };
  }
};

/// Imports serialized Swift modules from a MemoryBuffer into an ASTContext.
/// This interface is primarily used by LLDB.
class MemoryBufferSerializedModuleLoader : public SerializedModuleLoaderBase {

  struct MemoryBufferInfo {
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    llvm::VersionTuple userVersion;
  };

  llvm::StringMap<MemoryBufferInfo> MemoryBuffers;

  MemoryBufferSerializedModuleLoader(ASTContext &ctx,
                                     DependencyTracker *tracker,
                                     ModuleLoadingMode loadMode,
                                     bool IgnoreSwiftSourceInfo,
                                     bool BypassResilience)
      : SerializedModuleLoaderBase(ctx, tracker, loadMode,
                                   IgnoreSwiftSourceInfo),
        BypassResilience(BypassResilience) {}

  std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool IsTestableDependencyLookup = false) override;

  bool maybeDiagnoseTargetMismatch(
      SourceLoc sourceLocation,
      StringRef moduleName,
      const SerializedModuleBaseName &BaseName) override;

  bool BypassResilience;
public:
  virtual ~MemoryBufferSerializedModuleLoader();

  bool canImportModule(ImportPath::Module named,
                       ModuleVersionInfo *versionInfo,
                       bool isTestableDependencyLookup = false) override;

  ModuleDecl *
  loadModule(SourceLoc importLoc,
             ImportPath::Module path,
             bool AllowMemoryCache = true) override;

  /// Register a memory buffer that contains the serialized module for the given
  /// access path. This API is intended to be used by LLDB to add swiftmodules
  /// discovered in the __swift_ast section of a Mach-O file (or the .swift_ast
  /// section of an ELF file) to the search path.
  ///
  /// FIXME: make this an actual import *path* once submodules are designed.
  void registerMemoryBuffer(StringRef importPath,
                            std::unique_ptr<llvm::MemoryBuffer> input,
                            llvm::VersionTuple version) {
    MemoryBuffers[importPath] = {std::move(input), version};
  }

  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override {}

  /// Create a new importer that can load serialized Swift modules
  /// into the given ASTContext.
  static std::unique_ptr<MemoryBufferSerializedModuleLoader>
  create(ASTContext &ctx, DependencyTracker *tracker = nullptr,
         ModuleLoadingMode loadMode = ModuleLoadingMode::PreferSerialized,
         bool IgnoreSwiftSourceInfo = false, bool BypassResilience = false) {
    return std::unique_ptr<MemoryBufferSerializedModuleLoader>{
        new MemoryBufferSerializedModuleLoader(
            ctx, tracker, loadMode, IgnoreSwiftSourceInfo, BypassResilience)};
  }
};


/// A file-unit loaded from a serialized AST file.
class SerializedASTFile final : public LoadedFile {
  friend class SerializedModuleLoaderBase;
  friend class SerializedSILLoader;
  friend class ModuleFile;

  ModuleFile &File;

  SerializedASTFile(ModuleDecl &M, ModuleFile &file)
    : LoadedFile(FileUnitKind::SerializedAST, M), File(file) {}

  void
  collectLinkLibrariesFromImports(ModuleDecl::LinkLibraryCallback callback) const;

public:
  /// Whether this represents a '.sib' file.
  bool isSIB() const;

  /// Returns the language version that was used to compile the contents of this
  /// file.
  const version::Version &getLanguageVersionBuiltWith() const;

  virtual bool hadLoadError() const override;

  virtual bool isSystemModule() const override;

  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           OptionSet<ModuleLookupFlags> Flags,
                           SmallVectorImpl<ValueDecl*> &results) const override;

  virtual StringRef
  getFilenameForPrivateDecl(const Decl *decl) const override;

  virtual TypeDecl *lookupLocalType(StringRef MangledName) const override;
  
  virtual OpaqueTypeDecl *
  lookupOpaqueResultType(StringRef MangledName) override;

  virtual TypeDecl *
  lookupNestedType(Identifier name,
                   const NominalTypeDecl *parent) const override;

protected:
  virtual void
  lookupOperatorDirect(Identifier name, OperatorFixity fixity,
                       TinyPtrVector<OperatorDecl *> &results) const override;

  virtual void lookupPrecedenceGroupDirect(
      Identifier name,
      TinyPtrVector<PrecedenceGroupDecl *> &results) const override;

public:
  virtual void lookupVisibleDecls(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer,
                                  NLKind lookupKind) const override;

  virtual void lookupClassMembers(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer) const override;

  virtual void
  lookupClassMember(ImportPath::Access accessPath, DeclName name,
                    SmallVectorImpl<ValueDecl*> &decls) const override;

  /// Find all Objective-C methods with the given selector.
  void lookupObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const override;

  llvm::Optional<Fingerprint>
  loadFingerprint(const IterableDeclContext *IDC) const override;

  virtual void
  lookupImportedSPIGroups(
                const ModuleDecl *importedModule,
                llvm::SmallSetVector<Identifier, 4> &spiGroups) const override;

  llvm::Optional<CommentInfo> getCommentForDecl(const Decl *D) const override;

  bool hasLoadedSwiftDoc() const override;

  llvm::Optional<StringRef> getGroupNameForDecl(const Decl *D) const override;

  llvm::Optional<StringRef>
  getSourceFileNameForDecl(const Decl *D) const override;

  llvm::Optional<unsigned> getSourceOrderForDecl(const Decl *D) const override;

  llvm::Optional<StringRef> getGroupNameByUSR(StringRef USR) const override;

  llvm::Optional<ExternalSourceLocs::RawLocs>
  getExternalRawLocsForDecl(const Decl *D) const override;

  void collectAllGroups(SmallVectorImpl<StringRef> &Names) const override;

  virtual void getTopLevelDecls(SmallVectorImpl<Decl*> &results) const override;

  virtual void getExportedPrespecializations(
      SmallVectorImpl<Decl *> &results) const override;

  virtual void
  getTopLevelDeclsWhereAttributesMatch(
      SmallVectorImpl<Decl*> &Results,
      llvm::function_ref<bool(DeclAttributes)> matchAttributes) const override;

  virtual void
  getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) const override;

  virtual void
  getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl*> &Results) const override;

  virtual void
  getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &results) const override;
  virtual void
  getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &results) const override;

  virtual void getDisplayDecls(SmallVectorImpl<Decl*> &results, bool recursive = false) const override;

  virtual void
  getImportedModules(SmallVectorImpl<ImportedModule> &imports,
                     ModuleDecl::ImportFilter filter) const override;

  virtual void
  collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const override;

  virtual void loadDependenciesForTestable(SourceLoc diagLoc) const override;

  Identifier getDiscriminatorForPrivateDecl(const Decl *D) const override;

  virtual StringRef getFilename() const override;

  virtual StringRef getLoadedFilename() const override;

  virtual StringRef getSourceFilename() const override;

  virtual StringRef getModuleDefiningPath() const override;

  virtual StringRef getExportedModuleName() const override;

  ValueDecl *getMainDecl() const override;

  bool hasEntryPoint() const override;

  virtual const clang::Module *getUnderlyingClangModule() const override;

  virtual ModuleDecl *getUnderlyingModuleIfOverlay() const override;

  virtual bool getAllGenericSignatures(
                   SmallVectorImpl<GenericSignature> &genericSignatures)
                override;

  StringRef getTargetTriple() const;

  virtual void collectBasicSourceFileInfo(
      llvm::function_ref<void(const BasicSourceFileInfo &)>) const override;

  virtual void collectSerializedSearchPath(
      llvm::function_ref<void(StringRef)> callback) const override;

  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::SerializedAST;
  }
  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }
};

/// Extract compiler arguments from an interface file buffer.
bool extractCompilerFlagsFromInterface(StringRef interfacePath,
                                       StringRef buffer, llvm::StringSaver &ArgSaver,
                                       SmallVectorImpl<const char *> &SubArgs);

/// Extract the user module version number from an interface file.
llvm::VersionTuple extractUserModuleVersionFromInterface(StringRef moduleInterfacePath);
} // end namespace swift

#endif

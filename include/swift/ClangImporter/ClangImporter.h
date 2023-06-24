//===--- ClangImporter.h - Import Clang Modules -----------------*- C++ -*-===//
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
// This file implements support for loading Clang modules into Swift.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANG_IMPORTER_H
#define SWIFT_CLANG_IMPORTER_H

#include "swift/AST/ClangModuleLoader.h"
#include "llvm/Support/VirtualFileSystem.h"

/// The maximum number of SIMD vector elements we currently try to import.
#define SWIFT_MAX_IMPORTED_SIMD_ELEMENTS 4

namespace llvm {
  class Triple;
  class FileCollectorBase;
  template<typename Fn> class function_ref;
  namespace vfs {
    class FileSystem;
    class OutputBackend;
  }
}

namespace clang {
  class ASTContext;
  class CodeGenOptions;
  class Decl;
  class DependencyCollector;
  class DiagnosticConsumer;
  class EnumConstantDecl;
  class EnumDecl;
  class MacroInfo;
  class Module;
  class ModuleMacro;
  class NamedDecl;
  class Sema;
  class TargetInfo;
  class Type;
  class VisibleDeclConsumer;
  class DeclarationName;
  class CompilerInvocation;
  class TargetOptions;
namespace tooling {
namespace dependencies {
  struct ModuleDeps;
  struct TranslationUnitDeps;
  using ModuleDepsGraph = std::vector<ModuleDeps>;
}
}
}

namespace swift {
class ASTContext;
class CompilerInvocation;
class ClangImporterOptions;
class ClangModuleUnit;
class ClangNode;
class ConcreteDeclRef;
class Decl;
class DeclContext;
class EffectiveClangContext;
class EnumDecl;
class FuncDecl;
class ImportDecl;
class IRGenOptions;
class ModuleDecl;
class NominalTypeDecl;
class StructDecl;
class SwiftLookupTable;
class TypeDecl;
class ValueDecl;
class VisibleDeclConsumer;
enum class SelectorSplitKind;

/// Kinds of optional types.
enum OptionalTypeKind : unsigned {
  /// The type is not an optional type.
  OTK_None = 0,

  /// The type is Optional<T>.
  OTK_Optional,

  /// The type is ImplicitlyUnwrappedOptional<T>.
  OTK_ImplicitlyUnwrappedOptional
};
enum { NumOptionalTypeKinds = 2 };

/// This interface is implemented by LLDB to serve as a fallback when Clang
/// modules can't be imported from source in the debugger.
///
/// During compile time, ClangImporter-imported Clang modules are compiled with
/// -gmodules, which emits a DWARF rendition of all types defined in the module
/// into the .pcm file. On Darwin, these types can be collected by
/// dsymutil. This delegate allows DWARFImporter to ask LLDB to look up a Clang
/// type by name, synthesize a Clang AST from it. DWARFImporter then hands this
/// Clang AST to ClangImporter to import the type into Swift.
class DWARFImporterDelegate {
public:
  virtual ~DWARFImporterDelegate() = default;
  /// Perform a qualified lookup of a Clang type with this name.
  /// \param kind  Only return results with this type kind.
  /// \param inModule only return results from this module.
  virtual void lookupValue(StringRef name, llvm::Optional<ClangTypeKind> kind,
                           StringRef inModule,
                           SmallVectorImpl<clang::Decl *> &results) {}
  /// vtable anchor.
  virtual void anchor();
};

// ⚠️ DANGER ⚠️
// Putting more than four types in this `PointerUnion` will break the build for
// 32-bit hosts. If we need five or more types in the future, we'll need to
// design a proper larger-than-word-sized type.
typedef llvm::PointerUnion<const clang::Decl *, const clang::MacroInfo *,
                           const clang::Type *, const clang::Token *>
    ImportDiagnosticTarget;

/// Class that imports Clang modules into Swift, mapping directly
/// from Clang ASTs over to Swift ASTs.
class ClangImporter final : public ClangModuleLoader {
  friend class ClangModuleUnit;

public:
  class Implementation;

private:
  Implementation &Impl;

  ClangImporter(ASTContext &ctx,
                DependencyTracker *tracker,
                DWARFImporterDelegate *dwarfImporterDelegate);

  /// Creates a clone of Clang importer's compiler instance that has been
  /// configured for operations on precompiled outputs (either emitting a
  /// precompiled header, emitting a precompiled module, or dumping a
  /// precompiled module).
  ///
  /// The caller of this method should set any action-specific invocation
  /// options (like FrontendOptions::ProgramAction, input files, and output
  /// paths), then create the appropriate FrontendAction and execute it.
  std::unique_ptr<clang::CompilerInstance>
  cloneCompilerInstanceForPrecompiling();

public:
  /// Create a new Clang importer that can import a suitable Clang
  /// module into the given ASTContext.
  ///
  /// \param ctx The ASTContext into which the module will be imported.
  /// The ASTContext's SearchPathOptions will be used for the Clang importer.
  ///
  /// \param swiftPCHHash A hash of Swift's various options in a compiler
  /// invocation, used to create a unique Bridging PCH if requested.
  ///
  /// \param tracker The object tracking files this compilation depends on.
  ///
  /// \param dwarfImporterDelegate A helper object that can synthesize
  /// Clang Decls from debug info. Used by LLDB.
  ///
  /// \returns a new Clang module importer, or null (with a diagnostic) if
  /// an error occurred.
  static std::unique_ptr<ClangImporter>
  create(ASTContext &ctx,
         std::string swiftPCHHash = "", DependencyTracker *tracker = nullptr,
         DWARFImporterDelegate *dwarfImporterDelegate = nullptr);

  static std::vector<std::string>
  getClangArguments(ASTContext &ctx, bool ignoreClangTarget = false);

  static std::unique_ptr<clang::CompilerInvocation>
  createClangInvocation(ClangImporter *importer,
                        const ClangImporterOptions &importerOpts,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                        ArrayRef<std::string> invocationArgStrs,
                        std::vector<std::string> *CC1Args = nullptr);
  ClangImporter(const ClangImporter &) = delete;
  ClangImporter(ClangImporter &&) = delete;
  ClangImporter &operator=(const ClangImporter &) = delete;
  ClangImporter &operator=(ClangImporter &&) = delete;

  ~ClangImporter();

  /// Only to be used by lldb-moduleimport-test.
  void setDWARFImporterDelegate(DWARFImporterDelegate &delegate);

  /// Create a new clang::DependencyCollector customized to
  /// ClangImporter's specific uses.
  static std::shared_ptr<clang::DependencyCollector> createDependencyCollector(
      IntermoduleDepTrackingMode Mode,
      std::shared_ptr<llvm::FileCollectorBase> FileCollector);

  static bool isKnownCFTypeName(llvm::StringRef name);

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;

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
                               bool isTestableImport = false) override;

  /// Import a module with the given module path.
  ///
  /// Clang modules will be imported using the Objective-C ARC dialect,
  /// with all warnings disabled.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \param AllowMemoryCache Affects only loading serialized Swift modules,
  /// this parameter has no effect in the ClangImporter.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns NULL.
  virtual ModuleDecl *loadModule(
                        SourceLoc importLoc,
                        ImportPath::Module path,
                        bool AllowMemoryCache = true)
                      override;

  /// Determine whether \c overlayDC is within an overlay module for the
  /// imported context enclosing \c importedDC.
  ///
  /// This routine is used for various hacks that are only permitted within
  /// overlays of imported modules, e.g., Objective-C bridging conformances.
  bool isInOverlayModuleForImportedModule(
                                      const DeclContext *overlayDC,
                                      const DeclContext *importedDC) override;

  /// Look for declarations associated with the given name.
  ///
  /// \param name The name we're searching for.
  void lookupValue(DeclName name, VisibleDeclConsumer &consumer) override;

  /// Look up a type declaration by its Clang name.
  ///
  /// Note that this method does no filtering. If it finds the type in a loaded
  /// module, it returns it. This is intended for use in reflection / debugging
  /// contexts where access is not a problem.
  void lookupTypeDecl(StringRef clangName, ClangTypeKind kind,
                      llvm::function_ref<void(TypeDecl *)> receiver) override;

  /// Look up type a declaration synthesized by the Clang importer itself, using
  /// a "related entity kind" to determine which type it should be. For example,
  /// this can be used to find the synthesized error struct for an
  /// NS_ERROR_ENUM.
  ///
  /// Note that this method does no filtering. If it finds the type in a loaded
  /// module, it returns it. This is intended for use in reflection / debugging
  /// contexts where access is not a problem.
  void
  lookupRelatedEntity(StringRef clangName, ClangTypeKind kind,
                      StringRef relatedEntityKind,
                      llvm::function_ref<void(TypeDecl *)> receiver) override;

  StructDecl *
  instantiateCXXClassTemplate(clang::ClassTemplateDecl *decl,
                      ArrayRef<clang::TemplateArgument> arguments) override;

  ConcreteDeclRef getCXXFunctionTemplateSpecialization(
          SubstitutionMap subst, ValueDecl *decl) override;

  FuncDecl *getCXXSynthesizedOperatorFunc(FuncDecl *decl);

  /// Just like Decl::getClangNode() except we look through to the 'Code'
  /// enum of an error wrapper struct.
  ClangNode getEffectiveClangNode(const Decl *decl) const;

  /// Look for textually included declarations from the bridging header.
  ///
  /// \param filter returns true if the given clang decl/macro should be
  /// imported and fed to the consumer
  /// \param receiver will be fed decls as they are found and imported.
  ///
  /// \c receiver is not a VisibleDeclConsumer so that it is not limited to
  /// accepting ValueDecls only.
  void lookupBridgingHeaderDecls(llvm::function_ref<bool(ClangNode)> filter,
                                llvm::function_ref<void(Decl*)> receiver) const;

  /// Look for declarations from a particular header. The header may be part of
  /// a clang module or included from the bridging header.
  ///
  /// \param filename path to the header
  /// \param filter returns true if the given clang decl/macro should be
  /// imported and fed to the consumer
  /// \param receiver will be fed decls as they are found and imported.
  ///
  /// \c receiver is not a VisibleDeclConsumer so that it is not limited to
  /// accepting ValueDecls only.
  ///
  /// \returns true if there was a problem, e.g. the file does not exist.
  bool lookupDeclsFromHeader(StringRef filename,
                             llvm::function_ref<bool(ClangNode)> filter,
                             llvm::function_ref<void(Decl*)> receiver) const;

  /// Load extensions to the given nominal type.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) override;

  virtual void loadObjCMethods(
                 NominalTypeDecl *typeDecl,
                 ObjCSelector selector,
                 bool isInstanceMethod,
                 unsigned previousGeneration,
                 llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) override;

  /// Adds a new search path to the Clang CompilerInstance, as if specified with
  /// -I or -F.
  ///
  /// \returns true if there was an error adding the search path.
  bool addSearchPath(StringRef newSearchPath, bool isFramework,
                     bool isSystem) override;

  /// Imports an Objective-C header file into the shared imported header module.
  ///
  /// \param header A header name or full path, to be used in a \#import
  /// directive.
  /// \param adapter The module that depends on the contents of this header.
  /// \param expectedSize The size of the header when the module was compiled
  ///        against it.
  /// \param expectedModTime The mtime of the header when the module was
  ///        compiled against it.
  /// \param cachedContents A buffer to use if the header has been modified
  ///        since the module was compiled.
  /// \param diagLoc A location to attach any diagnostics to if import fails.
  ///
  /// \returns true if there was an error importing the header.
  ///
  /// \sa getImportedHeaderModule
  bool importHeader(StringRef header, ModuleDecl *adapter, off_t expectedSize,
                    time_t expectedModTime, StringRef cachedContents,
                    SourceLoc diagLoc);

  /// Imports an Objective-C header file into the shared imported header module.
  ///
  /// \param header A header name or full path, to be used in a \#import
  ///        directive.
  /// \param adapter The module that depends on the contents of this header.
  /// \param diagLoc A location to attach any diagnostics to if import fails.
  /// \param trackParsedSymbols If true, tracks decls and macros that were
  ///        parsed from the bridging header.
  /// \param implicitImport If true, indicates that this import was implicit
  ///        from a reference in a module file (deprecated behavior).
  ///
  /// \returns true if there was an error importing the header.
  ///
  /// \sa getImportedHeaderModule
  bool importBridgingHeader(StringRef header, ModuleDecl *adapter,
                            SourceLoc diagLoc = {},
                            bool trackParsedSymbols = false,
                            bool implicitImport = false);

  /// Returns the module that contains imports and declarations from all loaded
  /// Objective-C header files.
  ///
  /// \sa importHeader
  ModuleDecl *getImportedHeaderModule() const override;

  /// Retrieves the Swift wrapper for the given Clang module, creating
  /// it if necessary.
  ModuleDecl *
  getWrapperForModule(const clang::Module *mod,
                      bool returnOverlayIfPossible = false) const override;

  std::string getBridgingHeaderContents(StringRef headerPath, off_t &fileSize,
                                        time_t &fileModTime);

  /// Makes a temporary replica of the ClangImporter's CompilerInstance, reads
  /// an Objective-C header file into the replica and emits a PCH file of its
  /// content. Delegates to clang for everything except construction of the
  /// replica.
  ///
  /// \sa clang::GeneratePCHAction
  bool emitBridgingPCH(StringRef headerPath, StringRef outputPCHPath,
                       bool cached);

  /// Returns true if a clang CompilerInstance can successfully read in a PCH,
  /// assuming it exists, with the current options. This can be used to find out
  /// if we need to persist a PCH for later reuse.
  bool canReadPCH(StringRef PCHFilename);

  /// Reads the original source file name from PCH.
  std::string getOriginalSourceFile(StringRef PCHFilename);

  /// Add clang dependency file names.
  ///
  /// \param files The list of file to append dependencies to.
  void addClangInvovcationDependencies(std::vector<std::string> &files);

  /// Makes a temporary replica of the ClangImporter's CompilerInstance, reads a
  /// module map into the replica and emits a PCM file for one of the modules it
  /// declares. Delegates to clang for everything except construction of the
  /// replica.
  bool emitPrecompiledModule(StringRef moduleMapPath, StringRef moduleName,
                             StringRef outputPath);

  /// Makes a temporary replica of the ClangImporter's CompilerInstance and
  /// dumps information about a PCM file (assumed to be generated by -emit-pcm
  /// or in the Swift module cache). Delegates to clang for everything except
  /// construction of the replica.
  bool dumpPrecompiledModule(StringRef modulePath, StringRef outputPath);

  bool runPreprocessor(StringRef inputPath, StringRef outputPath);
  const clang::Module *getClangOwningModule(ClangNode Node) const;
  bool hasTypedef(const clang::Decl *typeDecl) const;

  void verifyAllModules() override;

  void recordModuleDependencies(
      ModuleDependenciesCache &cache,
      const clang::tooling::dependencies::ModuleDepsGraph &clangDependencies);

  void recordBridgingHeaderOptions(
      ModuleDependencyInfo &MDI,
      const clang::tooling::dependencies::TranslationUnitDeps &deps);

  Optional<const ModuleDependencyInfo*> getModuleDependencies(
      StringRef moduleName, ModuleDependenciesCache &cache,
      InterfaceSubContextDelegate &delegate,
      bool isTestableImport = false) override;

  /// Add dependency information for the bridging header.
  ///
  /// \param moduleName the name of the Swift module whose dependency
  /// information will be augmented with information about the given
  /// bridging header.
  ///
  /// \param cache The module dependencies cache to update, with information
  /// about new Clang modules discovered along the way.
  ///
  /// \returns \c true if an error occurred, \c false otherwise
  bool addBridgingHeaderDependencies(
      StringRef moduleName,
      ModuleDependencyKind moduleKind,
      ModuleDependenciesCache &cache);
  clang::TargetInfo &getModuleAvailabilityTarget() const override;
  clang::ASTContext &getClangASTContext() const override;
  clang::Preprocessor &getClangPreprocessor() const override;
  clang::Sema &getClangSema() const override;
  const clang::CompilerInstance &getClangInstance() const override;

  /// ClangImporter's Clang instance may be configured with a different
  /// (higher) OS version than the compilation target itself in order to be able
  /// to load pre-compiled Clang modules that are aligned with the broader SDK,
  /// and match the SDK deployment target against which Swift modules are also
  /// built.
  ///
  /// In this case, we must use the Swift compiler's OS version triple when
  /// performing codegen, and the importer's Clang instance OS version triple
  /// during module loading.
  ///
  /// `ClangImporter`'s `Implementation` keeps track of a distinct `TargetInfo`
  /// and `CodeGenOpts` containers that are meant to be used by clients in
  /// IRGen. When a separate `-clang-target` is not set, they are defined to be
  /// copies of the `ClangImporter`'s built-in module-loading Clang instance.
  /// When `-clang-target` is set, they are configured with the Swift
  /// compilation's target triple and OS version (but otherwise identical)
  /// instead. To distinguish IRGen clients from module loading clients,
  /// `getModuleAvailabilityTarget` should be used instead by module-loading
  /// clients.
  clang::TargetInfo &getTargetInfo() const;
  clang::CodeGenOptions &getCodeGenOpts() const;

  std::string getClangModuleHash() const;

  /// If we already imported a given decl successfully, return the corresponding
  /// Swift decl as an Optional<Decl *>, but if we previously tried and failed
  /// to import said decl then return nullptr.
  /// Otherwise, if we have never encountered this decl previously then return
  /// None.
  Optional<Decl *> importDeclCached(const clang::NamedDecl *ClangDecl);

  // Returns true if it is expected that the macro is ignored.
  bool shouldIgnoreMacro(StringRef Name, const clang::MacroInfo *Macro);

  /// Returns the name of the given enum element as it would be imported into
  /// Swift.
  ///
  /// The return value may be an empty identifier, in which case the enum would
  /// not be imported.
  ///
  /// This is not used by the importer itself, but is used by the debugger.
  Identifier getEnumConstantName(const clang::EnumConstantDecl *enumConstant);

  /// Writes the mangled name of \p clangDecl to \p os.
  void getMangledName(raw_ostream &os, const clang::NamedDecl *clangDecl) const;

  // Print statistics from the Clang AST reader.
  void printStatistics() const override;

  /// Dump Swift lookup tables.
  void dumpSwiftLookupTables();

  /// Given the path of a Clang module, collect the names of all its submodules.
  /// Calling this function does not load the module.
  void collectSubModuleNames(
      ImportPath::Module path,
      std::vector<std::string> &names) const;

  /// Given a Clang module, decide whether this module is imported already.
  static bool isModuleImported(const clang::Module *M);

  DeclName importName(
      const clang::NamedDecl *D,
      clang::DeclarationName givenName = clang::DeclarationName()) override;

  Type importFunctionReturnType(const clang::FunctionDecl *clangDecl,
                                 DeclContext *dc) override;

  Type importVarDeclType(const clang::VarDecl *clangDecl,
                         VarDecl *swiftDecl,
                         DeclContext *dc) override;

  Optional<std::string>
  getOrCreatePCH(const ClangImporterOptions &ImporterOptions,
                 StringRef SwiftPCHHash, bool Cached);
  Optional<std::string>
  /// \param isExplicit true if the PCH filename was passed directly
  /// with -import-objc-header option.
  getPCHFilename(const ClangImporterOptions &ImporterOptions,
                 StringRef SwiftPCHHash, bool &isExplicit);

  const clang::Type *parseClangFunctionType(StringRef type,
                                            SourceLoc loc) const override;
  void printClangType(const clang::Type *type,
                      llvm::raw_ostream &os) const override;

  StableSerializationPath
  findStableSerializationPath(const clang::Decl *decl) const override;

  const clang::Decl *
  resolveStableSerializationPath(
                            const StableSerializationPath &path) const override;

  bool isSerializable(const clang::Type *type,
                      bool checkCanonical) const override;

  clang::FunctionDecl *
  instantiateCXXFunctionTemplate(ASTContext &ctx,
                                 clang::FunctionTemplateDecl *func,
                                 SubstitutionMap subst) override;

  bool isCXXMethodMutating(const clang::CXXMethodDecl *method) override;

  bool isUnsafeCXXMethod(const FuncDecl *func) override;

  bool isAnnotatedWith(const clang::CXXMethodDecl *method, StringRef attr);

  /// Find the lookup table that corresponds to the given Clang module.
  ///
  /// \param clangModule The module, or null to indicate that we're talking
  /// about the directly-parsed headers.
  SwiftLookupTable *findLookupTable(const clang::Module *clangModule) override;

  /// Determine the effective Clang context for the given Swift nominal type.
  EffectiveClangContext
  getEffectiveClangContext(const NominalTypeDecl *nominal) override;

  /// Imports a clang decl directly, rather than looking up it's name.
  Decl *importDeclDirectly(const clang::NamedDecl *decl) override;

  ValueDecl *importBaseMemberDecl(ValueDecl *decl,
                                  DeclContext *newContext) override;

  /// Emits diagnostics for any declarations named name
  /// whose direct declaration context is a TU.
  void diagnoseTopLevelValue(const DeclName &name) override;

  /// Emit diagnostics for declarations named name that are members
  /// of the provided baseType.
  void diagnoseMemberValue(const DeclName &name, const Type &baseType) override;

  /// Enable the symbolic import experimental feature for the given callback.
  void withSymbolicFeatureEnabled(llvm::function_ref<void(void)> callback);

  const clang::TypedefType *getTypeDefForCXXCFOptionsDefinition(
      const clang::Decl *candidateDecl) override;

  SourceLoc importSourceLocation(clang::SourceLocation loc) override;
};

ImportDecl *createImportDecl(ASTContext &Ctx, DeclContext *DC, ClangNode ClangN,
                             ArrayRef<clang::Module *> Exported);

/// Extract the specified-or-defaulted -module-cache-path that winds up in
/// the clang importer, for reuse as the .swiftmodule cache path when
/// building a ModuleInterfaceLoader.
std::string
getModuleCachePathFromClang(const clang::CompilerInstance &Instance);

/// Whether the given parameter name identifies a completion handler.
bool isCompletionHandlerParamName(StringRef paramName);

namespace importer {

/// Returns true if the given module has a 'cplusplus' requirement.
bool requiresCPlusPlus(const clang::Module *module);

/// Returns the pointee type if the given type is a C++ `const`
/// reference type, `None` otherwise.
llvm::Optional<clang::QualType>
getCxxReferencePointeeTypeOrNone(const clang::Type *type);

/// Returns true if the given type is a C++ `const` reference type.
bool isCxxConstReferenceType(const clang::Type *type);

} // namespace importer

struct ClangInvocationFileMapping {
  SmallVector<std::pair<std::string, std::string>, 2> redirectedFiles;
  SmallVector<std::pair<std::string, std::string>, 1> overridenFiles;
};

/// On Linux, some platform libraries (glibc, libstdc++) are not modularized.
/// We inject modulemaps for those libraries into their include directories
/// to allow using them from Swift.
ClangInvocationFileMapping getClangInvocationFileMapping(
    ASTContext &ctx,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

} // end namespace swift

#endif

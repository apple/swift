//===--- Module.h - Swift Language Module ASTs ------------------*- C++ -*-===//
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
// This file defines the Module class and its subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_MODULE_H
#define SWIFT_MODULE_H

#include "swift/AST/AccessNotes.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Import.h"
#include "swift/AST/LookupKinds.h"
#include "swift/AST/Type.h"
#include "swift/Basic/BasicSourceInfo.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MD5.h"
#include <set>

namespace clang {
  class Module;
}

namespace swift {
  enum class ArtificialMainKind : uint8_t;
  class ASTContext;
  class ASTWalker;
  class BraceStmt;
  class Decl;
  class DeclAttribute;
  class TypeDecl;
  enum class DeclKind : uint8_t;
  class ExtensionDecl;
  class DebuggerClient;
  class DeclName;
  class FileUnit;
  class FuncDecl;
  class InfixOperatorDecl;
  enum class LibraryLevel : uint8_t;
  class LinkLibrary;
  class ModuleLoader;
  class NominalTypeDecl;
  class EnumElementDecl;
  class OperatorDecl;
  class PostfixOperatorDecl;
  class PrefixOperatorDecl;
  class ProtocolConformance;
  class ProtocolDecl;
  struct PrintOptions;
  class Token;
  class TupleType;
  class Type;
  class TypeRefinementContext;
  class ValueDecl;
  class VarDecl;
  class VisibleDeclConsumer;
  class ASTScope;
  class SourceLookupCache;

namespace ast_scope {
class ASTSourceFileScope;
}

/// Discriminator for file-units.
enum class FileUnitKind {
  /// For a .swift source file.
  Source,
  /// For the compiler Builtin module.
  Builtin,
  /// A serialized Swift AST.
  SerializedAST,
  /// A synthesized file.
  Synthesized,
  /// An imported Clang module.
  ClangModule,
  /// A Clang module imported from DWARF.
  DWARFModule
};

enum class SourceFileKind {
  Library,  ///< A normal .swift file.
  Main,     ///< A .swift file that can have top-level code.
  SIL,      ///< Came from a .sil file.
  Interface, ///< Came from a .swiftinterface file, representing another module.
  MacroExpansion, ///< Came from a macro expansion.
};

/// Contains information about where a particular path is used in
/// \c SourceFiles.
struct SourceFilePathInfo {
  struct Comparator {
    bool operator () (SourceLoc lhs, SourceLoc rhs) const {
      return lhs.getOpaquePointerValue() <
             rhs.getOpaquePointerValue();
    }
  };

  SourceLoc physicalFileLoc{};
  std::set<SourceLoc, Comparator> virtualFileLocs{}; // std::set for sorting

  SourceFilePathInfo() = default;

  void merge(const SourceFilePathInfo &other) {
    if (other.physicalFileLoc.isValid()) {
      assert(!physicalFileLoc.isValid());
      physicalFileLoc = other.physicalFileLoc;
    }

    for (auto &elem : other.virtualFileLocs) {
      virtualFileLocs.insert(elem);
    }
  }

  bool operator == (const SourceFilePathInfo &other) const {
    return physicalFileLoc == other.physicalFileLoc &&
           virtualFileLocs == other.virtualFileLocs;
  }
};

/// Discriminator for resilience strategy.
enum class ResilienceStrategy : unsigned {
  /// Public nominal types: fragile
  /// Non-inlinable function bodies: resilient
  ///
  /// This is the default behavior without any flags.
  Default,

  /// Public nominal types: resilient
  /// Non-inlinable function bodies: resilient
  ///
  /// This is the behavior with -enable-library-evolution.
  Resilient
};

class OverlayFile;

/// A mapping used to find the source file that contains a particular source
/// location.
class ModuleSourceFileLocationMap;

/// A unit that allows grouping of modules by a package name.
///
/// PackageUnit is treated as an enclosing scope of ModuleDecl. Unlike other
/// DeclContext subclasses where a parent context is set in ctor, PackageUnit
/// (parent context) is set as a field in ModuleDecl (child context). It also has a
/// pointer back to the ModuleDecl, so that it can be used to return the module
/// in the existing DeclContext lookup functions, which assume ModuleDecl as
/// the top level context. Since both PackageUnit and ModuleDecl are created
/// in the ASTContext memory arena, i.e. they will be destroyed when the
/// ASTContext is destroyed, both pointng to each other is not considered risky.
///
/// See \c ModuleDecl
class PackageUnit: public DeclContext {
  /// Identifies this package and used for the equality check
  Identifier PackageName;
  /// Non-null reference to ModuleDecl that points to this package.
  /// Instead of having multiple ModuleDecls pointing to one PackageUnit, we
  /// create one PackageUnit per ModuleDecl, to make it easier to look up the
  /// module pointing to this package context, which is needed in the existing
  /// DeclContext look up functions.
  /// \see DeclContext::getModuleScopeContext
  /// \see DeclContext::getParentModule
  ModuleDecl &SourceModule;

  PackageUnit(Identifier name, ModuleDecl &src)
      : DeclContext(DeclContextKind::Package, nullptr), PackageName(name),
        SourceModule(src) {}

public:
  static PackageUnit *create(Identifier name, ModuleDecl &src,
                             ASTContext &ctx) {
    return new (ctx) PackageUnit(name, src);
  }

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::Package;
  }

  static bool classof(const PackageUnit *PU) { return true; }

  Identifier getName() const {
    return PackageName;
  }

  ModuleDecl &getSourceModule() { return SourceModule; }

  /// Equality check via package name instead of pointer comparison.
  /// Returns false if the name is empty.
  bool isSamePackageAs(PackageUnit *other) {
    return !(getName().empty()) && getName() == other->getName();
  }
};

/// The minimum unit of compilation.
///
/// A module is made up of several file-units, which are all part of the same
/// output binary and logical module (such as a single library or executable).
///
/// \sa FileUnit
class ModuleDecl
    : public DeclContext, public TypeDecl, public ASTAllocated<ModuleDecl> {
  friend class DirectOperatorLookupRequest;
  friend class DirectPrecedenceGroupLookupRequest;

  /// The ABI name of the module, if it differs from the module name.
  mutable Identifier ModuleABIName;

  /// A package this module belongs to. It's set as a property instead of a
  /// parent decl context; otherwise it will break the existing decl context
  /// lookup functions that assume ModuleDecl as the top level context.
  PackageUnit *Package = nullptr;

  /// Module name to use when referenced in clients module interfaces.
  mutable Identifier ExportAsName;

public:
  /// Produces the components of a given module's full name in reverse order.
  ///
  /// For a Swift module, this will only ever have one component, but an
  /// imported Clang module might actually be a submodule.
  ///
  /// *Note: see `StringRef operator*()` for details on the returned name for printing
  /// for a Swift module.
  class ReverseFullNameIterator {
  public:
    // Make this look like a valid STL iterator.
    using difference_type = int;
    using value_type = StringRef;
    using pointer = StringRef *;
    using reference = StringRef;
    using iterator_category = std::forward_iterator_tag;

  private:
    PointerUnion<const ModuleDecl *, const /* clang::Module */ void *> current;
  public:
    ReverseFullNameIterator() = default;
    explicit ReverseFullNameIterator(const ModuleDecl *M);
    explicit ReverseFullNameIterator(const clang::Module *clangModule) {
      current = clangModule;
    }

    /// Returns the name of the current module.
    /// Note that for a Swift module, it returns the current module's real (binary) name,
    /// which can be different from the name if module aliasing was used (see `-module-alias`).
    StringRef operator*() const;
    ReverseFullNameIterator &operator++();

    friend bool operator==(ReverseFullNameIterator left,
                           ReverseFullNameIterator right) {
      return left.current == right.current;
    }
    friend bool operator!=(ReverseFullNameIterator left,
                           ReverseFullNameIterator right) {
      return !(left == right);
    }

    /// This is a convenience function that writes the entire name, in forward
    /// order, to \p out.
    ///
    /// It calls `StringRef operator*()` under the hood (see for more detail on the
    /// returned name for a Swift module).
    void printForward(raw_ostream &out, StringRef delim = ".") const;
  };

private:
  /// If non-NULL, a plug-in that should be used when performing external
  /// lookups.
  // FIXME: Do we really need to bloat all modules with this?
  DebuggerClient *DebugClient = nullptr;

  SmallVector<FileUnit *, 2> Files;

  /// Mapping used to find the source file associated with a given source
  /// location.
  ModuleSourceFileLocationMap *sourceFileLocationMap = nullptr;

  /// The set of auxiliary source files build as part of this module.
  SmallVector<SourceFile *, 2> AuxiliaryFiles;

  llvm::SmallDenseMap<Identifier, SmallVector<OverlayFile *, 1>>
    declaredCrossImports;

  llvm::DenseMap<Identifier, SmallVector<Decl *, 2>> ObjCNameLookupCache;

  /// A description of what should be implicitly imported by each file of this
  /// module.
  const ImplicitImportInfo ImportInfo;

  std::unique_ptr<SourceLookupCache> Cache;
  SourceLookupCache &getSourceLookupCache() const;

  /// Tracks the file that will generate the module's entry point, either
  /// because it contains a class marked with \@UIApplicationMain
  /// or \@NSApplicationMain, or because it is a script file.
  class EntryPointInfoTy {
    enum class Flags {
      DiagnosedMultipleMainClasses = 1 << 0,
      DiagnosedMainClassWithScript = 1 << 1
    };
    llvm::PointerIntPair<FileUnit *, 2, OptionSet<Flags>> storage;
  public:
    EntryPointInfoTy() = default;

    FileUnit *getEntryPointFile() const;
    void setEntryPointFile(FileUnit *file);
    bool hasEntryPoint() const;

    bool markDiagnosedMultipleMainClasses();
    bool markDiagnosedMainClassWithScript();
  };

  /// Information about the file responsible for the module's entry point,
  /// if any.
  ///
  /// \see EntryPointInfoTy
  EntryPointInfoTy EntryPointInfo;

  AccessNotesFile accessNotes;

  /// Used by the debugger to bypass resilient access to fields.
  bool BypassResilience = false;

  ModuleDecl(Identifier name, ASTContext &ctx, ImplicitImportInfo importInfo);

public:
  /// Creates a new module with a given \p name.
  ///
  /// \param importInfo Information about which modules should be implicitly
  /// imported by each file of this module.
  static ModuleDecl *
  create(Identifier name, ASTContext &ctx,
         ImplicitImportInfo importInfo = ImplicitImportInfo()) {
    return new (ctx) ModuleDecl(name, ctx, importInfo);
  }

  static ModuleDecl *createMainModule(ASTContext &ctx, Identifier name,
                                      ImplicitImportInfo iinfo) {
    auto *Mod = ModuleDecl::create(name, ctx, iinfo);
    Mod->Bits.ModuleDecl.IsMainModule = true;
    return Mod;
  }

  using Decl::getASTContext;

  /// Retrieves information about which modules are implicitly imported by
  /// each file of this module.
  const ImplicitImportInfo &getImplicitImportInfo() const { return ImportInfo; }

  /// Retrieve a list of modules that each file of this module implicitly
  /// imports.
  ImplicitImportList getImplicitImports() const;

  AccessNotesFile &getAccessNotes() { return accessNotes; }
  const AccessNotesFile &getAccessNotes() const { return accessNotes; }

  /// Return whether the module was imported with resilience disabled. The
  /// debugger does this to access private fields.
  bool getBypassResilience() const { return BypassResilience; }
  /// Only to be called by MemoryBufferSerializedModuleLoader.
  void setBypassResilience() { BypassResilience = true; }

  ArrayRef<FileUnit *> getFiles() {
    assert(!Files.empty() || failedToLoad());
    return Files;
  }
  ArrayRef<const FileUnit *> getFiles() const {
    return { Files.begin(), Files.size() };
  }

  /// Add the given file to this module.
  ///
  /// FIXME: Remove this function from public view. Modules never need to add
  /// files once they are created.
  ///
  /// \warning There are very few safe points to call this function once a
  /// \c ModuleDecl has been created. If you find yourself needing to insert
  /// a file in the middle of e.g. semantic analysis, use a \c
  /// SynthesizedFileUnit instead.
  void addFile(FileUnit &newFile);

  /// Add an auxiliary source file, introduced as part of the translation.
  void addAuxiliaryFile(SourceFile &sourceFile);

  /// Produces the source file that contains the given source location, or
  /// \c nullptr if the source location isn't in this module.
  SourceFile *getSourceFileContainingLocation(SourceLoc loc);

  // Retrieve the buffer ID and source location of the outermost location that
  // caused the generation of the buffer containing \p loc. \p loc and its
  // buffer if it isn't in a generated buffer or has no original location.
  std::pair<unsigned, SourceLoc> getOriginalLocation(SourceLoc loc) const;

  /// Creates a map from \c #filePath strings to corresponding \c #fileID
  /// strings, diagnosing any conflicts.
  ///
  /// A given \c #filePath string always maps to exactly one \c #fileID string,
  /// but it is possible for \c #sourceLocation directives to introduce
  /// duplicates in the opposite direction. If there are such conflicts, this
  /// method will diagnose the conflict and choose a "winner" among the paths
  /// in a reproducible way. The \c bool paired with the \c #fileID string is
  /// \c true for paths which did not have a conflict or won a conflict, and
  /// \c false for paths which lost a conflict. Thus, if you want to generate a
  /// reverse mapping, you should drop or special-case the \c #fileID strings
  /// that are paired with \c false.
  llvm::StringMap<std::pair<std::string, /*isWinner=*/bool>>
  computeFileIDMap(bool shouldDiagnose) const;

  /// Add a file declaring a cross-import overlay.
  void addCrossImportOverlayFile(StringRef file);

  /// Collect cross-import overlay names from a given YAML file path.
  static llvm::SmallSetVector<Identifier, 4>
  collectCrossImportOverlay(ASTContext &ctx, StringRef file,
                            StringRef moduleName, StringRef& bystandingModule);

  /// If this method returns \c false, the module does not declare any
  /// cross-import overlays.
  ///
  /// This is a quick check you can use to bail out of expensive logic early;
  /// however, a \c true return doesn't guarantee that the module declares
  /// cross-import overlays--it only means that it \em might declare some.
  ///
  /// (Specifically, this method checks if the module loader found any
  /// swiftoverlay files, but does not load the files to see if they list any
  /// overlay modules.)
  bool mightDeclareCrossImportOverlays() const;

  /// Append to \p overlayNames the names of all modules that this module
  /// declares should be imported when \p bystanderName is imported.
  ///
  /// This operation is asymmetric: you will get different results if you
  /// reverse the positions of the two modules involved in the cross-import.
  void findDeclaredCrossImportOverlays(
      Identifier bystanderName, SmallVectorImpl<Identifier> &overlayNames,
      SourceLoc diagLoc) const;

  /// Get the list of all modules this module declares a cross-import with.
  void getDeclaredCrossImportBystanders(
      SmallVectorImpl<Identifier> &bystanderNames);

  /// Retrieve the ABI name of the module, which is used for metadata and
  /// mangling.
  Identifier getABIName() const;

  /// Set the ABI name of the module;
  void setABIName(Identifier name) {
    ModuleABIName = name;
  }

  /// Get the package name of this module
  /// FIXME: remove this and bump module version rdar://104723918
  Identifier getPackageName() const {
    if (auto pkg = getPackage())
      return pkg->getName();
    return Identifier();
  }

  /// Get the package associated with this module
  PackageUnit *getPackage() const { return Package; }

  /// Set the package this module is associated with
  /// FIXME: rename this with setPackage(name) rdar://104723918
  void setPackageName(Identifier name);

  Identifier getExportAsName() const { return ExportAsName; }

  void setExportAsName(Identifier name) {
    ExportAsName = name;
  }

  /// Retrieve the actual module name of an alias used for this module (if any).
  ///
  /// For example, if '-module-alias Foo=Bar' is passed in when building the main module,
  /// and this module is (a) not the main module and (b) is named Foo, then it returns
  /// the real (physically on-disk) module name Bar.
  ///
  /// If no module aliasing is set, it will return getName(), i.e. Foo.
  Identifier getRealName() const;

  /// User-defined module version number.
  llvm::VersionTuple UserModuleVersion;
  void setUserModuleVersion(llvm::VersionTuple UserVer) {
    UserModuleVersion = UserVer;
  }
  llvm::VersionTuple getUserModuleVersion() const {
    return UserModuleVersion;
  }

  void addAllowableClientName(Identifier name) {
    allowableClientNames.push_back(name);
  }
  ArrayRef<Identifier> getAllowableClientNames() const {
    return allowableClientNames;
  }
  bool allowImportedBy(ModuleDecl *importer) const;
private:

  /// An array of module names that are allowed to import this one.
  /// Any module can import this one if empty.
  std::vector<Identifier> allowableClientNames;

  /// A cache of this module's underlying module and required bystander if it's
  /// an underscored cross-import overlay.
  Optional<std::pair<ModuleDecl *, Identifier>> declaringModuleAndBystander;

  /// If this module is an underscored cross import overlay, gets the underlying
  /// module that declared it (which may itself be a cross-import overlay),
  /// along with the name of the required bystander module. Used by tooling to
  /// present overlays as if they were part of their underlying module.
  std::pair<ModuleDecl *, Identifier> getDeclaringModuleAndBystander();

  /// Update the source-file location map to make it current.
  void updateSourceFileLocationMap();

public:
  ///  If this is a traditional (non-cross-import) overlay, get its underlying
  ///  module if one exists.
  ModuleDecl *getUnderlyingModuleIfOverlay() const;

  /// Returns true if this module is an underscored cross import overlay
  /// declared by \p other or its underlying clang module, either directly or
  /// transitively (via intermediate cross-import overlays - for cross-imports
  /// involving more than two modules).
  bool isCrossImportOverlayOf(ModuleDecl *other);

  /// If this module is an underscored cross-import overlay, returns the
  /// non-underscored underlying module that declares it as an overlay, either
  /// directly or transitively (via intermediate cross-import overlays - for
  /// cross-imports involving more than two modules).
  ModuleDecl *getDeclaringModuleIfCrossImportOverlay();

  /// If this module is an underscored cross-import overlay of \p declaring or
  /// its underlying clang module, either directly or transitively, populates
  /// \p bystanderNames with the set of bystander modules that must be present
  /// alongside \p declaring for the overlay to be imported and returns true.
  /// Returns false otherwise.
  bool getRequiredBystandersIfCrossImportOverlay(
      ModuleDecl *declaring, SmallVectorImpl<Identifier> &bystanderNames);


  /// Walks and loads the declared, underscored cross-import overlays of this
  /// module and its underlying clang module, transitively, to find all cross
  /// import overlays this module underlies.
  ///
  /// This is used by tooling to present these overlays as part of this module.
  void findDeclaredCrossImportOverlaysTransitive(
      SmallVectorImpl<ModuleDecl *> &overlays);

  /// Convenience accessor for clients that know what kind of file they're
  /// dealing with.
  SourceFile &getMainSourceFile() const;

  /// Convenience accessor for clients that know what kind of file they're
  /// dealing with.
  FileUnit &getMainFile(FileUnitKind expectedKind) const;

  DebuggerClient *getDebugClient() const { return DebugClient; }
  void setDebugClient(DebuggerClient *R) {
    assert(!DebugClient && "Debugger client already set");
    DebugClient = R;
  }

  /// Returns true if this module is compiled as static library.
  bool isStaticLibrary() const {
    return Bits.ModuleDecl.StaticLibrary;
  }
  void setStaticLibrary(bool isStatic = true) {
    Bits.ModuleDecl.StaticLibrary = isStatic;
  }

  /// Returns true if this module was or is being compiled for testing.
  bool isTestingEnabled() const {
    return Bits.ModuleDecl.TestingEnabled;
  }
  void setTestingEnabled(bool enabled = true) {
    Bits.ModuleDecl.TestingEnabled = enabled;
  }

  // Returns true if this module is compiled with implicit dynamic.
  bool isImplicitDynamicEnabled() const {
    return Bits.ModuleDecl.ImplicitDynamicEnabled;
  }
  void setImplicitDynamicEnabled(bool enabled = true) {
    Bits.ModuleDecl.ImplicitDynamicEnabled = enabled;
  }

  /// Returns true if this module was or is begin compile with
  /// `-enable-private-imports`.
  bool arePrivateImportsEnabled() const {
    return Bits.ModuleDecl.PrivateImportsEnabled;
  }
  void setPrivateImportsEnabled(bool enabled = true) {
    Bits.ModuleDecl.PrivateImportsEnabled = true;
  }

  /// Returns true if there was an error trying to load this module.
  bool failedToLoad() const {
    return Bits.ModuleDecl.FailedToLoad;
  }
  void setFailedToLoad(bool failed = true) {
    Bits.ModuleDecl.FailedToLoad = failed;
  }

  bool hasResolvedImports() const {
    return Bits.ModuleDecl.HasResolvedImports;
  }
  void setHasResolvedImports() {
    Bits.ModuleDecl.HasResolvedImports = true;
  }

  ResilienceStrategy getResilienceStrategy() const {
    return ResilienceStrategy(Bits.ModuleDecl.RawResilienceStrategy);
  }
  void setResilienceStrategy(ResilienceStrategy strategy) {
    Bits.ModuleDecl.RawResilienceStrategy = unsigned(strategy);
  }

  /// Distribution level of the module.
  LibraryLevel getLibraryLevel() const;

  /// Returns true if this module was or is being compiled for testing.
  bool hasIncrementalInfo() const { return Bits.ModuleDecl.HasIncrementalInfo; }
  void setHasIncrementalInfo(bool enabled = true) {
    Bits.ModuleDecl.HasIncrementalInfo = enabled;
  }

  /// Returns true if this module was built with
  /// -experimental-hermetic-seal-at-link.
  bool hasHermeticSealAtLink() const {
    return Bits.ModuleDecl.HasHermeticSealAtLink;
  }
  void setHasHermeticSealAtLink(bool enabled = true) {
    Bits.ModuleDecl.HasHermeticSealAtLink = enabled;
  }

  /// Returns true if this module was built with C++ interoperability enabled.
  bool hasCxxInteroperability() const {
    return Bits.ModuleDecl.HasCxxInteroperability;
  }
  void setHasCxxInteroperability(bool enabled = true) {
    Bits.ModuleDecl.HasCxxInteroperability = enabled;
  }

  /// \returns true if this module is a system module; note that the StdLib is
  /// considered a system module.
  bool isSystemModule() const {
    return Bits.ModuleDecl.IsSystemModule;
  }
  void setIsSystemModule(bool flag = true);

  /// \returns true if this module is part of the stdlib or contained within
  /// the SDK. If no SDK was specified, falls back to whether the module was
  /// specified as a system module (ie. it's on the system search path).
  bool isNonUserModule() const;

public:
  /// Returns true if the module was rebuilt from a module interface instead
  /// of being built from the full source.
  bool isBuiltFromInterface() const {
    return Bits.ModuleDecl.IsBuiltFromInterface;
  }
  void setIsBuiltFromInterface(bool flag = true) {
    Bits.ModuleDecl.IsBuiltFromInterface = flag;
  }

  /// Returns true if this module is a non-Swift module that was imported into
  /// Swift.
  ///
  /// Right now that's just Clang modules.
  bool isNonSwiftModule() const {
    return Bits.ModuleDecl.IsNonSwiftModule;
  }
  /// \see #isNonSwiftModule
  void setIsNonSwiftModule(bool flag = true) {
    Bits.ModuleDecl.IsNonSwiftModule = flag;
  }

  bool isMainModule() const {
    return Bits.ModuleDecl.IsMainModule;
  }

  /// Whether this module has been compiled with comprehensive checking for
  /// concurrency, e.g., Sendable checking.
  bool isConcurrencyChecked() const {
    return Bits.ModuleDecl.IsConcurrencyChecked;
  }

  void setIsConcurrencyChecked(bool value = true) {
    Bits.ModuleDecl.IsConcurrencyChecked = value;
  }

  bool isObjCNameLookupCachePopulated() const {
    return Bits.ModuleDecl.ObjCNameLookupCachePopulated;
  }

  void setIsObjCNameLookupCachePopulated(bool value) {
    Bits.ModuleDecl.ObjCNameLookupCachePopulated = value;
  }

  /// For the main module, retrieves the list of primary source files being
  /// compiled, that is, the files we're generating code for.
  ArrayRef<SourceFile *> getPrimarySourceFiles() const;

  /// Retrieve the top-level module. If this module is already top-level, this
  /// returns itself. If this is a submodule such as \c Foo.Bar.Baz, this
  /// returns the module \c Foo.
  ModuleDecl *getTopLevelModule(bool overlay = false);

  bool isResilient() const {
    return getResilienceStrategy() != ResilienceStrategy::Default;
  }

  /// Look up a (possibly overloaded) value set at top-level scope
  /// (but with the specified access path, which may come from an import decl)
  /// within the current module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupValue(DeclName Name, NLKind LookupKind,
                   OptionSet<ModuleLookupFlags> Flags,
                   SmallVectorImpl<ValueDecl*> &Result) const;

  /// Look up a (possibly overloaded) value set at top-level scope
  /// (but with the specified access path, which may come from an import decl)
  /// within the current module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupValue(DeclName Name, NLKind LookupKind,
                   SmallVectorImpl<ValueDecl*> &Result) const {
    lookupValue(Name, LookupKind, {}, Result);
  }

  /// Look up a local type declaration by its mangled name.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  TypeDecl *lookupLocalType(StringRef MangledName) const;

  /// Look up an opaque return type by the mangled name of the declaration
  /// that defines it.
  OpaqueTypeDecl *lookupOpaqueResultType(StringRef MangledName);
  
  /// Find ValueDecls in the module and pass them to the given consumer object.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupVisibleDecls(ImportPath::Access AccessPath,
                          VisibleDeclConsumer &Consumer,
                          NLKind LookupKind) const;

private:
  void populateObjCNameLookupCache();

public:
  /// Finds top-levels decls of this module by @objc provided name.
  /// Decls that have no @objc attribute are not considered.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  ///
  /// \param Results Vector collecting the decls.
  ///
  /// \param name The @objc simple name to look for. Declarations with matching
  /// name and "anonymous" @objc attribute, as well a matching named @objc
  /// attribute will be added to Results.
  void lookupTopLevelDeclsByObjCName(SmallVectorImpl<Decl *> &Results,
                                     DeclName name);

  /// This is a hack for 'main' file parsing and the integrated REPL.
  ///
  /// FIXME: Refactor main file parsing to not pump the parser incrementally.
  /// FIXME: Remove the integrated REPL.
  void clearLookupCache();

  /// Finds all class members defined in this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupClassMembers(ImportPath::Access accessPath,
                          VisibleDeclConsumer &consumer) const;

  /// Finds class members defined in this module with the given name.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupClassMember(ImportPath::Access accessPath,
                         DeclName name,
                         SmallVectorImpl<ValueDecl*> &results) const;

  /// Look for the conformance of the given type to the given protocol.
  ///
  /// This routine determines whether the given \c type conforms to the given
  /// \c protocol.
  ///
  /// \param type The type for which we are computing conformance.
  ///
  /// \param protocol The protocol to which we are computing conformance.
  ///
  /// \param allowMissing When \c true, the resulting conformance reference
  /// might include "missing" conformances, which are synthesized for some
  /// protocols as an error recovery mechanism.
  ///
  /// \returns The result of the conformance search, which will be
  /// None if the type does not conform to the protocol or contain a
  /// ProtocolConformanceRef if it does conform.
  ProtocolConformanceRef lookupConformance(Type type, ProtocolDecl *protocol,
                                           bool allowMissing = false);

  /// Look for the conformance of the given existential type to the given
  /// protocol.
  ProtocolConformanceRef lookupExistentialConformance(Type type,
                                                      ProtocolDecl *protocol);

  /// Exposes TypeChecker functionality for querying protocol conformance.
  /// Returns a valid ProtocolConformanceRef only if all conditional
  /// requirements are successfully resolved.
  ProtocolConformanceRef conformsToProtocol(Type sourceTy,
                                            ProtocolDecl *targetProtocol);

  /// Find a member named \p name in \p container that was declared in this
  /// module.
  ///
  /// \p container may be \c this for a top-level lookup.
  ///
  /// If \p privateDiscriminator is non-empty, only matching private decls are
  /// returned; otherwise, only non-private decls are returned.
  void lookupMember(SmallVectorImpl<ValueDecl*> &results,
                    DeclContext *container, DeclName name,
                    Identifier privateDiscriminator) const;

  /// Find all Objective-C methods with the given selector.
  void lookupObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const;

  /// Find all SPI names imported from \p importedModule by this module,
  /// collecting the identifiers in \p spiGroups.
  void lookupImportedSPIGroups(
                         const ModuleDecl *importedModule,
                         llvm::SmallSetVector<Identifier, 4> &spiGroups) const;

  // Is \p attr accessible as an explicitly imported SPI from this module?
  bool isImportedAsSPI(const SpecializeAttr *attr,
                       const ValueDecl *targetDecl) const;

  // Is \p spiGroup accessible as an explicitly imported SPI from this module?
  bool isImportedAsSPI(Identifier spiGroup, const ModuleDecl *fromModule) const;

  /// Is \p module imported as \c @_weakLinked from this module?
  bool isImportedAsWeakLinked(const ModuleDecl *module) const;

  /// \sa getImportedModules
  enum class ImportFilterKind {
    /// Include imports declared with `@_exported`.
    Exported = 1 << 0,
    /// Include "regular" imports with an access-level of `public`.
    Default = 1 << 1,
    /// Include imports declared with `@_implementationOnly`.
    ImplementationOnly = 1 << 2,
    /// Include imports declared with `package import`.
    PackageOnly = 1 << 3,
    /// Include imports marked `internal` or lower. These differs form
    /// implementation-only imports by stricter type-checking and loading
    /// policies. At this moment, we can group them under the same category
    /// as they have the same loading behavior.
    InternalOrBelow = 1 << 4,
    /// Include imports declared with `@_spiOnly`.
    SPIOnly = 1 << 5,
    /// Include imports shadowed by a cross-import overlay. Unshadowed imports
    /// are included whether or not this flag is specified.
    ShadowedByCrossImportOverlay = 1 << 6
  };
  /// \sa getImportedModules
  using ImportFilter = OptionSet<ImportFilterKind>;

  /// Returns an \c ImportFilter with all elements of \c ImportFilterKind.
  constexpr static ImportFilter getImportFilterAll() {
    return {ImportFilterKind::Exported,
            ImportFilterKind::Default,
            ImportFilterKind::ImplementationOnly,
            ImportFilterKind::PackageOnly,
            ImportFilterKind::InternalOrBelow,
            ImportFilterKind::SPIOnly,
            ImportFilterKind::ShadowedByCrossImportOverlay};
  }

  /// Import kinds visible to the module declaring them.
  ///
  /// This leaves out \c ShadowedByCrossImportOverlay as even if present in
  /// the sources it's superseded by the cross-overlay as the local import.
  constexpr static ImportFilter getImportFilterLocal() {
    return {ImportFilterKind::Exported,
            ImportFilterKind::Default,
            ImportFilterKind::ImplementationOnly,
            ImportFilterKind::PackageOnly,
            ImportFilterKind::InternalOrBelow,
            ImportFilterKind::SPIOnly};
  }

  /// Looks up which modules are imported by this module.
  ///
  /// \p filter controls whether public, private, or any imports are included
  /// in this list.
  void getImportedModules(SmallVectorImpl<ImportedModule> &imports,
                          ImportFilter filter = ImportFilterKind::Exported) const;

  /// Lists modules that are not imported from a file and used in API.
  void
  getMissingImportedModules(SmallVectorImpl<ImportedModule> &imports) const;

  /// Looks up which modules are imported by this module, ignoring any that
  /// won't contain top-level decls.
  ///
  /// This is a performance hack. Do not use for anything but name lookup.
  /// May go away in the future.
  void
  getImportedModulesForLookup(SmallVectorImpl<ImportedModule> &imports) const;

  /// Has \p module been imported via an '@_implementationOnly' import
  /// instead of another kind of import?
  ///
  /// This assumes that \p module was imported.
  bool isImportedImplementationOnly(const ModuleDecl *module) const;

  /// Returns true if decl context or its content can be serialized by
  /// cross-module-optimization.
  /// The \p ctxt can e.g. be a NominalType or the context of a function.
  bool canBeUsedForCrossModuleOptimization(DeclContext *ctxt) const;

  /// Finds all top-level decls of this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  void getTopLevelDecls(SmallVectorImpl<Decl*> &Results) const;

  void getExportedPrespecializations(SmallVectorImpl<Decl *> &results) const;

  /// Finds top-level decls of this module filtered by their attributes.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  ///
  /// \param Results Vector collecting the decls.
  ///
  /// \param matchAttributes Check on the attributes of a decl to
  /// filter which decls to fully deserialize. Only decls with accepted
  /// attributes are deserialized and added to Results.
  void getTopLevelDeclsWhereAttributesMatch(
               SmallVectorImpl<Decl*> &Results,
               llvm::function_ref<bool(DeclAttributes)> matchAttributes) const;

  /// Finds all local type decls of this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  void getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &Results) const;

  /// Finds all operator decls of this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  void getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) const;

  /// Finds all precedence group decls of this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  void getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl*> &Results) const;

  /// Determines whether this module should be recursed into when calling
  /// \c getDisplayDecls.
  ///
  /// Some modules should not call \c getDisplayDecls, due to assertions
  /// in their implementation. These are usually implicit imports that would be
  /// recursed into for parsed modules. This function provides a guard against
  /// recusing into modules that should not have decls collected.
  bool shouldCollectDisplayDecls() const;

  /// Finds all top-level decls that should be displayed to a client of this
  /// module.
  ///
  /// This includes types, variables, functions, and extensions.
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  ///
  /// This can differ from \c getTopLevelDecls, e.g. it returns decls from a
  /// shadowed clang module. It does not force synthesized top-level decls that
  /// should be printed to be added; use \c swift::getTopLevelDeclsForDisplay()
  /// for that.
  void getDisplayDecls(SmallVectorImpl<Decl*> &results, bool recursive = false) const;

  using LinkLibraryCallback = llvm::function_ref<void(LinkLibrary)>;

  /// Generate the list of libraries needed to link this module, based on its
  /// imports.
  void collectLinkLibraries(LinkLibraryCallback callback) const;

  /// Get the path for the file that this module came from, or an empty
  /// string if this is not applicable.
  StringRef getModuleFilename() const;

  /// Get the path to the file defining this module, what we consider the
  /// source of truth about the module. Usually a swiftinterface file for a
  /// resilient module, a swiftmodule for a non-resilient module, or the
  /// modulemap for a clang module. Returns an empty string if not applicable.
  StringRef getModuleSourceFilename() const;

  /// Get the path to the file loaded by the compiler. Usually the binary
  /// swiftmodule file or a pcm in the cache. Returns an empty string if not
  /// applicable.
  StringRef getModuleLoadedFilename() const;

  /// \returns true if this module is the "swift" standard library module.
  bool isStdlibModule() const;

  /// \returns true if this module has standard substitutions for mangling.
  bool hasStandardSubstitutions() const;

  /// \returns true if this module is the "SwiftShims" module;
  bool isSwiftShimsModule() const;

  /// \returns true if this module is the "builtin" module.
  bool isBuiltinModule() const;

  /// \returns true if this module is the "SwiftOnoneSupport" module;
  bool isOnoneSupportModule() const;

  /// \returns true if this module is the "Foundation" module;
  bool isFoundationModule() const;

  /// \returns true if traversal was aborted, false otherwise.
  bool walk(ASTWalker &Walker);

  /// Register the file responsible for generating this module's entry point.
  ///
  /// \returns true if there was a problem adding this file.
  bool registerEntryPointFile(FileUnit *file, SourceLoc diagLoc,
                              Optional<ArtificialMainKind> kind);

  /// \returns true if this module has a main entry point.
  bool hasEntryPoint() const {
    return EntryPointInfo.hasEntryPoint();
  }

  NominalTypeDecl *getMainTypeDecl() const;

  /// Returns the associated clang module if one exists.
  const clang::Module *findUnderlyingClangModule() const;

  /// Does this module or the underlying clang module defines export_as with
  /// a value corresponding to the \p other module?
  bool isExportedAs(const ModuleDecl *other) const;

  /// Returns a generator with the components of this module's full,
  /// hierarchical name.
  ///
  /// For a Swift module, this will only ever have one component, but an
  /// imported Clang module might actually be a submodule.
  ///
  /// *Note: see `StringRef operator*()` for details on the returned name for printing
  /// for a Swift module.
  ReverseFullNameIterator getReverseFullModuleName() const {
    return ReverseFullNameIterator(this);
  }

  /// Calls \p callback for each source file of the module.
  void collectBasicSourceFileInfo(
      llvm::function_ref<void(const BasicSourceFileInfo &)> callback) const;

  void collectSerializedSearchPath(
      llvm::function_ref<void(StringRef)> callback) const;
  /// Retrieve a fingerprint value that summarizes the contents of this module.
  ///
  /// This interface hash a of a module is guaranteed to change if the interface
  /// hash of any of its (primary) source files changes. For example, when
  /// building incrementally, the interface hash of this module will change when
  /// the primaries contributing to its content changes. In contrast, when
  /// a module is deserialized, the hash of every source file contributes to
  /// the module's interface hash. It therefore serves as an effective, if
  /// coarse-grained, way of determining when top-level changes to a module's
  /// contents have been made.
  Fingerprint getFingerprint() const;

  /// Returns an approximation of whether the given module could be
  /// redistributed and consumed by external clients.
  ///
  /// FIXME: The scope of this computation should be limited entirely to
  /// RenamedDeclRequest. Unfortunately, it has been co-opted to support the
  /// \c SerializeOptionsForDebugging hack. Once this information can be
  /// transferred from module files to the dSYMs, remove this.
  bool isExternallyConsumed() const;

  SWIFT_DEBUG_DUMPER(dumpDisplayDecls());
  SWIFT_DEBUG_DUMPER(dumpTopLevelDecls());

  SourceRange getSourceRange() const { return SourceRange(); }

  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Module;
  }

  using ASTAllocated<ModuleDecl>::operator new;
  using ASTAllocated<ModuleDecl>::operator delete;
};

/// Wraps either a swift module or a clang one.
/// FIXME: Should go away once swift modules can support submodules natively.
class ModuleEntity {
  llvm::PointerUnion<const ModuleDecl *, const /* clang::Module */ void *> Mod;

public:
  ModuleEntity() = default;
  ModuleEntity(const ModuleDecl *Mod) : Mod(Mod) {}
  ModuleEntity(const clang::Module *Mod) : Mod(static_cast<const void *>(Mod)){}

  /// @param useRealNameIfAliased Whether to use the module's real name in case
  ///                             module aliasing is used. For example, if a file
  ///                             has `import Foo` and `-module-alias Foo=Bar` is
  ///                             passed, treat Foo as an alias and Bar as the real
  ///                             module name as its dependency. This only applies
  ///                             to Swift modules.
  /// @return The module name; for Swift modules, the real module name could be
  ///         different from the name if module aliasing is used.
  StringRef getName(bool useRealNameIfAliased = false) const;

  /// For Swift modules, it returns the same result as \c ModuleEntity::getName(bool).
  /// For Clang modules, it returns the result of \c clang::Module::getFullModuleName.
  std::string getFullName(bool useRealNameIfAliased = false) const;

  bool isSystemModule() const;
  bool isNonUserModule() const;
  bool isBuiltinModule() const;
  const ModuleDecl *getAsSwiftModule() const;
  const clang::Module *getAsClangModule() const;

  void *getOpaqueValue() const {
    assert(!Mod.isNull());
    return Mod.getOpaqueValue();
  }

  explicit operator bool() const { return !Mod.isNull(); }
};

inline bool DeclContext::isModuleContext() const {
  if (auto D = getAsDecl())
    return ModuleDecl::classof(D);
  return false;
}

inline bool DeclContext::isModuleScopeContext() const {
  if (ParentAndKind.getInt() == ASTHierarchy::FileUnit)
    return true;
  return isModuleContext();
}

inline bool DeclContext::isPackageContext() const {
  return ParentAndKind.getInt() == ASTHierarchy::Package;
}

/// Extract the source location from the given module declaration.
inline SourceLoc extractNearestSourceLoc(const ModuleDecl *mod) {
  return extractNearestSourceLoc(static_cast<const Decl *>(mod));
}

/// Collects modules that this module imports via `@_exported import`.
void collectParsedExportedImports(const ModuleDecl *M,
                                  SmallPtrSetImpl<ModuleDecl *> &Imports,
                                  llvm::SmallDenseMap<ModuleDecl *, SmallPtrSet<Decl *, 4>, 4> &QualifiedImports,
                                  llvm::function_ref<bool(AttributedImport<ImportedModule>)> includeImport = nullptr);

} // end namespace swift

#endif

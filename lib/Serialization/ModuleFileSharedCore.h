//===--- ModuleFileSharedCore.h - Core of a serialized module ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SERIALIZATION_MODULEFILECORE_H
#define SWIFT_SERIALIZATION_MODULEFILECORE_H

#include "ModuleFormat.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/Module.h"
#include "swift/Serialization/Validation.h"
#include "llvm/Bitstream/BitstreamReader.h"

namespace llvm {
  template <typename Info> class OnDiskIterableChainedHashTable;
}

namespace swift {

/// How a dependency should be loaded.
///
/// \sa getTransitiveLoadingBehavior
enum class ModuleLoadingBehavior {
  Required,
  Optional,
  Ignored
};

/// Serialized core data of a module. The difference with `ModuleFile` is that
/// `ModuleFileSharedCore` provides immutable data and is independent of a
/// particular ASTContext. It is designed to be able to be shared across
/// multiple `ModuleFile`s of different `ASTContext`s in a thread-safe manner.
///
/// It is **important** to preserve the following properties for
/// `ModuleFileSharedCore`:
///   * a `ModuleFile` should access its assigned `ModuleFileSharedCore` as
///   immutable and thread-safe
///   * `ModuleFileSharedCore` should be Independent of an `ASTContext` object.
class ModuleFileSharedCore {
  friend class ModuleFile;
  using DeclID = serialization::DeclID;
  using Status = serialization::Status;

  /// The module file data.
  std::unique_ptr<llvm::MemoryBuffer> ModuleInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> ModuleDocInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> ModuleSourceInfoInputBuffer;

  /// The cursor used to lazily load things from the file.
  llvm::BitstreamCursor DeclTypeCursor;

  llvm::BitstreamCursor SILCursor;
  llvm::BitstreamCursor SILIndexCursor;
  llvm::BitstreamCursor DeclMemberTablesCursor;

  /// The name of the module.
  StringRef Name;

  /// The target the module was built for.
  StringRef TargetTriple;

  /// The canonical name of the SDK the module was built with.
  StringRef SDKName;

  /// The name of the module interface this module was compiled from.
  ///
  /// Empty if this module didn't come from an interface file.
  StringRef ModuleInterfacePath;

  /// The module interface path if this module is adjacent to such an interface
  /// or it was itself compiled from an interface. Empty otherwise.
  StringRef CorrespondingInterfacePath;

  /// The Swift compatibility version in use when this module was built.
  version::Version CompatibilityVersion;

  /// User-defined module version number.
  llvm::VersionTuple UserModuleVersion;

  /// The data blob containing all of the module's identifiers.
  StringRef IdentifierData;

  /// Full blob from the misc. version field of the metadata block. This should
  /// include the version string of the compiler that built the module.
  StringRef MiscVersion;

  /// The module ABI name.
  StringRef ModuleABIName;

  /// The name of the package this module belongs to.
  StringRef ModulePackageName;

  /// Module name to use when referenced in clients module interfaces.
  StringRef ModuleExportAsName;

  /// \c true if this module has incremental dependency information.
  bool HasIncrementalInfo = false;

  /// \c true if this module was compiled with -enable-ossa-modules.
  bool RequiresOSSAModules;

  /// An array of module names that are allowed to import this one.
  ArrayRef<StringRef> AllowableClientNames;

public:
  /// Represents another module that has been imported as a dependency.
  class Dependency {
  public:
    const StringRef RawPath;
    const StringRef RawSPIs;

  private:
    using ImportFilterKind = ModuleDecl::ImportFilterKind;
    const unsigned RawImportControl : 3;
    const unsigned IsHeader : 1;
    const unsigned IsScoped : 1;

    static unsigned rawControlFromKind(ImportFilterKind importKind) {
      return llvm::countTrailingZeros(static_cast<unsigned>(importKind));
    }
    ImportFilterKind getImportControl() const {
      return static_cast<ImportFilterKind>(1 << RawImportControl);
    }

    Dependency(StringRef path, StringRef spiGroups, bool isHeader,
               ImportFilterKind importControl, bool isScoped)
        : RawPath(path),
          RawSPIs(spiGroups),
          RawImportControl(rawControlFromKind(importControl)),
          IsHeader(isHeader),
          IsScoped(isScoped) {
      assert(llvm::countPopulation(static_cast<unsigned>(importControl)) == 1 &&
             "must be a particular filter option, not a bitset");
      assert(getImportControl() == importControl && "not enough bits");
    }

  public:
   Dependency(StringRef path, StringRef spiGroups,
              ImportFilterKind importControl, bool isScoped)
       : Dependency(path, spiGroups, false, importControl, isScoped) {}

   static Dependency forHeader(StringRef headerPath, bool exported) {
     auto importControl =
         exported ? ImportFilterKind::Exported : ImportFilterKind::Default;
     return Dependency(headerPath, StringRef(), true, importControl, false);
    }

    bool isExported() const {
      return getImportControl() == ImportFilterKind::Exported;
    }
    bool isImplementationOnly() const {
      return getImportControl() == ImportFilterKind::ImplementationOnly;
    }
    bool isInternalOrBelow() const {
      return getImportControl() == ImportFilterKind::InternalOrBelow;
    }
    bool isPackageOnly() const {
      return getImportControl() == ImportFilterKind::PackageOnly;
    }

    bool isHeader() const { return IsHeader; }
    bool isScoped() const { return IsScoped; }

    std::string getPrettyPrintedPath() const;
  };

private:
  /// All modules this module depends on.
  SmallVector<Dependency, 8> Dependencies;

  /// Search paths this module may provide.
  ///
  /// This is not intended for use by frameworks, but may show up in debug
  /// modules.
  std::vector<serialization::SearchPath> SearchPaths;

  /// Info for the (lone) imported header for this module.
  struct {
    off_t fileSize;
    time_t fileModTime;
    StringRef contents;
  } importedHeaderInfo = {};

  /// All of this module's link-time dependencies.
  SmallVector<LinkLibrary, 8> LinkLibraries;

public:
  using RawBitOffset = uint64_t;

private:
  /// An allocator for buffers owned by the file.
  llvm::BumpPtrAllocator Allocator;

  /// Allocates a buffer using #Allocator and initializes it with the contents
  /// of the container \p rawData, then stores it in \p buffer.
  ///
  /// \p buffer is passed as an argument rather than returned so that the
  /// element type can be inferred.
  template <typename T, typename RawData>
  void allocateBuffer(MutableArrayRef<T> &buffer, const RawData &rawData);

  /// Allocates a buffer using #Allocator and initializes it with the contents
  /// of the container \p rawData, then stores it in \p buffer.
  ///
  /// \p buffer is passed as an argument rather than returned so that the
  /// element type can be inferred.
  template <typename T, typename RawData>
  void allocateBuffer(ArrayRef<T> &buffer, const RawData &rawData) {
    assert(buffer.empty());
    MutableArrayRef<T> result;
    allocateBuffer(result, rawData);
    buffer = result;
  }

  /// Decls referenced by this module.
  ArrayRef<RawBitOffset> Decls;

  /// Local DeclContexts referenced by this module.
  ArrayRef<RawBitOffset> LocalDeclContexts;

  /// Protocol conformances referenced by this module.
  ArrayRef<RawBitOffset> Conformances;

  /// Pack conformances referenced by this module.
  ArrayRef<RawBitOffset> PackConformances;

  /// SILLayouts referenced by this module.
  ArrayRef<RawBitOffset> SILLayouts;

  /// Types referenced by this module.
  ArrayRef<RawBitOffset> Types;

  /// Clang types referenced by this module.
  ArrayRef<RawBitOffset> ClangTypes;

  /// Generic signatures referenced by this module.
  ArrayRef<RawBitOffset> GenericSignatures;

  /// Generic environments referenced by this module.
  ArrayRef<RawBitOffset> GenericEnvironments;

  /// Substitution maps referenced by this module.
  ArrayRef<RawBitOffset> SubstitutionMaps;

  /// Identifiers referenced by this module.
  ArrayRef<RawBitOffset> Identifiers;

  class DeclTableInfo;
  using SerializedDeclTable =
      llvm::OnDiskIterableChainedHashTable<DeclTableInfo>;

  class ExtensionTableInfo;
  using SerializedExtensionTable =
      llvm::OnDiskIterableChainedHashTable<ExtensionTableInfo>;

  class LocalDeclTableInfo;
  using SerializedLocalDeclTable =
      llvm::OnDiskIterableChainedHashTable<LocalDeclTableInfo>;

  using OpaqueReturnTypeDeclTableInfo = LocalDeclTableInfo;
  using SerializedOpaqueReturnTypeDeclTable =
      llvm::OnDiskIterableChainedHashTable<OpaqueReturnTypeDeclTableInfo>;

  class NestedTypeDeclsTableInfo;
  using SerializedNestedTypeDeclsTable =
      llvm::OnDiskIterableChainedHashTable<NestedTypeDeclsTableInfo>;

  class DeclMemberNamesTableInfo;
  using SerializedDeclMemberNamesTable =
      llvm::OnDiskIterableChainedHashTable<DeclMemberNamesTableInfo>;

  class DeclMembersTableInfo;
  using SerializedDeclMembersTable =
      llvm::OnDiskIterableChainedHashTable<DeclMembersTableInfo>;

  class DeclFingerprintsTableInfo;
  using SerializedDeclFingerprintsTable =
      llvm::OnDiskIterableChainedHashTable<DeclFingerprintsTableInfo>;

  std::unique_ptr<SerializedDeclTable> TopLevelDecls;
  std::unique_ptr<SerializedDeclTable> OperatorDecls;
  std::unique_ptr<SerializedDeclTable> PrecedenceGroupDecls;
  std::unique_ptr<SerializedDeclTable> ClassMembersForDynamicLookup;
  std::unique_ptr<SerializedDeclTable> OperatorMethodDecls;
  std::unique_ptr<SerializedExtensionTable> ExtensionDecls;
  std::unique_ptr<SerializedLocalDeclTable> LocalTypeDecls;
  std::unique_ptr<SerializedOpaqueReturnTypeDeclTable> OpaqueReturnTypeDecls;
  std::unique_ptr<SerializedNestedTypeDeclsTable> NestedTypeDecls;
  std::unique_ptr<SerializedDeclMemberNamesTable> DeclMemberNames;
  std::unique_ptr<SerializedDeclFingerprintsTable> DeclFingerprints;

  class ObjCMethodTableInfo;
  using SerializedObjCMethodTable =
    llvm::OnDiskIterableChainedHashTable<ObjCMethodTableInfo>;

  std::unique_ptr<SerializedObjCMethodTable> ObjCMethods;

  ArrayRef<serialization::DeclID> OrderedTopLevelDecls;
  ArrayRef<serialization::DeclID> ExportedPrespecializationDecls;

  class DeclCommentTableInfo;
  using SerializedDeclCommentTable =
      llvm::OnDiskIterableChainedHashTable<DeclCommentTableInfo>;
  struct DeserializedCommentInfo;

  using GroupNameTable = const llvm::DenseMap<unsigned, StringRef>;

  std::unique_ptr<GroupNameTable> GroupNamesMap;
  std::unique_ptr<SerializedDeclCommentTable> DeclCommentTable;

  class DeclUSRTableInfo;
  using SerializedDeclUSRTable =
      llvm::OnDiskIterableChainedHashTable<DeclUSRTableInfo>;
  std::unique_ptr<SerializedDeclUSRTable> DeclUSRsTable;

  class DerivativeFunctionConfigTableInfo;
  using SerializedDerivativeFunctionConfigTable =
      llvm::OnDiskIterableChainedHashTable<DerivativeFunctionConfigTableInfo>;
  std::unique_ptr<SerializedDerivativeFunctionConfigTable>
      DerivativeFunctionConfigurations;

  /// A blob of 0 terminated string segments referenced in \c SourceLocsTextData
  StringRef SourceLocsTextData;

  /// A blob of source file list.
  StringRef SourceFileListData;

  /// An array of fixed size source location data for each USR appearing in
  /// \c DeclUSRsTable.
  StringRef BasicDeclLocsData;

  /// An array of fixed-size location data for each `SingleRawComment` piece
  /// of declaration's documentation `RawComment`s.
  StringRef DocRangesData;

  struct ModuleBits {
    /// The decl ID of the main class in this module file, if it has one.
    unsigned EntryPointDeclID : 31;

    /// Whether or not this module file comes from a context that had a main
    /// entry point.
    unsigned HasEntryPoint : 1;

    /// Whether this module file comes from a framework.
    unsigned IsFramework : 1;

    /// Whether an error has been detected setting up this module file.
    unsigned HasError : 1;

    /// Whether this module is `-enable-private-imports`.
    unsigned ArePrivateImportsEnabled : 1;

    /// Whether this module file is actually a .sib file.
    unsigned IsSIB : 1;

    /// Whether this module is compiled as static library.
    unsigned IsStaticLibrary : 1;

    /// Whether this module was built with -experimental-hermetic-seal-at-link.
    unsigned HasHermeticSealAtLink : 1;

    /// Whether this module file is compiled with '-enable-testing'.
    unsigned IsTestable : 1;

    /// Discriminator for resilience strategy.
    unsigned ResilienceStrategy : 2;

    /// Whether the module was rebuilt from a module interface instead of being
    /// build from the full source.
    unsigned IsBuiltFromInterface: 1;

    /// Whether this module is compiled with implicit dynamic.
    unsigned IsImplicitDynamicEnabled: 1;

    /// Whether this module is compiled while allowing errors.
    unsigned IsAllowModuleWithCompilerErrorsEnabled: 1;

    /// \c true if this module was built with complete checking for concurrency.
    unsigned IsConcurrencyChecked: 1;

    /// Whether this module is built with C++ interoperability enabled.
    unsigned HasCxxInteroperability : 1;

    // Explicitly pad out to the next word boundary.
    unsigned : 3;
  } Bits = {};
  static_assert(sizeof(ModuleBits) <= 8, "The bit set should be small");

  bool hasError() const {
    return Bits.HasError;
  }

  void setEntryPointClassID(serialization::DeclID DID) {
    Bits.HasEntryPoint = true;
    Bits.EntryPointDeclID = DID;
    assert(Bits.EntryPointDeclID == DID && "not enough bits for DeclID");
  }

  /// Constructs a new module and validates it.
  ModuleFileSharedCore(
      std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
      std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
      std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer,
      bool isFramework, bool requiresOSSAModules, StringRef requiredSDK,
      serialization::ValidationInfo &info, PathObfuscator &pathRecoverer);

  /// Change the status of the current module.
  Status error(Status issue) {
    assert(issue != Status::Valid);
    Bits.HasError = true;
    return issue;
  }

  /// Emits one last diagnostic, logs the error, and then aborts for the stack
  /// trace.
  [[noreturn]] void fatal(llvm::Error error) const;
  void fatalIfNotSuccess(llvm::Error error) const {
    if (error)
      fatal(std::move(error));
  }
  template <typename T> T fatalIfUnexpected(llvm::Expected<T> expected) const {
    if (expected)
      return std::move(expected.get());
    fatal(expected.takeError());
  }

  /// Read an on-disk decl hash table stored in index_block::DeclListLayout
  /// format.
  std::unique_ptr<SerializedDeclTable>
  readDeclTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local decl hash table stored in
  /// index_block::DeclListLayout format.
  std::unique_ptr<SerializedLocalDeclTable>
  readLocalDeclTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk Objective-C method table stored in
  /// index_block::ObjCMethodTableLayout format.
  std::unique_ptr<ModuleFileSharedCore::SerializedObjCMethodTable>
  readObjCMethodTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local decl hash table stored in
  /// index_block::ExtensionTableLayout format.
  std::unique_ptr<SerializedExtensionTable>
  readExtensionTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local decl hash table stored in
  /// index_block::NestedTypeDeclsLayout format.
  std::unique_ptr<SerializedNestedTypeDeclsTable>
  readNestedTypeDeclsTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local decl-name hash table stored in
  /// index_block::DeclMemberNamesLayout format.
  std::unique_ptr<SerializedDeclMemberNamesTable>
  readDeclMemberNamesTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local decl-members hash table stored in
  /// index_block::DeclMembersLayout format.
  std::unique_ptr<SerializedDeclMembersTable>
  readDeclMembersTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Read an on-disk local declid-string hash table stored in
  /// index_block::DeclFingerprintsLayout format.
  std::unique_ptr<SerializedDeclFingerprintsTable>
  readDeclFingerprintsTable(ArrayRef<uint64_t> fields,
                            StringRef blobData) const;

  /// Read an on-disk derivative function configuration table stored in
  /// index_block::DerivativeFunctionConfigTableLayout format.
  std::unique_ptr<ModuleFileSharedCore::SerializedDerivativeFunctionConfigTable>
  readDerivativeFunctionConfigTable(ArrayRef<uint64_t> fields,
                                    StringRef blobData) const;

  /// Reads the index block, which contains global tables.
  ///
  /// Returns false if there was an error.
  bool readIndexBlock(llvm::BitstreamCursor &cursor);

  /// Read an on-disk decl hash table stored in
  /// \c comment_block::DeclCommentListLayout format.
  std::unique_ptr<SerializedDeclCommentTable>
  readDeclCommentTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  std::unique_ptr<GroupNameTable>
  readGroupTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Reads the comment block, which contains USR to comment mappings.
  ///
  /// Returns false if there was an error.
  bool readCommentBlock(llvm::BitstreamCursor &cursor);

  /// Loads data from #ModuleDocInputBuffer.
  ///
  /// Returns false if there was an error.
  bool readModuleDocIfPresent(PathObfuscator &pathRecoverer);

  /// Reads the source loc block, which contains USR to decl location mapping.
  ///
  /// Returns false if there was an error.
  bool readDeclLocsBlock(llvm::BitstreamCursor &cursor);

  /// Loads data from #ModuleSourceInfoInputBuffer.
  ///
  /// Returns false if there was an error.
  bool readModuleSourceInfoIfPresent(PathObfuscator &pathRecoverer);

  /// Read an on-disk decl hash table stored in
  /// \c sourceinfo_block::DeclUSRSLayout format.
  std::unique_ptr<SerializedDeclUSRTable>
  readDeclUSRsTable(ArrayRef<uint64_t> fields, StringRef blobData) const;

  /// Returns the appropriate module name for the given ID.
  StringRef getModuleNameFromID(serialization::ModuleID MID) const;

  /// Convenience method to retrieve the text of the name with the given ID.
  /// Asserts that the name with this ID is not special.
  StringRef getIdentifierText(serialization::IdentifierID IID) const;

public:
  /// Loads a module from the given memory buffer.
  ///
  /// \param moduleInputBuffer A memory buffer containing the serialized module
  /// data. The created ModuleFile takes ownership of the buffer, even if
  /// there's an error in loading.
  /// \param moduleDocInputBuffer An optional memory buffer containing
  /// documentation data for the module. The created ModuleFile takes ownership
  /// of the buffer, even if there's an error in loading.
  /// \param isFramework If true, this is treated as a framework module for
  /// linking purposes.
  /// \param requiresOSSAModules If true, this requires dependent modules to be
  /// compiled with -enable-ossa-modules.
  /// \param[out] theModule The loaded module.
  /// \returns Whether the module was successfully loaded, or what went wrong
  ///          if it was not.
  static serialization::ValidationInfo
  load(StringRef moduleInterfacePath, StringRef moduleInterfaceSourcePath,
       std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
       std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
       std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer,
       bool isFramework, bool requiresOSSAModules, StringRef requiredSDK,
       PathObfuscator &pathRecoverer,
       std::shared_ptr<const ModuleFileSharedCore> &theModule) {
    serialization::ValidationInfo info;
    auto *core = new ModuleFileSharedCore(
        std::move(moduleInputBuffer), std::move(moduleDocInputBuffer),
        std::move(moduleSourceInfoInputBuffer), isFramework,
        requiresOSSAModules, requiredSDK, info, pathRecoverer);
    if (!moduleInterfacePath.empty()) {
      ArrayRef<char> path;
      core->allocateBuffer(path, moduleInterfacePath);
      core->ModuleInterfacePath = StringRef(path.data(), path.size());
    }
    if (!moduleInterfaceSourcePath.empty()) {
      ArrayRef<char> path;
      core->allocateBuffer(path, moduleInterfaceSourcePath);
      core->CorrespondingInterfacePath = StringRef(path.data(), path.size());
    }
    theModule.reset(core);
    return info;
  }

  /// Outputs information useful for diagnostics to \p out
  void outputDiagnosticInfo(llvm::raw_ostream &os) const;
  
  // Out of line to avoid instantiation OnDiskChainedHashTable here.
  ~ModuleFileSharedCore();

  /// The name of the module.
  StringRef getName() const {
    return Name;
  }

  StringRef getModulePackageName() const {
    return ModulePackageName;
  }

  /// Is the module built with testing enabled?
  bool isTestable() const {
     return Bits.IsTestable;
   }

  /// Whether the module is resilient. ('-enable-library-evolution')
  ResilienceStrategy getResilienceStrategy() const {
    return ResilienceStrategy(Bits.ResilienceStrategy);
  }

  /// Returns the list of modules this module depends on.
  ArrayRef<Dependency> getDependencies() const {
    return Dependencies;
  }

  /// Returns \c true if this module file contains a section with incremental
  /// information.
  bool hasIncrementalInfo() const { return HasIncrementalInfo; }

  /// Returns \c true if a corresponding .swiftsourceinfo has been found.
  bool hasSourceInfoFile() const { return !!ModuleSourceInfoInputBuffer; }

  /// Returns \c true if a corresponding .swiftsourceinfo has been found *and
  /// read*.
  bool hasSourceInfo() const;

  bool isConcurrencyChecked() const { return Bits.IsConcurrencyChecked; }

  /// How should \p dependency be loaded for a transitive import via \c this?
  ///
  /// If \p debuggerMode, more transitive dependencies should try to be loaded
  /// as they can be useful in debugging.
  ///
  /// If \p isPartialModule, transitive dependencies should be loaded as we're
  /// in merge-module mode.
  ///
  /// If \p packageName is set, transitive package dependencies are loaded if
  /// loaded from the same package.
  ///
  /// If \p forTestable, get the desired loading behavior for a @testable
  /// import. Reports non-public dependencies as required for a testable
  /// client so it can access internal details, which in turn can reference
  /// those non-public dependencies.
  ModuleLoadingBehavior
  getTransitiveLoadingBehavior(const Dependency &dependency,
                               bool debuggerMode,
                               bool isPartialModule,
                               StringRef packageName,
                               bool forTestable) const;
};

template <typename T, typename RawData>
void ModuleFileSharedCore::allocateBuffer(MutableArrayRef<T> &buffer,
                                    const RawData &rawData) {
  assert(buffer.empty() && "reallocating deserialized buffer");
  if (rawData.empty())
    return;

  void *rawBuffer = Allocator.Allocate(sizeof(T) * rawData.size(), alignof(T));
  buffer = llvm::makeMutableArrayRef(static_cast<T *>(rawBuffer),
                                     rawData.size());
  std::uninitialized_copy(rawData.begin(), rawData.end(), buffer.begin());
}

} // end namespace swift

#endif

//===--- FileUnit.h - The contents of a module ------------------*- C++ -*-===//
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

#ifndef SWIFT_AST_FILEUNIT_H
#define SWIFT_AST_FILEUNIT_H

#include "swift/AST/Module.h"
#include "swift/AST/RawComment.h"
#include "swift/Basic/BasicSourceInfo.h"
#include "swift/Basic/Debug.h"

#include "llvm/ADT/PointerIntPair.h"

namespace swift {
class SynthesizedFileUnit;

/// A container for module-scope declarations that itself provides a scope; the
/// smallest unit of code organization.
///
/// FileUnit is an abstract base class; its subclasses represent different
/// sorts of containers that can each provide a set of decls, e.g. a source
/// file. A module can contain several file-units.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
class FileUnit : public DeclContext, public ASTAllocated<FileUnit> {
#pragma clang diagnostic pop
  virtual void anchor();

  friend class DirectOperatorLookupRequest;
  friend class DirectPrecedenceGroupLookupRequest;

  // The pointer is FileUnit insted of SynthesizedFileUnit to break circularity.
  llvm::PointerIntPair<FileUnit *, 3, FileUnitKind> SynthesizedFileAndKind;

protected:
  FileUnit(FileUnitKind kind, ModuleDecl &M)
    : DeclContext(DeclContextKind::FileUnit, &M),
      SynthesizedFileAndKind(nullptr, kind) {
  }

public:
  FileUnitKind getKind() const {
    return SynthesizedFileAndKind.getInt();
  }

  /// Returns the synthesized file for this source file, if it exists.
  SynthesizedFileUnit *getSynthesizedFile() const;

  /// Returns the synthesized file for this source file, creating one and
  /// inserting it into the module if it does not exist.
  SynthesizedFileUnit &getOrCreateSynthesizedFile();

  /// Look up a (possibly overloaded) value set at top-level scope
  /// (but with the specified access path, which may come from an import decl)
  /// within this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           OptionSet<ModuleLookupFlags> Flags,
                           SmallVectorImpl<ValueDecl*> &result) const = 0;

  /// Look up a local type declaration by its mangled name.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  virtual TypeDecl *lookupLocalType(StringRef MangledName) const {
    return nullptr;
  }

  /// Look up an opaque return type by the mangled name of the declaration
  /// that defines it.
  virtual OpaqueTypeDecl *lookupOpaqueResultType(StringRef MangledName) {
    return nullptr;
  }

  /// Directly look for a nested type declared within this module inside the
  /// given nominal type (including any extensions).
  ///
  /// This is a fast-path hack to avoid circular dependencies in deserialization
  /// and the Clang importer.
  ///
  /// Private and fileprivate types should not be returned by this lookup.
  virtual TypeDecl *lookupNestedType(Identifier name,
                                     const NominalTypeDecl *parent) const {
    return nullptr;
  }

  /// Find ValueDecls in the module and pass them to the given consumer object.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  virtual void lookupVisibleDecls(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer,
                                  NLKind lookupKind) const {}

  /// Finds all class members defined in this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  virtual void lookupClassMembers(ImportPath::Access accessPath,
                                  VisibleDeclConsumer &consumer) const {}

  /// Finds class members defined in this file with the given name.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  virtual void lookupClassMember(ImportPath::Access accessPath,
                                 DeclName name,
                                 SmallVectorImpl<ValueDecl*> &results) const {}

  /// Find all Objective-C methods with the given selector.
  virtual void lookupObjCMethods(
                 ObjCSelector selector,
                 SmallVectorImpl<AbstractFunctionDecl *> &results) const = 0;

  /// Find all SPI names imported from \p importedModule by this module,
  /// collecting the identifiers in \p spiGroups.
  virtual void lookupImportedSPIGroups(
                            const ModuleDecl *importedModule,
                            SmallSetVector<Identifier, 4> &spiGroups) const {};

  /// Checks whether this file imports \c module as \c @_weakLinked.
  virtual bool importsModuleAsWeakLinked(const ModuleDecl *module) const {
    // For source files, this should be overridden to inspect the import
    // declarations in the file. Other kinds of file units, like serialized
    // modules, can just use this default implementation since the @_weakLinked
    // attribute is not transitive. If module C is imported @_weakLinked by
    // module B, that does not imply that module A imports module C @_weakLinked
    // if it imports module B.
    return false;
  }

  virtual llvm::Optional<Fingerprint>
  loadFingerprint(const IterableDeclContext *IDC) const {
    return llvm::None;
  }

protected:
  /// Look up an operator declaration. Do not call directly, use
  /// \c DirectOperatorLookupRequest instead.
  ///
  /// \param name The operator name ("+", ">>", etc.)
  ///
  /// \param fixity One of Prefix, Infix, or Postfix.
  virtual void
  lookupOperatorDirect(Identifier name, OperatorFixity fixity,
                       TinyPtrVector<OperatorDecl *> &results) const {}

  /// Look up a precedence group. Do not call directly, use
  /// \c DirectPrecedenceGroupLookupRequest instead.
  ///
  /// \param name The precedence group name.
  virtual void lookupPrecedenceGroupDirect(
      Identifier name, TinyPtrVector<PrecedenceGroupDecl *> &results) const {}

public:
  /// Returns the comment attached to the given declaration.
  ///
  /// This function is an implementation detail for comment serialization.
  /// If you just want to get a comment attached to a decl, use
  /// \c Decl::getRawComment() or \c Decl::getSemanticBriefComment().
  virtual llvm::Optional<CommentInfo> getCommentForDecl(const Decl *D) const {
    return llvm::None;
  }

  /// For a serialized AST file, returns \c true if an adjacent swiftdoc has been
  /// loaded. Otherwise, returns \c false.
  virtual bool hasLoadedSwiftDoc() const { return false; }

  virtual llvm::Optional<StringRef> getGroupNameForDecl(const Decl *D) const {
    return llvm::None;
  }

  virtual llvm::Optional<StringRef>
  getSourceFileNameForDecl(const Decl *D) const {
    return llvm::None;
  }

  virtual llvm::Optional<unsigned> getSourceOrderForDecl(const Decl *D) const {
    return llvm::None;
  }

  virtual llvm::Optional<StringRef> getGroupNameByUSR(StringRef USR) const {
    return llvm::None;
  }

  virtual llvm::Optional<ExternalSourceLocs::RawLocs>
  getExternalRawLocsForDecl(const Decl *D) const {
    return llvm::None;
  }

  virtual void collectAllGroups(SmallVectorImpl<StringRef> &Names) const {}

  /// Returns an implementation-defined "discriminator" for \p D, which
  /// distinguishes \p D from other declarations in the same module with the
  /// same name.
  ///
  /// Since this value is used in name mangling, it should be a valid ASCII-only
  /// identifier.
  virtual Identifier
  getDiscriminatorForPrivateDecl(const Decl *D) const = 0;

  virtual bool shouldCollectDisplayDecls() const { return true; }

  /// Finds all top-level decls in this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  virtual void getTopLevelDecls(SmallVectorImpl<Decl*> &results) const {}

  /// Finds all top-level decls in this file with their auxiliary decls such as
  /// macro expansions.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  void getTopLevelDeclsWithAuxiliaryDecls(
      SmallVectorImpl<Decl*> &results) const;

  virtual void
  getExportedPrespecializations(SmallVectorImpl<Decl *> &results) const {}

  /// Finds top-level decls in this file filtered by their attributes.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  ///
  /// \param Results Vector collecting the decls.
  ///
  /// \param matchAttributes Check on the attributes of a decl to keep only
  /// decls with matching attributes. The subclass SerializedASTFile checks the
  /// attributes first to only deserialize decls with accepted attributes,
  /// limiting deserialization work.
  virtual void
  getTopLevelDeclsWhereAttributesMatch(
              SmallVectorImpl<Decl*> &Results,
              llvm::function_ref<bool(DeclAttributes)> matchAttributes) const;

  /// Finds all operator decls in this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  virtual void
  getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) const {}

  /// Finds all precedence group decls in this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  virtual void
  getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl*> &Results) const {}

  /// Finds all local type decls in this file.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  /// The order of the results is not guaranteed to be meaningful.
  virtual void getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &results) const {}

  virtual void
  getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &results) const {}

  /// Adds all top-level decls to the given vector.
  ///
  /// This includes all decls that should be displayed to clients of the module.
  /// The order of the results is not guaranteed to be meaningful.
  ///
  /// This can differ from \c getTopLevelDecls, e.g. it returns decls from a
  /// shadowed clang module.
  virtual void getDisplayDecls(SmallVectorImpl<Decl*> &results, bool recursive = false) const {
    getTopLevelDecls(results);
  }

  /// Looks up which modules are imported by this file.
  ///
  /// \p filter controls whether public, private, or any imports are included
  /// in this list.
  virtual void
  getImportedModules(SmallVectorImpl<ImportedModule> &imports,
                     ModuleDecl::ImportFilter filter) const {}

  /// Lists modules that are not imported from this file and used in API.
  virtual void
  getMissingImportedModules(SmallVectorImpl<ImportedModule> &imports) const {}

  /// \see ModuleDecl::getImportedModulesForLookup
  virtual void getImportedModulesForLookup(
      SmallVectorImpl<ImportedModule> &imports) const {
    return getImportedModules(imports, ModuleDecl::ImportFilterKind::Exported);
  }

  /// Generates the list of libraries needed to link this file, based on its
  /// imports.
  virtual void
  collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const {}

  /// Load extra dependencies of this module to satisfy a testable import.
  virtual void loadDependenciesForTestable(SourceLoc diagLoc) const {}

  /// Returns the path of the file or directory that defines the module
  /// represented by this \c FileUnit, or empty string if there is none.
  /// Cross-import overlay specifiers are found relative to this path.
  virtual StringRef getModuleDefiningPath() const {
    return "";
  }

  /// True if this file contains the main class for the module.
  bool hasMainClass() const {
    return getMainClass();
  }
  ClassDecl *getMainClass() const {
    return dyn_cast_or_null<ClassDecl>(getMainDecl());
  }
  bool hasMainDecl() const { return getMainDecl(); }
  virtual ValueDecl *getMainDecl() const { return nullptr; }
  FuncDecl *getMainFunc() const {
    return dyn_cast_or_null<FuncDecl>(getMainDecl());
  }
  virtual bool hasEntryPoint() const {
    return false;
  }

  virtual ModuleDecl *getUnderlyingModuleIfOverlay() const {
    return nullptr;
  }

  /// Returns the associated clang module if one exists.
  virtual const clang::Module *getUnderlyingClangModule() const {
    return nullptr;
  }

  /// Returns the real name of the enclosing module to use when referencing entities in this file.
  /// The 'real name' is the actual binary name of the module, which can be different from the 'name'
  /// if module aliasing was used (via -module-alias flag).
  ///
  /// This is usually the module real name which can be overriden by an
  /// `export_as` definition of a clang module, or `-export-as` flag on an
  /// imported Swift module. Swift modules built from source do not apply
  /// their own `-export-as` flag, this way the swiftinterface can be
  /// verified.
  virtual StringRef getExportedModuleName() const {
    return getParentModule()->getRealName().str();
  }

  SWIFT_DEBUG_DUMPER(dumpDisplayDecls());
  SWIFT_DEBUG_DUMPER(dumpTopLevelDecls());

  /// Traverse the decls within this file.
  ///
  /// \returns true if traversal was aborted, false if it completed
  /// successfully.
  virtual bool walk(ASTWalker &walker);

  // Efficiency override for DeclContext::getParentModule().
  ModuleDecl *getParentModule() const {
    return const_cast<ModuleDecl *>(cast<ModuleDecl>(getParent()));
  }

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::FileUnit;
  }

  using ASTAllocated<FileUnit>::operator new;
  using ASTAllocated<FileUnit>::operator delete;
};

/// This represents the compiler's implicitly generated declarations in the
/// Builtin module.
class BuiltinUnit final : public FileUnit {
public:
  class LookupCache;

private:
  std::unique_ptr<LookupCache> Cache;
  LookupCache &getCache() const;

  friend ASTContext;
  ~BuiltinUnit() = default;

public:
  explicit BuiltinUnit(ModuleDecl &M);

  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           OptionSet<ModuleLookupFlags> Flags,
                           SmallVectorImpl<ValueDecl*> &result) const override;

  /// Find all Objective-C methods with the given selector.
  void lookupObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const override;

  Identifier
  getDiscriminatorForPrivateDecl(const Decl *D) const override {
    llvm_unreachable("no private values in the Builtin module");
  }

  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::Builtin;
  }

  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }
};

/// Represents an externally-loaded file of some kind.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
class LoadedFile : public FileUnit {
#pragma clang diagnostic pop
protected:
  LoadedFile(FileUnitKind Kind, ModuleDecl &M) noexcept
    : FileUnit(Kind, M) {
    assert(classof(this) && "invalid kind");
  }
public:
  /// Returns an arbitrary string representing the storage backing this file.
  ///
  /// This is usually a filesystem path.
  virtual StringRef getFilename() const;

  /// Get the path to the file loaded by the compiler. Usually the binary
  /// swiftmodule file or a pcm in the cache. Returns an empty string if not
  /// applicable.
  virtual StringRef getLoadedFilename() const { return StringRef(); }

  /// Returns the path of the file for the module represented by this
  /// \c FileUnit, or an empty string if there is none. For modules either
  /// built by or adjacent to a module interface, returns the module
  /// interface instead.
  virtual StringRef getSourceFilename() const {
    return getModuleDefiningPath();
  }

  virtual StringRef getFilenameForPrivateDecl(const Decl *decl) const {
    return StringRef();
  }

  /// Returns the Swift module that overlays a Clang module.
  virtual ModuleDecl *getOverlayModule() const { return nullptr; }

  virtual bool isSystemModule() const { return false; }

  /// Checks whether an error was encountered while loading the file.
  virtual bool hadLoadError() const { return false; }

  /// Retrieve the set of generic signatures stored within this module.
  ///
  /// \returns \c true if this module file supports retrieving all of the
  /// generic signatures, \c false otherwise.
  virtual bool getAllGenericSignatures(
                 SmallVectorImpl<GenericSignature> &genericSignatures) {
    return false;
  }

  virtual void collectBasicSourceFileInfo(
      llvm::function_ref<void(const BasicSourceFileInfo &)> callback) const {}

  virtual void collectSerializedSearchPath(
      llvm::function_ref<void(StringRef)> callback) const {}
  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::SerializedAST ||
           file->getKind() == FileUnitKind::ClangModule ||
           file->getKind() == FileUnitKind::DWARFModule;
  }
  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }
};

void simple_display(llvm::raw_ostream &out, const FileUnit *file);

inline FileUnit &ModuleDecl::getMainFile(FileUnitKind expectedKind) const {
  assert(expectedKind != FileUnitKind::Source &&
         "must use specific source kind; see getMainSourceFile");
  assert(!Files.empty() && "No files added yet");
  assert(Files.front()->getKind() == expectedKind);
  return *Files.front();
}

} // end namespace swift

#endif

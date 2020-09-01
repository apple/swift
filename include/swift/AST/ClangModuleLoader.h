//===--- ClangModuleLoader.h - Clang Module Loader Interface ----*- C++ -*-===//
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

#ifndef SWIFT_AST_CLANG_MODULE_LOADER_H
#define SWIFT_AST_CLANG_MODULE_LOADER_H

#include "clang/Basic/SourceLocation.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/Basic/TaggedUnion.h"

namespace clang {
class ASTContext;
class CompilerInstance;
class Decl;
class Module;
class Preprocessor;
class Sema;
class TargetInfo;
class Type;
} // namespace clang

namespace swift {

class Decl;
class DeclContext;
class VisibleDeclConsumer;

/// Represents the different namespaces for types in C.
///
/// A simplified version of clang::Sema::LookupKind.
enum class ClangTypeKind {
  Typedef,
  ObjCClass = Typedef,
  /// Structs, enums, and unions.
  Tag,
  ObjCProtocol,
};

/// A path for serializing a declaration.
class StableSerializationPath {
public:
  struct ExternalPath {
    enum ComponentKind {
      /// A named record type (but not a template specialization)
      Record,

      /// A named enum type
      Enum,

      /// A C++ namespace
      Namespace,

      /// A typedef
      Typedef,

      /// A typedef's anonymous tag declaration.  Identifier is empty.
      TypedefAnonDecl,

      /// An Objective-C interface.
      ObjCInterface,

      /// An Objective-C protocol.
      ObjCProtocol,
    };

    static bool requiresIdentifier(ComponentKind kind) {
      return kind != TypedefAnonDecl;
    }

    SmallVector<std::pair<ComponentKind, Identifier>, 2> Path;

    void add(ComponentKind kind, Identifier name) {
      Path.push_back({kind, name});
    }
  };
private:
  TaggedUnion<void, const Decl *, ExternalPath> Union;

public:
  StableSerializationPath() {}
  StableSerializationPath(const Decl *d) : Union(d) {}
  StableSerializationPath(ExternalPath ext) : Union(ext) {}

  explicit operator bool() const { return !Union.empty(); }

  bool isSwiftDecl() const { return Union.isa<const Decl*>(); }
  const Decl *getSwiftDecl() const {
    assert(isSwiftDecl());
    return Union.get<const Decl*>();
  }

  bool isExternalPath() const { return Union.isa<ExternalPath>(); }
  const ExternalPath &getExternalPath() const {
    assert(isExternalPath());
    return Union.get<ExternalPath>();
  }

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os) const;
};

class ClangModuleLoader : public ModuleLoader {
private:
  virtual void anchor() override;

protected:
  using ModuleLoader::ModuleLoader;

public:
  virtual clang::TargetInfo &getTargetInfo() const = 0;
  virtual clang::ASTContext &getClangASTContext() const = 0;
  virtual clang::Preprocessor &getClangPreprocessor() const = 0;
  virtual clang::Sema &getClangSema() const = 0;
  virtual const clang::CompilerInstance &getClangInstance() const = 0;
  virtual void printStatistics() const = 0;

  /// Returns the module that contains imports and declarations from all loaded
  /// Objective-C header files.
  virtual ModuleDecl *getImportedHeaderModule() const = 0;

  /// Retrieves the Swift wrapper for the given Clang module, creating
  /// it if necessary.
  virtual ModuleDecl *
  getWrapperForModule(const clang::Module *mod,
                      bool returnOverlayIfPossible = false) const = 0;

  /// Adds a new search path to the Clang CompilerInstance, as if specified with
  /// -I or -F.
  ///
  /// \returns true if there was an error adding the search path.
  virtual bool addSearchPath(StringRef newSearchPath, bool isFramework,
                             bool isSystem) = 0;

  /// Determine whether \c overlayDC is within an overlay module for the
  /// imported context enclosing \c importedDC.
  ///
  /// This routine is used for various hacks that are only permitted within
  /// overlays of imported modules, e.g., Objective-C bridging conformances.
  virtual bool
  isInOverlayModuleForImportedModule(const DeclContext *overlayDC,
                                     const DeclContext *importedDC) = 0;

  /// Look for declarations associated with the given name.
  ///
  /// \param name The name we're searching for.
  virtual void lookupValue(DeclName name, VisibleDeclConsumer &consumer) = 0;

  /// Look up a type declaration by its Clang name.
  ///
  /// Note that this method does no filtering. If it finds the type in a loaded
  /// module, it returns it. This is intended for use in reflection / debugging
  /// contexts where access is not a problem.
  virtual void
  lookupTypeDecl(StringRef clangName, ClangTypeKind kind,
                 llvm::function_ref<void(TypeDecl *)> receiver) = 0;

  /// Look up type a declaration synthesized by the Clang importer itself, using
  /// a "related entity kind" to determine which type it should be. For example,
  /// this can be used to find the synthesized error struct for an
  /// NS_ERROR_ENUM.
  ///
  /// Note that this method does no filtering. If it finds the type in a loaded
  /// module, it returns it. This is intended for use in reflection / debugging
  /// contexts where access is not a problem.
  virtual void
  lookupRelatedEntity(StringRef clangName, ClangTypeKind kind,
                      StringRef relatedEntityKind,
                      llvm::function_ref<void(TypeDecl *)> receiver) = 0;

  /// Try to parse the string as a Clang function type.
  ///
  /// Returns null if there was a parsing failure.
  virtual const clang::Type *parseClangFunctionType(StringRef type,
                                                    SourceLoc loc) const = 0;

  /// Print the Clang type.
  virtual void printClangType(const clang::Type *type,
                              llvm::raw_ostream &os) const = 0;

  /// Return a Swift source location that corresponds to the given Clang
  /// source location.
  virtual SourceLoc
  resolveSourceLocation(clang::SourceLocation clangLoc) const = 0;

  /// Try to find a stable serialization path for the given declaration,
  /// if there is one.
  virtual StableSerializationPath
  findStableSerializationPath(const clang::Decl *decl) const = 0;

  /// Try to resolve a stable serialization path down to the original
  /// declaration.
  virtual const clang::Decl *
  resolveStableSerializationPath(const StableSerializationPath &path) const = 0;

  /// Determine whether the given type is serializable.
  ///
  /// If \c checkCanonical is true, checks the canonical type,
  /// not the given possibly-sugared type.  In general:
  ///  - non-canonical representations should be preserving the
  ///    sugared type even if it isn't serializable, since that
  ///    maintains greater source fidelity;
  ///  - semantic checks need to be checking the serializability
  ///    of the canonical type, since it's always potentially
  ///    necessary to serialize that (e.g. in SIL); and
  ///  - serializers can try to serialize the sugared type to
  ///    maintain source fidelity and just fall back on the canonical
  ///    type if that's not possible.
  ///
  /// The expectation here is that this predicate is meaningful
  /// independent of the actual form of serialization: the types
  /// that we can't reliably binary-serialize without an absolute
  /// Clang AST cross-reference are the same types that won't
  /// reliably round-trip through a textual format.  At the very
  /// least, it's probably best to use conservative predicates
  /// that work both ways so that language behavior doesn't differ
  /// based on subtleties like the target module interface format.
  virtual bool isSerializable(const clang::Type *type,
                              bool checkCanonical) const = 0;
};

} // namespace swift

#endif // LLVM_SWIFT_AST_CLANG_MODULE_LOADER_H

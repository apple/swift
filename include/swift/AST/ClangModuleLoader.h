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

#include "swift/AST/ModuleLoader.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/TaggedUnion.h"
#include "clang/AST/DeclTemplate.h"

namespace clang {
class ASTContext;
class CompilerInstance;
class Decl;
class Module;
class Preprocessor;
class Sema;
class TargetInfo;
class Type;
class SourceLocation;
} // namespace clang

namespace swift {

class ConcreteDeclRef;
class Decl;
class FuncDecl;
class VarDecl;
class DeclContext;
class EffectiveClangContext;
class SwiftLookupTable;
class ValueDecl;
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
  /// This module loader's Clang instance may be configured with a different
  /// (higher) OS version than the compilation target itself in order to be able
  /// to load pre-compiled Clang modules that are aligned with the broader SDK,
  /// and match the SDK deployment target against which Swift modules are also
  /// built.
  ///
  /// In this case, we must use the Swift compiler's OS version triple when
  /// performing codegen, and the importer's Clang instance OS version triple
  /// during module loading. `getModuleAvailabilityTarget` is for module-loading
  /// clients only, and uses the latter.
  ///
  /// (The implementing `ClangImporter` class maintains separate Target info
  /// for use by IRGen/CodeGen clients)
  virtual clang::TargetInfo &getModuleAvailabilityTarget() const = 0;

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

  /// Imports a clang decl directly, rather than looking up its name.
  virtual Decl *importDeclDirectly(const clang::NamedDecl *decl) = 0;

  /// Imports a clang decl from a base class, cloning it for \param newContext
  /// if it wasn't cloned for this specific context before.
  virtual ValueDecl *importBaseMemberDecl(ValueDecl *decl,
                                          DeclContext *newContext) = 0;

  /// Emits diagnostics for any declarations named name
  /// whose direct declaration context is a TU.
  virtual void diagnoseTopLevelValue(const DeclName &name) = 0;

  /// Emit diagnostics for declarations named name that are members
  /// of the provided baseType.
  virtual void diagnoseMemberValue(const DeclName &name,
                                   const Type &baseType) = 0;

  /// Instantiate and import class template using given arguments.
  ///
  /// This method will find the clang::ClassTemplateSpecialization decl if
  /// it already exists, or it will create one. Then it will import this
  /// decl the same way as we import typedeffed class templates - using
  /// the hidden struct prefixed with `__CxxTemplateInst`.
  virtual StructDecl *
  instantiateCXXClassTemplate(clang::ClassTemplateDecl *decl,
                      ArrayRef<clang::TemplateArgument> arguments) = 0;

  virtual ConcreteDeclRef
  getCXXFunctionTemplateSpecialization(SubstitutionMap subst,
                                       ValueDecl *decl) = 0;

  /// Try to parse the string as a Clang function type.
  ///
  /// Returns null if there was a parsing failure.
  virtual const clang::Type *parseClangFunctionType(StringRef type,
                                                    SourceLoc loc) const = 0;

  /// Print the Clang type.
  virtual void printClangType(const clang::Type *type,
                              llvm::raw_ostream &os) const = 0;

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

  virtual clang::FunctionDecl *
  instantiateCXXFunctionTemplate(ASTContext &ctx,
                                 clang::FunctionTemplateDecl *func,
                                 SubstitutionMap subst) = 0;

  virtual bool isCXXMethodMutating(const clang::CXXMethodDecl *method) = 0;

  virtual bool isUnsafeCXXMethod(const FuncDecl *func) = 0;

  virtual llvm::Optional<Type>
  importFunctionReturnType(const clang::FunctionDecl *clangDecl,
                           DeclContext *dc) = 0;

  virtual Type importVarDeclType(const clang::VarDecl *clangDecl,
                                 VarDecl *swiftDecl,
                                 DeclContext *dc) = 0;

  /// Find the lookup table that corresponds to the given Clang module.
  ///
  /// \param clangModule The module, or null to indicate that we're talking
  /// about the directly-parsed headers.
  virtual SwiftLookupTable *
  findLookupTable(const clang::Module *clangModule) = 0;

  virtual DeclName
  importName(const clang::NamedDecl *D,
             clang::DeclarationName givenName = clang::DeclarationName()) = 0;

  /// Determine the effective Clang context for the given Swift nominal type.
  virtual EffectiveClangContext getEffectiveClangContext(
      const NominalTypeDecl *nominal) = 0;

  virtual const clang::TypedefType *
  getTypeDefForCXXCFOptionsDefinition(const clang::Decl *candidateDecl) = 0;

  virtual SourceLoc importSourceLocation(clang::SourceLocation loc) = 0;
};

/// Describes a C++ template instantiation error.
struct TemplateInstantiationError {
  /// Generic types that could not be converted to QualTypes using the
  /// ClangTypeConverter.
  SmallVector<Type, 4> failedTypes;
};

} // namespace swift

#endif // LLVM_SWIFT_AST_CLANG_MODULE_LOADER_H

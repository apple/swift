//===--- NameLookupRequests.h - Name Lookup Requests ------------*- C++ -*-===//
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
//  This file defines name-lookup requests.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_NAME_LOOKUP_REQUESTS_H
#define SWIFT_NAME_LOOKUP_REQUESTS_H

#include "swift/AST/SimpleRequest.h"
#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/EvaluatorDependencies.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/TypeOrExtensionDecl.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class ClassDecl;
class DeclContext;
class DeclName;
class DeclNameRef;
class DestructorDecl;
class GenericContext;
class GenericParamList;
class LookupResult;
enum class NLKind;
class PotentialMacroExpansions;
class SourceLoc;
class TypeAliasDecl;
class TypeDecl;
enum class UnqualifiedLookupFlags;
namespace ast_scope {
class ASTScopeImpl;
class ScopeCreator;
} // namespace ast_scope
namespace namelookup {
enum class ResolutionKind;
} // namespace namelookup

/// Display a nominal type or extension thereof.
void simple_display(
    llvm::raw_ostream &out,
    const llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> &value);

/// Describes a set of type declarations that are "direct" referenced by
/// a particular type in the AST.
using DirectlyReferencedTypeDecls = llvm::TinyPtrVector<TypeDecl *>;

/// Request the set of declarations directly referenced by the an "inherited"
/// type of a type or extension declaration.
///
/// This request retrieves the set of declarations that are directly referenced
/// by a particular type in the "inherited" clause of a type or extension
/// declaration. For example:
///
/// \code
///   protocol P { }
///   protocol Q { }
///   typealias Alias = P & Q
///   class C { }
///
///   class D: C, Alias { }
/// \endcode
///
/// The inherited declaration of \c D at index 0 is the class declaration C.
/// The inherited declaration of \c D at index 1 is the typealias Alias.
class InheritedDeclsReferencedRequest
    : public SimpleRequest<
          InheritedDeclsReferencedRequest,
          DirectlyReferencedTypeDecls(
              llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *>,
              unsigned),
          RequestFlags::Uncached> // FIXME: Cache these
{
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  DirectlyReferencedTypeDecls
  evaluate(Evaluator &evaluator,
           llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
           unsigned index) const;

public:
  // Caching
  bool isCached() const { return true; }

  // Source location information.
  SourceLoc getNearestLoc() const;
};

/// Request the set of declarations directly referenced by the underlying
/// type of a typealias.
///
/// This request retrieves the set of type declarations that directly referenced
/// by the underlying type of a typealias. For example:
///
/// \code
///   protocol P { }
///   protocol Q { }
///   class C { }
///   typealias PQ = P & Q
///   typealias Alias = C & PQ
/// \endcode
///
/// The set of declarations referenced by the underlying type of \c PQ
/// contains both \c P and \c Q.
/// The set of declarations referenced by the underlying type of \c Alias
/// contains \c C and \c PQ. Clients can choose to look further into \c PQ
/// using another instance of this request.
class UnderlyingTypeDeclsReferencedRequest :
  public SimpleRequest<UnderlyingTypeDeclsReferencedRequest,
                       DirectlyReferencedTypeDecls(TypeAliasDecl *),
                       RequestFlags::Uncached> // FIXME: Cache these
{
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  DirectlyReferencedTypeDecls evaluate(
      Evaluator &evaluator,
      TypeAliasDecl *typealias) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the superclass declaration for the given class.
class SuperclassDeclRequest :
    public SimpleRequest<SuperclassDeclRequest,
                         ClassDecl *(NominalTypeDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ClassDecl *
  evaluate(Evaluator &evaluator, NominalTypeDecl *subject) const;

public:
  // Cycle handling
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Caching
  bool isCached() const { return true; }
  Optional<ClassDecl *> getCachedResult() const;
  void cacheResult(ClassDecl *value) const;
};

class InheritedProtocolsRequest
    : public SimpleRequest<
          InheritedProtocolsRequest, ArrayRef<ProtocolDecl *>(ProtocolDecl *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<ProtocolDecl *>
  evaluate(Evaluator &evaluator, ProtocolDecl *PD) const;

public:
  // Caching
  bool isCached() const { return true; }
  Optional<ArrayRef<ProtocolDecl *>> getCachedResult() const;
  void cacheResult(ArrayRef<ProtocolDecl *> value) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           ArrayRef<ProtocolDecl *> result) const;
};

class ProtocolRequirementsRequest
    : public SimpleRequest<ProtocolRequirementsRequest,
                           ArrayRef<ValueDecl *>(ProtocolDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<ValueDecl *> evaluate(Evaluator &, ProtocolDecl *) const;

public:
  // Caching
  bool isCached() const { return true; }
  Optional<ArrayRef<ValueDecl *>> getCachedResult() const;
  void cacheResult(ArrayRef<ValueDecl *> value) const;
};

/// Requests whether or not this class has designated initializers that are
/// not public or @usableFromInline.
class HasMissingDesignatedInitializersRequest :
    public SimpleRequest<HasMissingDesignatedInitializersRequest,
                         bool(ClassDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, ClassDecl *subject) const;

public:
  // Caching
  bool isCached() const { return true; }
  Optional<bool> getCachedResult() const;
  void cacheResult(bool) const;
};

/// Request the nominal declaration extended by a given extension declaration.
class ExtendedNominalRequest
    : public SimpleRequest<
          ExtendedNominalRequest, NominalTypeDecl *(ExtensionDecl *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  NominalTypeDecl *
  evaluate(Evaluator &evaluator, ExtensionDecl *ext) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  Optional<NominalTypeDecl *> getCachedResult() const;
  void cacheResult(NominalTypeDecl *value) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           NominalTypeDecl *result) const;
};

struct SelfBounds {
  llvm::TinyPtrVector<NominalTypeDecl *> decls;
  bool anyObject = false;
};

/// Request the nominal types that occur as the right-hand side of "Self: Foo"
/// constraints in the "where" clause of a protocol extension.
class SelfBoundsFromWhereClauseRequest
    : public SimpleRequest<SelfBoundsFromWhereClauseRequest,
                           SelfBounds(llvm::PointerUnion<
                                      const TypeDecl *, const ExtensionDecl *>),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  SelfBounds
  evaluate(Evaluator &evaluator,
           llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *>) const;
};

/// Request the nominal types that occur as the right-hand side of "Self: Foo"
/// constraints in the generic signature of a protocol extension.
class SelfBoundsFromGenericSignatureRequest
    : public SimpleRequest<SelfBoundsFromGenericSignatureRequest,
                           SelfBounds(const ExtensionDecl *),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  SelfBounds evaluate(Evaluator &evaluator, const ExtensionDecl *extDecl) const;
};

/// Request all type aliases and nominal types that appear in the "where"
/// clause of an extension.
class TypeDeclsFromWhereClauseRequest :
    public SimpleRequest<TypeDeclsFromWhereClauseRequest,
                         DirectlyReferencedTypeDecls(ExtensionDecl *),
                         RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  DirectlyReferencedTypeDecls evaluate(Evaluator &evaluator,
                                       ExtensionDecl *ext) const;
};

/// Request the nominal type declaration to which the given custom
/// attribute refers.
class CustomAttrNominalRequest :
    public SimpleRequest<CustomAttrNominalRequest,
                         NominalTypeDecl *(CustomAttr *, DeclContext *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  NominalTypeDecl *
  evaluate(Evaluator &evaluator, CustomAttr *attr, DeclContext *dc) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Finds or synthesizes a destructor for the given class.
class GetDestructorRequest
    : public SimpleRequest<GetDestructorRequest, DestructorDecl *(ClassDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  DestructorDecl *
  evaluate(Evaluator &evaluator, ClassDecl *classDecl) const;

public:
  // Caching
  bool isCached() const { return true; }
  Optional<DestructorDecl *> getCachedResult() const;
  void cacheResult(DestructorDecl *value) const;
};

class GenericParamListRequest :
    public SimpleRequest<GenericParamListRequest,
                         GenericParamList *(GenericContext *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;
  
private:
  friend SimpleRequest;
  
  // Evaluation.
  GenericParamList *
  evaluate(Evaluator &evaluator, GenericContext *value) const;
  
public:
  // Separate caching.
  bool isCached() const { return true; }
  Optional<GenericParamList *> getCachedResult() const;
  void cacheResult(GenericParamList *value) const;
};

/// The input type for an unqualified lookup request.
class UnqualifiedLookupDescriptor {
  using LookupOptions = OptionSet<UnqualifiedLookupFlags>;

public:
  DeclNameRef Name;
  DeclContext *DC;
  SourceLoc Loc;
  LookupOptions Options;

  UnqualifiedLookupDescriptor(DeclNameRef name, DeclContext *dc,
                              SourceLoc loc = SourceLoc(),
                              LookupOptions options = {})
      : Name(name), DC(dc), Loc(loc), Options(options) { }

  friend llvm::hash_code hash_value(const UnqualifiedLookupDescriptor &desc) {
    return llvm::hash_combine(desc.Name, desc.DC, desc.Loc,
                              desc.Options.toRaw());
  }

  friend bool operator==(const UnqualifiedLookupDescriptor &lhs,
                         const UnqualifiedLookupDescriptor &rhs) {
    return lhs.Name == rhs.Name && lhs.DC == rhs.DC && lhs.Loc == rhs.Loc &&
           lhs.Options.toRaw() == rhs.Options.toRaw();
  }

  friend bool operator!=(const UnqualifiedLookupDescriptor &lhs,
                         const UnqualifiedLookupDescriptor &rhs) {
    return !(lhs == rhs);
  }
};

void simple_display(llvm::raw_ostream &out,
                    const UnqualifiedLookupDescriptor &desc);

SourceLoc extractNearestSourceLoc(const UnqualifiedLookupDescriptor &desc);

/// Performs unqualified lookup for a DeclName from a given context.
class UnqualifiedLookupRequest
    : public SimpleRequest<UnqualifiedLookupRequest,
                           LookupResult(UnqualifiedLookupDescriptor),
                           RequestFlags::Uncached |
                               RequestFlags::DependencySink> {
public:
  UnqualifiedLookupRequest(UnqualifiedLookupDescriptor);

private:
  friend SimpleRequest;

  // Evaluation.
  LookupResult evaluate(Evaluator &evaluator,
                        UnqualifiedLookupDescriptor desc) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const LookupResult &res) const;
};

using QualifiedLookupResult = SmallVector<ValueDecl *, 4>;

/// Performs a lookup into a given module and its imports.
class LookupInModuleRequest
    : public SimpleRequest<
          LookupInModuleRequest,
          QualifiedLookupResult(const DeclContext *, DeclName, NLKind,
                                namelookup::ResolutionKind, const DeclContext *,
                                NLOptions),
          RequestFlags::Uncached | RequestFlags::DependencySink> {
public:
  LookupInModuleRequest(
      const DeclContext *, DeclName, NLKind,
      namelookup::ResolutionKind, const DeclContext *,
      SourceLoc, NLOptions);

private:
  friend SimpleRequest;

  // Evaluation.
  QualifiedLookupResult
  evaluate(Evaluator &evaluator, const DeclContext *moduleOrFile, DeclName name,
           NLKind lookupKind, namelookup::ResolutionKind resolutionKind,
           const DeclContext *moduleScopeContext, NLOptions options) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const QualifiedLookupResult &l) const;
};

/// Perform \c AnyObject lookup for a given member.
class AnyObjectLookupRequest
    : public SimpleRequest<AnyObjectLookupRequest,
                           QualifiedLookupResult(const DeclContext *,
                                                 DeclNameRef, NLOptions),
                           RequestFlags::Uncached | RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  QualifiedLookupResult evaluate(Evaluator &evaluator,
                                 const DeclContext *dc,
                                 DeclNameRef name,
                                 NLOptions options) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const QualifiedLookupResult &l) const;
};

class ModuleQualifiedLookupRequest
    : public SimpleRequest<ModuleQualifiedLookupRequest,
                           QualifiedLookupResult(const DeclContext *,
                                                 ModuleDecl *, DeclNameRef,
                                                 NLOptions),
                           RequestFlags::Uncached |
                              RequestFlags::DependencySink> {
public:
  ModuleQualifiedLookupRequest(const DeclContext *,
                               ModuleDecl *, DeclNameRef,
                               SourceLoc, NLOptions);

private:
  friend SimpleRequest;

  // Evaluation.
  QualifiedLookupResult evaluate(Evaluator &evaluator,
                                 const DeclContext *DC,
                                 ModuleDecl *mod, DeclNameRef name,
                                 NLOptions opts) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const QualifiedLookupResult &lookupResult) const;
};

class QualifiedLookupRequest
    : public SimpleRequest<QualifiedLookupRequest,
                           QualifiedLookupResult(const DeclContext *,
                                                 SmallVector<NominalTypeDecl *, 4>,
                                                 DeclNameRef, NLOptions),
                           RequestFlags::Uncached> {
public:
  QualifiedLookupRequest(const DeclContext *,
                         SmallVector<NominalTypeDecl *, 4>,
                         DeclNameRef, SourceLoc, NLOptions);

private:
  friend SimpleRequest;

  // Evaluation.
  QualifiedLookupResult
  evaluate(Evaluator &evaluator, const DeclContext *DC,
           SmallVector<NominalTypeDecl *, 4> decls,
           DeclNameRef name,
           NLOptions opts) const;
};

/// The input type for a direct lookup request.
class DirectLookupDescriptor final {
  using LookupOptions = OptionSet<NominalTypeDecl::LookupDirectFlags>;

public:
  NominalTypeDecl *DC;
  DeclName Name;
  LookupOptions Options;

  DirectLookupDescriptor(NominalTypeDecl *dc, DeclName name,
                         LookupOptions options = {})
      : DC(dc), Name(name), Options(options) {}

  friend llvm::hash_code hash_value(const DirectLookupDescriptor &desc) {
    return llvm::hash_combine(desc.Name, desc.DC, desc.Options.toRaw());
  }

  friend bool operator==(const DirectLookupDescriptor &lhs,
                         const DirectLookupDescriptor &rhs) {
    return lhs.Name == rhs.Name && lhs.DC == rhs.DC &&
           lhs.Options.toRaw() == rhs.Options.toRaw();
  }

  friend bool operator!=(const DirectLookupDescriptor &lhs,
                         const DirectLookupDescriptor &rhs) {
    return !(lhs == rhs);
  }
};

void simple_display(llvm::raw_ostream &out, const DirectLookupDescriptor &desc);

SourceLoc extractNearestSourceLoc(const DirectLookupDescriptor &desc);

class DirectLookupRequest
    : public SimpleRequest<DirectLookupRequest,
                           TinyPtrVector<ValueDecl *>(DirectLookupDescriptor),
                           RequestFlags::Uncached|RequestFlags::DependencySink> {
public:
  DirectLookupRequest(DirectLookupDescriptor, SourceLoc);

private:
  friend SimpleRequest;

  // Evaluation.
  TinyPtrVector<ValueDecl *>
  evaluate(Evaluator &evaluator, DirectLookupDescriptor desc) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const TinyPtrVector<ValueDecl *> &result) const;
};

class OperatorLookupDescriptor final {
public:
  using Storage = llvm::PointerUnion<FileUnit *, ModuleDecl *>;
  Storage fileOrModule;
  Identifier name;

private:
  OperatorLookupDescriptor(Storage fileOrModule, Identifier name)
      : fileOrModule(fileOrModule), name(name) {}

public:
  /// Retrieves the files to perform lookup in.
  ArrayRef<FileUnit *> getFiles() const;

  /// If this is for a module lookup, returns the module. Otherwise returns
  /// \c nullptr.
  ModuleDecl *getModule() const {
    return fileOrModule.dyn_cast<ModuleDecl *>();
  }

  /// Retrieve the file or module for the lookup, as a DeclContext.
  DeclContext *getDC() const {
    if (auto *module = getModule())
      return module;
    return fileOrModule.get<FileUnit *>();
  }

  friend llvm::hash_code hash_value(const OperatorLookupDescriptor &desc) {
    return llvm::hash_combine(desc.fileOrModule, desc.name);
  }

  friend bool operator==(const OperatorLookupDescriptor &lhs,
                         const OperatorLookupDescriptor &rhs) {
    return lhs.fileOrModule == rhs.fileOrModule && lhs.name == rhs.name;
  }

  friend bool operator!=(const OperatorLookupDescriptor &lhs,
                         const OperatorLookupDescriptor &rhs) {
    return !(lhs == rhs);
  }

  static OperatorLookupDescriptor forFile(FileUnit *file, Identifier name) {
    return OperatorLookupDescriptor(file, name);
  }

  static OperatorLookupDescriptor forModule(ModuleDecl *mod, Identifier name) {
    return OperatorLookupDescriptor(mod, name);
  }

  static OperatorLookupDescriptor forDC(const DeclContext *DC, Identifier name);
};

void simple_display(llvm::raw_ostream &out,
                    const OperatorLookupDescriptor &desc);

SourceLoc extractNearestSourceLoc(const OperatorLookupDescriptor &desc);

/// Looks up an operator in a given file or module without looking through
/// imports.
class DirectOperatorLookupRequest
    : public SimpleRequest<DirectOperatorLookupRequest,
                           TinyPtrVector<OperatorDecl *>(
                               OperatorLookupDescriptor, OperatorFixity),
                           RequestFlags::Uncached |
                               RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  TinyPtrVector<OperatorDecl *>
  evaluate(Evaluator &evaluator, OperatorLookupDescriptor descriptor,
           OperatorFixity fixity) const;

public:
  // Incremental dependencies.
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const TinyPtrVector<OperatorDecl *> &ops) const;
};

/// Looks up an precedencegroup in a given file or module without looking
/// through imports.
class DirectPrecedenceGroupLookupRequest
    : public SimpleRequest<DirectPrecedenceGroupLookupRequest,
                           TinyPtrVector<PrecedenceGroupDecl *>(
                               OperatorLookupDescriptor),
                           RequestFlags::Uncached |
                               RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  TinyPtrVector<PrecedenceGroupDecl *>
  evaluate(Evaluator &evaluator, OperatorLookupDescriptor descriptor) const;

public:
  // Incremental dependencies.
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const TinyPtrVector<PrecedenceGroupDecl *> &groups) const;
};

class LookupConformanceDescriptor final {
public:
  ModuleDecl *Mod;
  Type Ty;
  ProtocolDecl *PD;

  LookupConformanceDescriptor(ModuleDecl *Mod, Type Ty, ProtocolDecl *PD)
      : Mod(Mod), Ty(Ty), PD(PD) {}

  friend llvm::hash_code hash_value(const LookupConformanceDescriptor &desc) {
    return llvm::hash_combine(desc.Mod, desc.Ty.getPointer(), desc.PD);
  }

  friend bool operator==(const LookupConformanceDescriptor &lhs,
                         const LookupConformanceDescriptor &rhs) {
    return lhs.Mod == rhs.Mod && lhs.Ty.getPointer() == rhs.Ty.getPointer() &&
           lhs.PD == rhs.PD;
  }

  friend bool operator!=(const LookupConformanceDescriptor &lhs,
                         const LookupConformanceDescriptor &rhs) {
    return !(lhs == rhs);
  }
};

void simple_display(llvm::raw_ostream &out,
                    const LookupConformanceDescriptor &desc);

SourceLoc extractNearestSourceLoc(const LookupConformanceDescriptor &desc);

class LookupConformanceInModuleRequest
    : public SimpleRequest<LookupConformanceInModuleRequest,
                           ProtocolConformanceRef(LookupConformanceDescriptor),
                           RequestFlags::Uncached|RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ProtocolConformanceRef evaluate(
      Evaluator &evaluator, LookupConformanceDescriptor desc) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           ProtocolConformanceRef result) const;
};

/// Look up an 'infix operator' decl by name.
class LookupInfixOperatorRequest
    : public SimpleRequest<LookupInfixOperatorRequest,
                           TinyPtrVector<InfixOperatorDecl *>(
                               OperatorLookupDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  TinyPtrVector<InfixOperatorDecl *>
  evaluate(Evaluator &evaluator, OperatorLookupDescriptor desc) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Look up an 'prefix operator' decl by name.
class LookupPrefixOperatorRequest
    : public SimpleRequest<LookupPrefixOperatorRequest,
                           PrefixOperatorDecl *(OperatorLookupDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  PrefixOperatorDecl *evaluate(Evaluator &evaluator,
                               OperatorLookupDescriptor desc) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Look up an 'postfix operator' decl by name.
class LookupPostfixOperatorRequest
    : public SimpleRequest<LookupPostfixOperatorRequest,
                           PostfixOperatorDecl *(OperatorLookupDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  PostfixOperatorDecl *evaluate(Evaluator &evaluator,
                                OperatorLookupDescriptor desc) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Look up a precedencegroup decl by name.
class LookupPrecedenceGroupRequest
    : public SimpleRequest<LookupPrecedenceGroupRequest,
                           TinyPtrVector<PrecedenceGroupDecl *>(
                               OperatorLookupDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  TinyPtrVector<PrecedenceGroupDecl *>
  evaluate(Evaluator &evaluator, OperatorLookupDescriptor descriptor) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Computes whether this is a decl that supports being called through the
/// implementation of a \c callAsFunction method.
class IsCallAsFunctionNominalRequest
    : public SimpleRequest<IsCallAsFunctionNominalRequest,
                           bool(NominalTypeDecl *, DeclContext *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, NominalTypeDecl *decl,
                DeclContext *dc) const;

public:
  bool isCached() const { return true; }
};

/// Computes whether the specified decl or a super-class/super-protocol has the
/// @dynamicMemberLookup attribute on it.
class HasDynamicMemberLookupAttributeRequest
    : public SimpleRequest<HasDynamicMemberLookupAttributeRequest,
                           bool(NominalTypeDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  bool isCached() const { return true; }
};

/// Computes whether the specified decl or a super-class/super-protocol has the
/// @dynamicCallable attribute on it.
class HasDynamicCallableAttributeRequest
    : public SimpleRequest<HasDynamicCallableAttributeRequest,
                           bool(NominalTypeDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  bool isCached() const { return true; }
};

/// Determine the potential macro expansions for a given type or extension
/// context.
class PotentialMacroExpansionsInContextRequest
    : public SimpleRequest<
          PotentialMacroExpansionsInContextRequest,
          PotentialMacroExpansions(TypeOrExtensionDecl),
          RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PotentialMacroExpansions evaluate(
      Evaluator &evaluator, TypeOrExtensionDecl container) const;

public:
  bool isCached() const { return true; }
};

/// Resolves the protocol referenced by an @_implements attribute.
class ImplementsAttrProtocolRequest
    : public SimpleRequest<ImplementsAttrProtocolRequest,
                           ProtocolDecl *(const ImplementsAttr *, DeclContext *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ProtocolDecl *evaluate(Evaluator &evaluator, const ImplementsAttr *attr,
                         DeclContext *dc) const;

public:
  bool isCached() const { return true; }
};

#define SWIFT_TYPEID_ZONE NameLookup
#define SWIFT_TYPEID_HEADER "swift/AST/NameLookupTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

// Set up reporting of evaluated requests.
template<typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request);

#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
  template <>                                                                  \
  inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,              \
                                     const RequestType &request) {             \
    ++stats.getFrontendCounters().RequestType;                                 \
  }
#include "swift/AST/NameLookupTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_NAME_LOOKUP_REQUESTS

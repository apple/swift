//===--- TypeCheckRequests.h - Type Checking Requests -----------*- C++ -*-===//
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
//  This file defines type checking requests.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_TYPE_CHECK_REQUESTS_H
#define SWIFT_TYPE_CHECK_REQUESTS_H

#include "swift/AST/ActorIsolation.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/Effects.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Type.h"
#include "swift/AST/Evaluator.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SimpleRequest.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeResolutionStage.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class AbstractStorageDecl;
class AccessorDecl;
enum class AccessorKind;
class BreakStmt;
class ContextualPattern;
class ContinueStmt;
class DefaultArgumentExpr;
class DefaultArgumentType;
struct ExternalMacroDefinition;
class ClosureExpr;
class GenericParamList;
class LabeledStmt;
class MacroDefinition;
class PrecedenceGroupDecl;
class PropertyWrapperInitializerInfo;
struct PropertyWrapperLValueness;
struct PropertyWrapperMutability;
class RequirementRepr;
class ReturnStmt;
class SpecializeAttr;
class TrailingWhereClause;
class TypeAliasDecl;
class TypeLoc;
class Witness;
class TypeResolution;
struct TypeWitnessAndDecl;
class ValueDecl;
enum class OpaqueReadOwnership: uint8_t;
class StorageImplInfo;

/// Display a nominal type or extension thereof.
void simple_display(
    llvm::raw_ostream &out,
    const llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> &value);

void simple_display(llvm::raw_ostream &out, ASTContext *ctx);

/// Request the type from the ith entry in the inheritance clause for the
/// given declaration.
class InheritedTypeRequest
    : public SimpleRequest<
          InheritedTypeRequest,
          Type(llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *>,
               unsigned, TypeResolutionStage),
          RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type
  evaluate(Evaluator &evaluator,
           llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
           unsigned index, TypeResolutionStage stage) const;

public:
  // Source location
  SourceLoc getNearestLoc() const;

  // Caching
  bool isCached() const;
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

/// Request the superclass type for the given class.
class SuperclassTypeRequest
    : public SimpleRequest<
          SuperclassTypeRequest, Type(NominalTypeDecl *, TypeResolutionStage),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type
  evaluate(Evaluator &evaluator, NominalTypeDecl *classDecl,
           TypeResolutionStage stage) const;

public:
  // Cycle handling
  void diagnoseCycle(DiagnosticEngine &diags) const;

public:
  // Separate caching.
  bool isCached() const;
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;

public:
  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           Type t) const;
};

/// Request the raw type of the given enum.
class EnumRawTypeRequest
    : public SimpleRequest<EnumRawTypeRequest, Type(EnumDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, EnumDecl *enumDecl) const;

public:
  // Cycle handling
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  bool isCached() const { return true; }
};

/// Request to determine the set of declarations that were are overridden
/// by the given declaration.
class OverriddenDeclsRequest :
  public SimpleRequest<OverriddenDeclsRequest,
                       llvm::TinyPtrVector<ValueDecl *>(ValueDecl *),
                       RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::TinyPtrVector<ValueDecl *>
  evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<llvm::TinyPtrVector<ValueDecl *>> getCachedResult() const;
  void cacheResult(llvm::TinyPtrVector<ValueDecl *> value) const;
};

/// Determine whether the given declaration is exposed to Objective-C.
class IsObjCRequest :
    public SimpleRequest<IsObjCRequest,
                         bool(ValueDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

void simple_display(llvm::raw_ostream &out, CtorInitializerKind initKind);

/// Computes the kind of initializer for a given \c ConstructorDecl
class InitKindRequest :
    public SimpleRequest<InitKindRequest,
                         CtorInitializerKind(ConstructorDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  CtorInitializerKind
  evaluate(Evaluator &evaluator, ConstructorDecl *decl) const;

public:
// Caching.
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out, BodyInitKind initKind);
void simple_display(llvm::raw_ostream &out, BodyInitKindAndExpr initKindAndExpr);

/// Computes the kind of initializer call (self.init or super.init) performed
/// in the body of a \c ConstructorDecl
class BodyInitKindRequest :
    public SimpleRequest<BodyInitKindRequest,
                         BodyInitKindAndExpr(ConstructorDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  BodyInitKindAndExpr evaluate(Evaluator &evaluator, ConstructorDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Determine whether the given protocol declaration is class-bounded.
class ProtocolRequiresClassRequest :
    public SimpleRequest<ProtocolRequiresClassRequest,
                         bool(ProtocolDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ProtocolDecl *decl) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determine whether an existential conforming to a protocol can be matched
/// with a generic type parameter constrained to that protocol.
class ExistentialConformsToSelfRequest :
    public SimpleRequest<ExistentialConformsToSelfRequest,
                         bool(ProtocolDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ProtocolDecl *decl) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determine whether an existential type conforming to this protocol
/// requires the \c any syntax.
class HasSelfOrAssociatedTypeRequirementsRequest :
    public SimpleRequest<HasSelfOrAssociatedTypeRequirementsRequest,
                         bool(ProtocolDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ProtocolDecl *decl) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Find the list of primary associated types of the given protocol.
class PrimaryAssociatedTypesRequest :
    public SimpleRequest<PrimaryAssociatedTypesRequest,
                         ArrayRef<AssociatedTypeDecl *>(ProtocolDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<AssociatedTypeDecl *>
  evaluate(Evaluator &evaluator, ProtocolDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class PolymorphicEffectRequirementsRequest :
    public SimpleRequest<PolymorphicEffectRequirementsRequest,
                         PolymorphicEffectRequirementList(EffectKind, ProtocolDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PolymorphicEffectRequirementList
  evaluate(Evaluator &evaluator, EffectKind kind, ProtocolDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class ConformanceHasEffectRequest :
    public SimpleRequest<ConformanceHasEffectRequest,
                         bool(EffectKind, ProtocolConformance *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool 
  evaluate(Evaluator &evaluator, EffectKind kind,
           ProtocolConformance *conformance) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Determine whether the given declaration is 'final'.
class IsFinalRequest :
    public SimpleRequest<IsFinalRequest,
                         bool(ValueDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determine whether the given declaration is 'moveOnly'.
class IsMoveOnlyRequest
    : public SimpleRequest<IsMoveOnlyRequest, bool(ValueDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determine whether the given declaration is 'dynamic''.
class IsDynamicRequest :
    public SimpleRequest<IsDynamicRequest,
                         bool(ValueDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

class StructuralRequirementsRequest :
    public SimpleRequest<StructuralRequirementsRequest,
                         ArrayRef<StructuralRequirement>(ProtocolDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<StructuralRequirement>
  evaluate(Evaluator &evaluator, ProtocolDecl *proto) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class TypeAliasRequirementsRequest :
    public SimpleRequest<TypeAliasRequirementsRequest,
                         ArrayRef<Requirement>(ProtocolDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<Requirement>
  evaluate(Evaluator &evaluator, ProtocolDecl *proto) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class ProtocolDependenciesRequest :
    public SimpleRequest<ProtocolDependenciesRequest,
                         ArrayRef<ProtocolDecl *>(ProtocolDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<ProtocolDecl *>
  evaluate(Evaluator &evaluator, ProtocolDecl *proto) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Compute the requirements that describe a protocol.
class RequirementSignatureRequest :
    public SimpleRequest<RequirementSignatureRequest,
                         RequirementSignature(ProtocolDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  RequirementSignature
  evaluate(Evaluator &evaluator, ProtocolDecl *proto) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<RequirementSignature> getCachedResult() const;
  void cacheResult(RequirementSignature value) const;
};

/// Compute the default definition type of an associated type.
class DefaultDefinitionTypeRequest :
    public SimpleRequest<DefaultDefinitionTypeRequest,
                         Type(AssociatedTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, AssociatedTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Describes the owner of a where clause, from which we can extract
/// requirements.
struct WhereClauseOwner {
  /// The declaration context in which the where clause will be evaluated.
  DeclContext *dc;

  /// The source of the where clause, which can be a generic parameter list
  /// or a declaration that can have a where clause.
  llvm::PointerUnion<GenericParamList *, TrailingWhereClause *,
                     SpecializeAttr *, DifferentiableAttr *>
      source;

  WhereClauseOwner() : dc(nullptr) {}

  WhereClauseOwner(GenericContext *genCtx);
  WhereClauseOwner(AssociatedTypeDecl *atd);

  WhereClauseOwner(DeclContext *dc, GenericParamList *genericParams)
      : dc(dc), source(genericParams) {}

  WhereClauseOwner(DeclContext *dc, TrailingWhereClause *where)
      : dc(dc), source(where) {}

  WhereClauseOwner(DeclContext *dc, SpecializeAttr *attr)
      : dc(dc), source(attr) {}

  WhereClauseOwner(DeclContext *dc, DifferentiableAttr *attr)
      : dc(dc), source(attr) {}

  SourceLoc getLoc() const;

  friend hash_code hash_value(const WhereClauseOwner &owner) {
    return llvm::hash_value(owner.source.getOpaqueValue());
  }

  operator bool() const {
    return dc != nullptr;
  }

  friend bool operator==(const WhereClauseOwner &lhs,
                         const WhereClauseOwner &rhs) {
    return lhs.source.getOpaqueValue() == rhs.source.getOpaqueValue();
  }

  friend bool operator!=(const WhereClauseOwner &lhs,
                         const WhereClauseOwner &rhs) {
    return !(lhs == rhs);
  }

  /// Retrieve the array of requirements.
  MutableArrayRef<RequirementRepr> getRequirements() const;

  /// Visit each of the requirements,
  ///
  /// \returns true after short-circuiting if the callback returned \c true
  /// for any of the requirements.
  bool
  visitRequirements(TypeResolutionStage stage,
                    llvm::function_ref<bool(Requirement, RequirementRepr *)>
                        callback) const &&;
};

void simple_display(llvm::raw_ostream &out, const WhereClauseOwner &owner);

/// Retrieve a requirement from the where clause of the given declaration.
class RequirementRequest :
    public SimpleRequest<RequirementRequest,
                         Requirement(WhereClauseOwner, unsigned,
                                     TypeResolutionStage),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  /// Retrieve the requirement this request operates on.
  RequirementRepr &getRequirement() const;

  // Evaluation.
  Requirement evaluate(Evaluator &evaluator,
                       WhereClauseOwner,
                       unsigned index,
                       TypeResolutionStage stage) const;

public:
  // Source location
  SourceLoc getNearestLoc() const;

  // Cycle handling.
  void noteCycleStep(DiagnosticEngine &diags) const;
                           
  // Caching.
  bool isCached() const;
};

/// Generate the USR for the given declaration.
class USRGenerationRequest :
    public SimpleRequest<USRGenerationRequest,
                         std::string(const ValueDecl*),
                         RequestFlags::Cached>
{
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::string evaluate(Evaluator &eval, const ValueDecl *d) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Generate the mangling for the given local type declaration.
class MangleLocalTypeDeclRequest :
    public SimpleRequest<MangleLocalTypeDeclRequest,
                         std::string(const TypeDecl*),
                         RequestFlags::Cached>
{
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::string evaluate(Evaluator &eval, const TypeDecl *d) const;

public:
  // Caching
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out, const KnownProtocolKind);

// Find the type in the cache or look it up
class DefaultTypeRequest
    : public SimpleRequest<DefaultTypeRequest,
                           Type(KnownProtocolKind, const DeclContext *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &eval, KnownProtocolKind, const DeclContext *) const;

public:
  // Caching
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

/// Retrieve information about a property wrapper type.
class PropertyWrapperTypeInfoRequest
  : public SimpleRequest<PropertyWrapperTypeInfoRequest,
                         PropertyWrapperTypeInfo(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PropertyWrapperTypeInfo
  evaluate(Evaluator &eval, NominalTypeDecl *nominal) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the nominal type declaration to which the given custom attribute
/// refers.
class AttachedPropertyWrappersRequest :
    public SimpleRequest<AttachedPropertyWrappersRequest,
                         llvm::TinyPtrVector<CustomAttr *>(VarDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::TinyPtrVector<CustomAttr *>
  evaluate(Evaluator &evaluator, VarDecl *) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the raw (possibly unbound generic) type of the property wrapper
/// that is attached to the given variable.
class AttachedPropertyWrapperTypeRequest :
    public SimpleRequest<AttachedPropertyWrapperTypeRequest,
                         Type(VarDecl *, unsigned),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type
  evaluate(Evaluator &evaluator, VarDecl *var, unsigned i) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the nominal type declaration to which the given custom attribute
/// refers.
class PropertyWrapperBackingPropertyTypeRequest :
    public SimpleRequest<PropertyWrapperBackingPropertyTypeRequest,
                         Type(VarDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type
  evaluate(Evaluator &evaluator, VarDecl *var) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request information about the mutability of composed property wrappers.
class PropertyWrapperMutabilityRequest
    : public SimpleRequest<PropertyWrapperMutabilityRequest,
                           llvm::Optional<PropertyWrapperMutability>(VarDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::Optional<PropertyWrapperMutability> evaluate(Evaluator &evaluator,
                                                     VarDecl *var) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request information about the l-valueness of composed property wrappers.
class PropertyWrapperLValuenessRequest
    : public SimpleRequest<PropertyWrapperLValuenessRequest,
                           llvm::Optional<PropertyWrapperLValueness>(VarDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::Optional<PropertyWrapperLValueness> evaluate(Evaluator &evaluator,
                                                     VarDecl *var) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the synthesized auxiliary declarations for a wrapped property.
class PropertyWrapperAuxiliaryVariablesRequest :
    public SimpleRequest<PropertyWrapperAuxiliaryVariablesRequest,
                         PropertyWrapperAuxiliaryVariables(VarDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PropertyWrapperAuxiliaryVariables
  evaluate(Evaluator &evaluator, VarDecl *var) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request information about initialization of the backing property
/// for properties that have attached property wrappers.
class PropertyWrapperInitializerInfoRequest :
    public SimpleRequest<PropertyWrapperInitializerInfoRequest,
                         PropertyWrapperInitializerInfo(VarDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PropertyWrapperInitializerInfo
  evaluate(Evaluator &evaluator, VarDecl *var) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Retrieve the structural type of an alias type.
class StructuralTypeRequest :
    public SimpleRequest<StructuralTypeRequest,
                         Type(TypeAliasDecl*),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &eval, TypeAliasDecl *d) const;

public:
  // Caching.
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
};

/// Request the fragile function kind for the context.
class FragileFunctionKindRequest :
    public SimpleRequest<FragileFunctionKindRequest,
                         FragileFunctionKind(DeclContext*),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  FragileFunctionKind evaluate(Evaluator &eval, DeclContext *context) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out, FragileFunctionKind value);

void simple_display(llvm::raw_ostream &out, ResilienceExpansion value);

class PolymorphicEffectKindRequest :
    public SimpleRequest<PolymorphicEffectKindRequest,
                         PolymorphicEffectKind(EffectKind, AbstractFunctionDecl*),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PolymorphicEffectKind evaluate(Evaluator &evaluator,
                                 EffectKind kind,
                                 AbstractFunctionDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Request the custom attribute which attaches a result builder to the
/// given declaration.
class AttachedResultBuilderRequest :
    public SimpleRequest<AttachedResultBuilderRequest,
                         CustomAttr *(ValueDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  CustomAttr *
  evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request the result builder type attached to the given declaration,
/// if any.
class ResultBuilderTypeRequest :
    public SimpleRequest<ResultBuilderTypeRequest,
                         Type(ValueDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  Type
  evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Request a function's self access kind.
class SelfAccessKindRequest :
    public SimpleRequest<SelfAccessKindRequest,
                         SelfAccessKind(FuncDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  SelfAccessKind
  evaluate(Evaluator &evaluator, FuncDecl *func) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<SelfAccessKind> getCachedResult() const;
  void cacheResult(SelfAccessKind value) const;
};

/// Determine whether the given nominal type is an actor.
class IsActorRequest :
    public SimpleRequest<IsActorRequest,
                         bool(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, NominalTypeDecl *nominal) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Determine whether the given class is a default actor.
class IsDefaultActorRequest :
    public SimpleRequest<IsDefaultActorRequest,
                         bool(ClassDecl *, ModuleDecl *, ResilienceExpansion),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, ClassDecl *classDecl,
                ModuleDecl *M, ResilienceExpansion expansion) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Determine whether the given class is a distributed actor.
class IsDistributedActorRequest :
    public SimpleRequest<IsDistributedActorRequest,
        bool(NominalTypeDecl *),
        RequestFlags::Cached> {
public:
    using SimpleRequest::SimpleRequest;

private:
    friend SimpleRequest;

    bool evaluate(Evaluator &evaluator, NominalTypeDecl *nominal) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Retrieve the implicit conformance for the given distributed actor type to
/// the Codable protocol protocol.
///
/// Similar to 'GetImplicitSendableRequest'.
class GetDistributedActorImplicitCodableRequest :
    public SimpleRequest<GetDistributedActorImplicitCodableRequest,
                         NormalProtocolConformance *(NominalTypeDecl *, KnownProtocolKind),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  NormalProtocolConformance *evaluate(
      Evaluator &evaluator,
      NominalTypeDecl *nominal,
      KnownProtocolKind protoKind) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Check a distributed function declaration and cache if it was valid or not.
///
/// This is used because we not only type-check to emit errors, but also use
/// the information to potentially avoid emitting the distributed thunk for
/// methods which are invalid (e.g. their parameters dont conform to
/// SerializationRequirement), as otherwise we'd be causing errors in synthesized
/// code which looks very confusing to end-users.
///
class CheckDistributedFunctionRequest :
    public SimpleRequest<CheckDistributedFunctionRequest,
                         bool(AbstractFunctionDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  /// \returns \c true if there was a problem with the function declaration,
  /// \c false otherwise.
  bool evaluate(Evaluator &evaluator, AbstractFunctionDecl *) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Obtain the 'remoteCall' function of a 'DistributedActorSystem'.
class GetDistributedActorSystemRemoteCallFunctionRequest :
    public SimpleRequest<GetDistributedActorSystemRemoteCallFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *, bool voidReturn),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *actorSystem, bool voidReturn) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'recordArgument' function of a 'DistributedTargetInvocationEncoder'.
class GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest :
    public SimpleRequest<GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *encoder) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'recordReturnType' function of a 'DistributedTargetInvocationEncoder'.
class GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest :
    public SimpleRequest<GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *encoder) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'recordErrorType' function of a 'DistributedTargetInvocationEncoder'.
class GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest :
    public SimpleRequest<GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *encoder) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'decodeNextArgument' function of a 'DistributedTargetInvocationDecoder'.
class GetDistributedTargetInvocationDecoderDecodeNextArgumentFunctionRequest :
    public SimpleRequest<GetDistributedTargetInvocationDecoderDecodeNextArgumentFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *encoder) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'onReturn' function of a 'DistributedTargetInvocationResultHandler'.
class GetDistributedTargetInvocationResultHandlerOnReturnFunctionRequest :
    public SimpleRequest<GetDistributedTargetInvocationResultHandlerOnReturnFunctionRequest,
                         AbstractFunctionDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  AbstractFunctionDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *encoder) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'actorSystem' property of a 'distributed actor'.
class GetDistributedActorSystemPropertyRequest :
    public SimpleRequest<GetDistributedActorSystemPropertyRequest,
                         VarDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  VarDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *actor) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the constructor of the 'RemoteCallTarget' type.
class GetDistributedRemoteCallTargetInitFunctionRequest :
    public SimpleRequest<GetDistributedRemoteCallTargetInitFunctionRequest,
                         ConstructorDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ConstructorDecl *evaluate(Evaluator &evaluator,
                            NominalTypeDecl *nominal) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the constructor of the 'RemoteCallArgument' type.
class GetDistributedRemoteCallArgumentInitFunctionRequest :
    public SimpleRequest<GetDistributedRemoteCallArgumentInitFunctionRequest,
                         ConstructorDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ConstructorDecl *evaluate(Evaluator &evaluator,
                            NominalTypeDecl *nominal) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the 'distributed thunk' for the passed-in function.
///
/// The thunk is responsible for invoking 'remoteCall' when invoked on a remote
/// 'distributed actor'.
class GetDistributedThunkRequest
    : public SimpleRequest<GetDistributedThunkRequest,
                           FuncDecl *(
                               llvm::PointerUnion<AbstractStorageDecl *,
                                                  AbstractFunctionDecl *>),
                           RequestFlags::Cached> {
  using Originator =
      llvm::PointerUnion<AbstractStorageDecl *, AbstractFunctionDecl *>;

public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  FuncDecl *evaluate(Evaluator &evaluator, Originator originator) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Obtain the 'id' property of a 'distributed actor'.
class GetDistributedActorIDPropertyRequest :
    public SimpleRequest<GetDistributedActorIDPropertyRequest,
                         VarDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  VarDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *actor) const;

public:
    // Caching
    bool isCached() const { return true; }
};

/// Obtain the invocation decoder associated with the given distributed actor.
class GetDistributedActorInvocationDecoderRequest :
  public SimpleRequest<GetDistributedActorInvocationDecoderRequest,
                       NominalTypeDecl *(NominalTypeDecl *),
                       RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  NominalTypeDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *actor) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Obtain the method that could be used to decode argument values passed
/// to a particular actor invocation type.
class GetDistributedActorArgumentDecodingMethodRequest :
  public SimpleRequest<GetDistributedActorArgumentDecodingMethodRequest,
                       FuncDecl *(NominalTypeDecl *),
                       RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  FuncDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *actor) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Retrieve the static "shared" property within a global actor that provides
/// the actor instance representing the global actor.
///
/// Global actors can be applied to a declaration to indicate that the
/// declaration operations on state that is protected by the global actor.
class GlobalActorInstanceRequest :
    public SimpleRequest<GlobalActorInstanceRequest,
                         VarDecl *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  VarDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *nominal) const;

public:
  // Caching
  bool isCached() const { return true; }
};

using CustomAttrNominalPair = std::pair<CustomAttr *, NominalTypeDecl *>;

/// Request the custom attribute which denotes the global actor for the given
/// declaration.
///
/// This is the "raw" global actor attribute as written directly on the
/// declaration, with any inference rules applied.
class GlobalActorAttributeRequest
    : public SimpleRequest<GlobalActorAttributeRequest,
                           llvm::Optional<CustomAttrNominalPair>(
                               llvm::PointerUnion<Decl *, ClosureExpr *>),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::Optional<std::pair<CustomAttr *, NominalTypeDecl *>>
  evaluate(Evaluator &evaluator,
           llvm::PointerUnion<Decl *, ClosureExpr *>) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Determine the actor isolation for the given declaration.
class ActorIsolationRequest :
    public SimpleRequest<ActorIsolationRequest,
                         ActorIsolation(ValueDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ActorIsolation evaluate(Evaluator &evaluator, ValueDecl *value) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Determine whether the given function should have an isolated 'self'.
class HasIsolatedSelfRequest :
    public SimpleRequest<HasIsolatedSelfRequest,
                         bool(ValueDecl *),
                         RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, ValueDecl *func) const;
};

/// Request whether the storage has a mutating getter.
class IsGetterMutatingRequest :
    public SimpleRequest<IsGetterMutatingRequest,
                         bool(AbstractStorageDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, AbstractStorageDecl *func) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Request whether the storage has a mutating getter.
class IsSetterMutatingRequest :
    public SimpleRequest<IsSetterMutatingRequest,
                         bool(AbstractStorageDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, AbstractStorageDecl *func) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Request whether reading the storage yields a borrowed value.
class OpaqueReadOwnershipRequest :
    public SimpleRequest<OpaqueReadOwnershipRequest,
                         OpaqueReadOwnership(AbstractStorageDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  OpaqueReadOwnership
  evaluate(Evaluator &evaluator, AbstractStorageDecl *storage) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<OpaqueReadOwnership> getCachedResult() const;
  void cacheResult(OpaqueReadOwnership value) const;
};

/// Request to build the underlying storage for a lazy property.
class LazyStoragePropertyRequest :
    public SimpleRequest<LazyStoragePropertyRequest,
                         VarDecl *(VarDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  VarDecl *
  evaluate(Evaluator &evaluator, VarDecl *lazyVar) const;

public:
  bool isCached() const { return true; }
};

/// Request to retrieve the type-checked body of the given function.
class TypeCheckFunctionBodyRequest
    : public SimpleRequest<
          TypeCheckFunctionBodyRequest, BraceStmt *(AbstractFunctionDecl *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySource> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  BraceStmt *evaluate(Evaluator &evaluator, AbstractFunctionDecl *func) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<BraceStmt *> getCachedResult() const;
  void cacheResult(BraceStmt *body) const;

public:
  // Incremental dependencies.
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &) const;
};

/// Describes the context in which the AST node to type check in a
/// \c TypeCheckASTNodeAtLocRequest should be searched. This can be either of
/// two cases:
///  1. A \c DeclContext that contains the node representing the location to
///     type check
///  2. If the node that should be type checked that might not be part of the
///     AST (e.g. because it is a dangling property attribute), an \c ASTNode
///     that contains the location to type check in together with a DeclContext
///     in which we should pretend that node occurs.
class TypeCheckASTNodeAtLocContext {
  DeclContext *DC;
  ASTNode Node;

  /// Memberwise initializer
  TypeCheckASTNodeAtLocContext(DeclContext *DC, ASTNode Node)
      : DC(DC), Node(Node) {
    assert(DC != nullptr);
  }

public:
  static TypeCheckASTNodeAtLocContext declContext(DeclContext *DC) {
    return TypeCheckASTNodeAtLocContext(DC, /*Node=*/nullptr);
  }

  static TypeCheckASTNodeAtLocContext node(DeclContext *DC, ASTNode Node) {
    assert(!Node.isNull());
    return TypeCheckASTNodeAtLocContext(DC, Node);
  }

  DeclContext *getDeclContext() const { return DC; }

  bool isForUnattachedNode() const { return !Node.isNull(); }

  ASTNode getUnattachedNode() const {
    assert(isForUnattachedNode());
    return Node;
  }

  ASTNode &getUnattachedNode() {
    assert(isForUnattachedNode());
    return Node;
  }

  friend llvm::hash_code hash_value(const TypeCheckASTNodeAtLocContext &ctx) {
    return llvm::hash_combine(ctx.DC, ctx.Node);
  }

  friend bool operator==(const TypeCheckASTNodeAtLocContext &lhs,
                         const TypeCheckASTNodeAtLocContext &rhs) {
    return lhs.DC == rhs.DC && lhs.Node == rhs.Node;
  }

  friend bool operator!=(const TypeCheckASTNodeAtLocContext &lhs,
                         const TypeCheckASTNodeAtLocContext &rhs) {
    return !(lhs == rhs);
  }

  friend SourceLoc
  extractNearestSourceLoc(const TypeCheckASTNodeAtLocContext &ctx) {
    if (!ctx.Node.isNull()) {
      return ctx.Node.getStartLoc();
    } else {
      return extractNearestSourceLoc(ctx.DC);
    }
  }
};

void simple_display(llvm::raw_ostream &out,
                    const TypeCheckASTNodeAtLocContext &ctx);

/// Request to typecheck a function body element at the given source location.
///
/// Produces true if an error occurred, false otherwise.
class TypeCheckASTNodeAtLocRequest
    : public SimpleRequest<TypeCheckASTNodeAtLocRequest,
                           bool(TypeCheckASTNodeAtLocContext, SourceLoc),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, TypeCheckASTNodeAtLocContext,
                SourceLoc Loc) const;
};

/// Request to obtain a list of stored properties in a nominal type.
///
/// This will include backing storage for lazy properties and
/// property wrappers, synthesizing them if necessary.
class StoredPropertiesRequest :
    public SimpleRequest<StoredPropertiesRequest,
                         ArrayRef<VarDecl *>(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<VarDecl *>
  evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  bool isCached() const { return true; }
};

/// Request to obtain a list of stored properties in a nominal type,
/// together with any missing members corresponding to stored
/// properties that could not be deserialized.
///
/// This will include backing storage for lazy properties and
/// property wrappers, synthesizing them if necessary.
class StoredPropertiesAndMissingMembersRequest :
    public SimpleRequest<StoredPropertiesAndMissingMembersRequest,
                         ArrayRef<Decl *>(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<Decl *>
  evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  bool isCached() const { return true; }
};

/// Request to obtain a list of computed properties with init accesors
/// in the given nominal type.
class InitAccessorPropertiesRequest :
    public SimpleRequest<InitAccessorPropertiesRequest,
                         ArrayRef<VarDecl *>(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<VarDecl *>
  evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  bool isCached() const { return true; }
};

/// Request to obtain a list of properties that will be reflected in the parameters of a
/// memberwise initializer.
class MemberwiseInitPropertiesRequest :
    public SimpleRequest<MemberwiseInitPropertiesRequest,
                         ArrayRef<VarDecl *>(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<VarDecl *>
  evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  bool isCached() const { return true; }
};

class HasStorageRequest :
    public SimpleRequest<HasStorageRequest,
                         bool(AbstractStorageDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  bool isCached() const { return true; }
};

class StorageImplInfoRequest :
    public SimpleRequest<StorageImplInfoRequest,
                         StorageImplInfo(AbstractStorageDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  StorageImplInfo
  evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<StorageImplInfo> getCachedResult() const;
  void cacheResult(StorageImplInfo value) const;
};

class RequiresOpaqueAccessorsRequest :
    public SimpleRequest<RequiresOpaqueAccessorsRequest,
                         bool(VarDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, VarDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

class RequiresOpaqueModifyCoroutineRequest :
    public SimpleRequest<RequiresOpaqueModifyCoroutineRequest,
                         bool(AbstractStorageDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

class IsAccessorTransparentRequest :
    public SimpleRequest<IsAccessorTransparentRequest,
                         bool(AccessorDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, AccessorDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

class SynthesizeAccessorRequest :
    public SimpleRequest<SynthesizeAccessorRequest,
                         AccessorDecl *(AbstractStorageDecl *,
                                        AccessorKind),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  AccessorDecl *
  evaluate(Evaluator &evaluator, AbstractStorageDecl *decl,
           AccessorKind kind) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<AccessorDecl *> getCachedResult() const;
  void cacheResult(AccessorDecl *value) const;
};

class ABIMembersRequest :
    public SimpleRequest<ABIMembersRequest,
                         ArrayRef<Decl *>(IterableDeclContext *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<Decl *>
  evaluate(Evaluator &evaluator, IterableDeclContext *idc) const;

public:
  bool isCached() const { return true; }
};

class AllMembersRequest :
    public SimpleRequest<AllMembersRequest,
                         ArrayRef<Decl *>(IterableDeclContext *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<Decl *>
  evaluate(Evaluator &evaluator, IterableDeclContext *idc) const;

public:
  bool isCached() const { return true; }
};

class IsImplicitlyUnwrappedOptionalRequest :
    public SimpleRequest<IsImplicitlyUnwrappedOptionalRequest,
                         bool(ValueDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, ValueDecl *value) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

class ClassAncestryFlagsRequest :
    public SimpleRequest<ClassAncestryFlagsRequest,
                         AncestryFlags (ClassDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  AncestryFlags
  evaluate(Evaluator &evaluator, ClassDecl *value) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out, AncestryFlags value);

class AbstractGenericSignatureRequest :
    public SimpleRequest<AbstractGenericSignatureRequest,
                         GenericSignatureWithError (const GenericSignatureImpl *,
                                                    SmallVector<GenericTypeParamType *, 2>,
                                                    SmallVector<Requirement, 2>),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  GenericSignatureWithError
  evaluate(Evaluator &evaluator,
           const GenericSignatureImpl *baseSignature,
           SmallVector<GenericTypeParamType *, 2> addedParameters,
           SmallVector<Requirement, 2> addedRequirements) const;

public:
  // Separate caching.
  bool isCached() const { return true; }

  /// Abstract generic signature requests never have source-location info.
  SourceLoc getNearestLoc() const {
    return SourceLoc();
  }
};

class InferredGenericSignatureRequest :
    public SimpleRequest<InferredGenericSignatureRequest,
                         GenericSignatureWithError (const GenericSignatureImpl *,
                                                    GenericParamList *,
                                                    WhereClauseOwner,
                                                    SmallVector<Requirement, 2>,
                                                    SmallVector<TypeLoc, 2>,
                                                    bool),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  GenericSignatureWithError
  evaluate(Evaluator &evaluator,
           const GenericSignatureImpl *baseSignature,
           GenericParamList *genericParams,
           WhereClauseOwner whereClause,
           SmallVector<Requirement, 2> addedRequirements,
           SmallVector<TypeLoc, 2> inferenceSources,
           bool allowConcreteGenericParams) const;

public:
  // Separate caching.
  bool isCached() const { return true; }

  /// Inferred generic signature requests don't have source-location info.
  SourceLoc getNearestLoc() const {
    return SourceLoc();
  }

  // Cycle handling.
  void noteCycleStep(DiagnosticEngine &diags) const;
};

void simple_display(llvm::raw_ostream &out, const TypeLoc source);

class ExtendedTypeRequest
    : public SimpleRequest<ExtendedTypeRequest,
                           Type(ExtensionDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &eval, ExtensionDecl *) const;
public:
  // Caching.
  bool isCached() const { return true; }
};

class FunctionOperatorRequest :
    public SimpleRequest<FunctionOperatorRequest,
                         OperatorDecl *(FuncDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;
  
private:
  friend SimpleRequest;
  
  // Evaluation.
  OperatorDecl *
  evaluate(Evaluator &evaluator, FuncDecl *value) const;
  
public:
  // Caching.
  bool isCached() const { return true; }
};

class GenericSignatureRequest :
    public SimpleRequest<GenericSignatureRequest,
                         GenericSignature (GenericContext *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;
  
private:
  friend SimpleRequest;
  
  // Evaluation.
  GenericSignature
  evaluate(Evaluator &evaluator, GenericContext *value) const;
  
public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<GenericSignature> getCachedResult() const;
  void cacheResult(GenericSignature value) const;

  void diagnoseCycle(DiagnosticEngine &diags) const;
};

/// Compute the underlying interface type of a typealias.
class UnderlyingTypeRequest :
    public SimpleRequest<UnderlyingTypeRequest,
                         Type(TypeAliasDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, TypeAliasDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
  void diagnoseCycle(DiagnosticEngine &diags) const;
};

/// Looks up the precedence group of an operator declaration.
class OperatorPrecedenceGroupRequest
    : public SimpleRequest<OperatorPrecedenceGroupRequest,
                           PrecedenceGroupDecl *(InfixOperatorDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  PrecedenceGroupDecl *
  evaluate(Evaluator &evaluator, InfixOperatorDecl *PGD) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

/// Computes the raw values for an enum type.
class EnumRawValuesRequest :
    public SimpleRequest<EnumRawValuesRequest,
                         evaluator::SideEffect (EnumDecl *, TypeResolutionStage),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;
  
private:
  friend SimpleRequest;
  
  // Evaluation.
  evaluator::SideEffect
  evaluate(Evaluator &evaluator, EnumDecl *ED, TypeResolutionStage stage) const;
  
public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
                           
  // Separate caching.
  bool isCached() const;
  llvm::Optional<evaluator::SideEffect> getCachedResult() const;
  void cacheResult(evaluator::SideEffect value) const;
};

/// Determines if an override is ABI compatible with its base method.
class IsABICompatibleOverrideRequest
    : public SimpleRequest<IsABICompatibleOverrideRequest, bool(ValueDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Builds an opaque result type for a declaration.
class OpaqueResultTypeRequest
    : public SimpleRequest<OpaqueResultTypeRequest,
                           OpaqueTypeDecl *(ValueDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  OpaqueTypeDecl *
  evaluate(Evaluator &evaluator, ValueDecl *VD) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Determines if a function declaration is 'static'.
class IsStaticRequest :
    public SimpleRequest<IsStaticRequest,
                         bool(FuncDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool
  evaluate(Evaluator &evaluator, FuncDecl *value) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determines if a method override should introduce a new vtable entry,
/// because the override is not ABI compatible, or the base method is
/// less visible than the override.
class NeedsNewVTableEntryRequest
    : public SimpleRequest<NeedsNewVTableEntryRequest,
                           bool(AbstractFunctionDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AbstractFunctionDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Determines the ownership specifier for a parameter
/// (inout/consuming/borrowing)
class ParamSpecifierRequest
    : public SimpleRequest<ParamSpecifierRequest,
                           ParamSpecifier(ParamDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ParamSpecifier
  evaluate(Evaluator &evaluator, ParamDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<ParamSpecifier> getCachedResult() const;
  void cacheResult(ParamSpecifier value) const;
};

/// Determines the result type of a function or element type of a subscript.
class ResultTypeRequest
    : public SimpleRequest<ResultTypeRequest,
                           Type(ValueDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

class PatternBindingEntryRequest
    : public SimpleRequest<PatternBindingEntryRequest,
                           const PatternBindingEntry *(PatternBindingDecl *,
                                                       unsigned, bool),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  const PatternBindingEntry *evaluate(Evaluator &evaluator,
                                      PatternBindingDecl *PBD, unsigned i,
                                      bool LeaveClosureBodiesUnchecked) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<const PatternBindingEntry *> getCachedResult() const;
  void cacheResult(const PatternBindingEntry *value) const;
};

class NamingPatternRequest
    : public SimpleRequest<NamingPatternRequest, NamedPattern *(VarDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  NamedPattern * evaluate(Evaluator &evaluator, VarDecl *VD) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<NamedPattern *> getCachedResult() const;
  void cacheResult(NamedPattern *P) const;
};

class ExprPatternMatchResult {
  VarDecl *MatchVar;
  Expr *MatchExpr;

  friend class ExprPattern;

  // Should only be used as the default value for the request, as the caching
  // logic assumes the request always produces a non-null result.
  ExprPatternMatchResult(llvm::NoneType)
      : MatchVar(nullptr), MatchExpr(nullptr) {}

public:
  ExprPatternMatchResult(VarDecl *matchVar, Expr *matchExpr)
      : MatchVar(matchVar), MatchExpr(matchExpr) {
    // Note the caching logic currently requires the request to produce a
    // non-null result.
    assert(matchVar && matchExpr);
  }

  VarDecl *getMatchVar() const { return MatchVar; }
  Expr *getMatchExpr() const { return MatchExpr; }
};

/// Compute the match VarDecl and expression for an ExprPattern, which applies
/// the \c ~= operator.
class ExprPatternMatchRequest
    : public SimpleRequest<ExprPatternMatchRequest,
                           ExprPatternMatchResult(const ExprPattern *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ExprPatternMatchResult evaluate(Evaluator &evaluator,
                                  const ExprPattern *EP) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<ExprPatternMatchResult> getCachedResult() const;
  void cacheResult(ExprPatternMatchResult result) const;
};

/// Creates a corresponding ExprPattern from the original Expr of an
/// EnumElementPattern. This needs to be a cached request to ensure we don't
/// generate multiple ExprPatterns along different constraint solver paths.
class EnumElementExprPatternRequest
    : public SimpleRequest<EnumElementExprPatternRequest,
                           ExprPattern *(const EnumElementPattern *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ExprPattern *evaluate(Evaluator &evaluator,
                        const EnumElementPattern *EEP) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

class InterfaceTypeRequest :
    public SimpleRequest<InterfaceTypeRequest,
                         Type (ValueDecl *),
                         RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type
  evaluate(Evaluator &evaluator, ValueDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

struct PrecedenceGroupDescriptor {
  enum PathDirection : bool {
    LowerThan = false,
    HigherThan = true,
  };
  DeclContext *dc;
  Identifier ident;
  SourceLoc nameLoc;
  // Exists for diagnostics. Does not contribute to the descriptor otherwise.
  llvm::Optional<PathDirection> pathDirection;

  SourceLoc getLoc() const;

  friend llvm::hash_code hash_value(const PrecedenceGroupDescriptor &owner) {
    return llvm::hash_combine(owner.dc,
                              owner.ident.getAsOpaquePointer(),
                              owner.nameLoc.getOpaquePointerValue());
  }

  friend bool operator==(const PrecedenceGroupDescriptor &lhs,
                         const PrecedenceGroupDescriptor &rhs) {
    return lhs.dc == rhs.dc &&
           lhs.ident == rhs.ident &&
           lhs.nameLoc == rhs.nameLoc;
  }

  friend bool operator!=(const PrecedenceGroupDescriptor &lhs,
                         const PrecedenceGroupDescriptor &rhs) {
    return !(lhs == rhs);
  }
};

void simple_display(llvm::raw_ostream &out, const PrecedenceGroupDescriptor &d);

class ValidatePrecedenceGroupRequest
    : public SimpleRequest<ValidatePrecedenceGroupRequest,
                           TinyPtrVector<PrecedenceGroupDecl *>(
                               PrecedenceGroupDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  TinyPtrVector<PrecedenceGroupDecl *>
  evaluate(Evaluator &evaluator, PrecedenceGroupDescriptor descriptor) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Source location
  SourceLoc getNearestLoc() const;

  // Separate caching.
  bool isCached() const { return true; }
};

/// Computes whether all of the stored properties in a nominal type have initial
/// values.
class AreAllStoredPropertiesDefaultInitableRequest
    : public SimpleRequest<AreAllStoredPropertiesDefaultInitableRequest,
                           bool(NominalTypeDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Computes whether this type has a user-defined designated initializer. This
/// does not include a synthesized designated initializer used to satisfy a
/// conformance.
class HasUserDefinedDesignatedInitRequest
    : public SimpleRequest<HasUserDefinedDesignatedInitRequest,
                           bool(NominalTypeDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Checks whether this type has a synthesized memberwise initializer.
class HasMemberwiseInitRequest
    : public SimpleRequest<HasMemberwiseInitRequest, bool(StructDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, StructDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Synthesizes a memberwise initializer for a given type.
class SynthesizeMemberwiseInitRequest
    : public SimpleRequest<SynthesizeMemberwiseInitRequest,
                           ConstructorDecl *(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ConstructorDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Resolves the effective memberwise initializer for a given type.
///
/// An effective memberwise initializer is either a synthesized memberwise
/// initializer or a user-defined initializer with the same type.
///
/// See `NominalTypeDecl::getEffectiveMemberwiseInitializer` for details.
class ResolveEffectiveMemberwiseInitRequest
    : public SimpleRequest<ResolveEffectiveMemberwiseInitRequest,
                           ConstructorDecl *(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ConstructorDecl *evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Checks whether this type has a synthesized zero parameter default
/// initializer.
class HasDefaultInitRequest
    : public SimpleRequest<HasDefaultInitRequest, bool(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator,
                                NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Synthesizes a default initializer for a given type.
class SynthesizeDefaultInitRequest
    : public SimpleRequest<SynthesizeDefaultInitRequest,
                           ConstructorDecl *(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ConstructorDecl * evaluate(Evaluator &evaluator,
                             NominalTypeDecl *decl) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class CompareDeclSpecializationRequest
    : public SimpleRequest<CompareDeclSpecializationRequest,
                           bool(DeclContext *, ValueDecl *, ValueDecl *, bool),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, DeclContext *DC,
                ValueDecl *VD1, ValueDecl *VD2,
                bool dynamic) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Checks whether the first function decl is a refinement of the second,
/// meaning the two functions have the same structure, and the requirements
/// of the first are refining the requirements of the second.
class IsDeclRefinementOfRequest
    : public SimpleRequest<IsDeclRefinementOfRequest,
                           bool(ValueDecl *, ValueDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ValueDecl *declA, ValueDecl *declB) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Checks whether this declaration inherits its superclass' designated and
/// convenience initializers.
class InheritsSuperclassInitializersRequest
    : public SimpleRequest<InheritsSuperclassInitializersRequest,
                           bool(ClassDecl *), RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ClassDecl *decl) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

// The actions this request takes are all huge layering violations.
//
// Please do not add any more.
enum class ImplicitMemberAction : uint8_t {
  ResolveImplicitInit,
  ResolveCodingKeys,
  ResolveEncodable,
  ResolveDecodable,
  ResolveDistributedActor,
  ResolveDistributedActorID,
  ResolveDistributedActorSystem,
};

class ResolveImplicitMemberRequest
    : public SimpleRequest<ResolveImplicitMemberRequest,
                           evaluator::SideEffect(NominalTypeDecl *,
                                                 ImplicitMemberAction),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  evaluator::SideEffect
  evaluate(Evaluator &evaluator, NominalTypeDecl *NTD,
           ImplicitMemberAction action) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

class TypeWitnessRequest
    : public SimpleRequest<TypeWitnessRequest,
                           TypeWitnessAndDecl(NormalProtocolConformance *,
                                              AssociatedTypeDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  TypeWitnessAndDecl
  evaluate(Evaluator &evaluator, NormalProtocolConformance *conformance,
           AssociatedTypeDecl *ATD) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<TypeWitnessAndDecl> getCachedResult() const;
  void cacheResult(TypeWitnessAndDecl value) const;
};

class ValueWitnessRequest
    : public SimpleRequest<ValueWitnessRequest,
                           Witness(NormalProtocolConformance *, ValueDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Witness evaluate(Evaluator &evaluator,
                   NormalProtocolConformance *conformance,
                   ValueDecl *VD) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Witness> getCachedResult() const;
  void cacheResult(Witness value) const;
};

struct PreCheckResultBuilderDescriptor {
  AnyFunctionRef Fn;
  bool SuppressDiagnostics;

private:
  // NOTE: Since source tooling (e.g. code completion) might replace the body,
  // we need to take the body into account to calculate 'hash_value' and '=='.
  // Also, we cannot 'getBody()' inside 'hash_value' and '==' because it invokes
  // another request (even if it's cached).
  BraceStmt *Body;

public:
  PreCheckResultBuilderDescriptor(AnyFunctionRef Fn, bool suppressDiagnostics)
      : Fn(Fn), SuppressDiagnostics(suppressDiagnostics), Body(Fn.getBody()) {}

  friend llvm::hash_code
  hash_value(const PreCheckResultBuilderDescriptor &owner) {
    return llvm::hash_combine(owner.Fn, owner.Body);
  }

  friend bool operator==(const PreCheckResultBuilderDescriptor &lhs,
                         const PreCheckResultBuilderDescriptor &rhs) {
    return lhs.Fn == rhs.Fn && lhs.Body == rhs.Body;
  }

  friend bool operator!=(const PreCheckResultBuilderDescriptor &lhs,
                         const PreCheckResultBuilderDescriptor &rhs) {
    return !(lhs == rhs);
  }

  friend SourceLoc extractNearestSourceLoc(PreCheckResultBuilderDescriptor d) {
    return extractNearestSourceLoc(d.Fn);
  }

  friend void simple_display(llvm::raw_ostream &out,
                             const PreCheckResultBuilderDescriptor &d) {
    simple_display(out, d.Fn);
  }
};

enum class ResultBuilderBodyPreCheck : uint8_t {
  /// There were no problems pre-checking the closure.
  Okay,

  /// There was an error pre-checking the closure.
  Error,

  /// The closure has a return statement.
  HasReturnStmt,
};

class PreCheckResultBuilderRequest
    : public SimpleRequest<PreCheckResultBuilderRequest,
                           ResultBuilderBodyPreCheck(
                               PreCheckResultBuilderDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ResultBuilderBodyPreCheck
  evaluate(Evaluator &evaluator, PreCheckResultBuilderDescriptor owner) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

/// Computes whether a protocol has a circular reference in its list of
/// inherited protocols.
class HasCircularInheritedProtocolsRequest
    : public SimpleRequest<HasCircularInheritedProtocolsRequest,
                           bool(ProtocolDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, ProtocolDecl *decl) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Cached.
  bool isCached() const { return true; }
};

/// Computes whether an enum's raw value has a circular reference.
class HasCircularRawValueRequest
    : public SimpleRequest<HasCircularRawValueRequest, bool(EnumDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, EnumDecl *decl) const;

public:
  // Cycle handling.
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;

  // Cached.
  bool isCached() const { return true; }
};

/// Checks if the Distributed module is available.
class DistributedModuleIsAvailableRequest
    : public SimpleRequest<DistributedModuleIsAvailableRequest, bool(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, Decl *decl) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Computes an initializer context for a parameter with a default argument.
class DefaultArgumentInitContextRequest
    : public SimpleRequest<DefaultArgumentInitContextRequest,
                           Initializer *(ParamDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Initializer * evaluate(Evaluator &evaluator,
                                         ParamDecl *param) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Initializer *> getCachedResult() const;
  void cacheResult(Initializer *init) const;
};

/// Computes the fully type-checked default argument expression for a given
/// parameter.
class DefaultArgumentExprRequest
    : public SimpleRequest<DefaultArgumentExprRequest, Expr *(ParamDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Expr *evaluate(Evaluator &evaluator, ParamDecl *param) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Expr *> getCachedResult() const;
  void cacheResult(Expr *expr) const;
};

/// Computes the type of the default expression for a given parameter.
class DefaultArgumentTypeRequest
    : public SimpleRequest<DefaultArgumentTypeRequest, Type(ParamDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, ParamDecl *param) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type type) const;
};

/// Computes the fully type-checked caller-side default argument within the
/// context of the call site that it will be inserted into.
class CallerSideDefaultArgExprRequest
    : public SimpleRequest<CallerSideDefaultArgExprRequest,
                           Expr *(DefaultArgumentExpr *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Expr * evaluate(Evaluator &evaluator, DefaultArgumentExpr *defaultExpr) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Expr *> getCachedResult() const;
  void cacheResult(Expr *expr) const;
};

class DynamicallyReplacedDeclRequest
    : public SimpleRequest<DynamicallyReplacedDeclRequest,
                           ValueDecl *(ValueDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ValueDecl * evaluate(Evaluator &evaluator, ValueDecl *VD) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

class SpecializeAttrTargetDeclRequest
    : public SimpleRequest<SpecializeAttrTargetDeclRequest,
                           ValueDecl *(const ValueDecl *, SpecializeAttr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ValueDecl *evaluate(Evaluator &evaluator, const ValueDecl *vd,
                      SpecializeAttr *attr) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Looks up and applies the access note for a given declaration.
class ApplyAccessNoteRequest
    : public SimpleRequest<ApplyAccessNoteRequest,
                           evaluator::SideEffect(ValueDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  evaluator::SideEffect evaluate(Evaluator &evaluator, ValueDecl *VD) const;

public:
  // Cached.
  bool isCached() const { return true; }
};


class TypeCheckSourceFileRequest
    : public SimpleRequest<
          TypeCheckSourceFileRequest, evaluator::SideEffect(SourceFile *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySource> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  evaluator::SideEffect
  evaluate(Evaluator &evaluator, SourceFile *SF) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<evaluator::SideEffect> getCachedResult() const;
  void cacheResult(evaluator::SideEffect) const;

public:
  // Incremental dependencies.
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &) const;
};

/// Determines the type of a given pattern.
///
/// Note that this returns the "raw" pattern type, which can involve
/// unresolved types and unbound generic types where type inference is
/// allowed.
class PatternTypeRequest
    : public SimpleRequest<PatternTypeRequest, Type(ContextualPattern),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, ContextualPattern pattern) const;

public:
  bool isCached() const { return true; }

  SourceLoc getNearestLoc() const {
    return std::get<0>(getStorage()).getPattern()->getLoc();
  }
};

/// List SPI group ids declared on a decl.
class SPIGroupsRequest :
    public SimpleRequest<SPIGroupsRequest,
                         llvm::ArrayRef<Identifier>(const Decl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::ArrayRef<Identifier>
  evaluate(Evaluator &evaluator, const Decl *decl) const;

public:
  bool isCached() const { return true; }
};


/// Type-checks a `@differentiable` attribute and returns the resolved parameter
/// indices on success. On failure, emits diagnostics and returns `nullptr`.
///
/// Currently, this request resolves other `@differentiable` attribute
/// components but mutates them in place:
/// - `JVPFunction`
/// - `VJPFunction`
/// - `DerivativeGenericSignature`
class DifferentiableAttributeTypeCheckRequest
    : public SimpleRequest<DifferentiableAttributeTypeCheckRequest,
                           IndexSubset *(DifferentiableAttr *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  IndexSubset * evaluate(Evaluator &evaluator,
                         DifferentiableAttr *attr) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<IndexSubset *> getCachedResult() const;
  void cacheResult(IndexSubset *value) const;
};

/// Resolves the referenced original declaration for a `@derivative` attribute.
class DerivativeAttrOriginalDeclRequest
    : public SimpleRequest<DerivativeAttrOriginalDeclRequest,
                           AbstractFunctionDecl *(DerivativeAttr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  AbstractFunctionDecl *evaluate(Evaluator &evaluator,
                                 DerivativeAttr *attr) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Resolves the "tangent stored property" corresponding to an original stored
/// property in a `Differentiable`-conforming type.
class TangentStoredPropertyRequest
    : public SimpleRequest<TangentStoredPropertyRequest,
                           TangentPropertyInfo(VarDecl *, CanType),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  TangentPropertyInfo evaluate(Evaluator &evaluator, VarDecl *originalField,
                               CanType parentType) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

/// Checks whether a type eraser has a viable initializer.
class TypeEraserHasViableInitRequest
    : public SimpleRequest<TypeEraserHasViableInitRequest,
                           bool(TypeEraserAttr *, ProtocolDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation
  bool evaluate(Evaluator &evaluator, TypeEraserAttr *attr,
                ProtocolDecl *protocol) const;

public:
  bool isCached() const { return true; }
};

/// Looks up the decls that a scoped import references, ensuring the import is
/// valid.
///
/// A "scoped import" is an import which only covers one particular
/// declaration, such as:
///
///     import class Foundation.NSString
///
class ScopedImportLookupRequest
    : public SimpleRequest<ScopedImportLookupRequest,
                           ArrayRef<ValueDecl *>(ImportDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<ValueDecl *>
  evaluate(Evaluator &evaluator, ImportDecl *import) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Determine whether closure body has any explicit `return`
/// statements which could produce a non-void result.
class ClosureHasExplicitResultRequest
    : public SimpleRequest<ClosureHasExplicitResultRequest, bool(ClosureExpr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, ClosureExpr *closure) const;

public:
  bool isCached() const { return true; }
};

using ProtocolConformanceLookupResult = std::vector<ProtocolConformance *>;
void simple_display(llvm::raw_ostream &out, ConformanceLookupKind kind);

/// Lookup and expand all conformances in the given context.
///
/// This request specifically accommodates algorithms for retrieving all
/// conformances in the primary, even those that are unstated in source but
/// are implied by other conformances, inherited from other types, or synthesized
/// by the compiler. A simple case of this is the following:
///
/// \code
/// protocol P {}
/// protocol Q : P {}
/// extension T : Q {}
/// \endcode
///
/// Here, a conformance to \c Q has been stated, but a conformance to \c P
/// must also be reported so it can be checked as well.
class LookupAllConformancesInContextRequest
    : public SimpleRequest<LookupAllConformancesInContextRequest,
                           ProtocolConformanceLookupResult(
                               const IterableDeclContext *),
                           RequestFlags::Cached |
                               RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ProtocolConformanceLookupResult
  evaluate(Evaluator &evaluator, const IterableDeclContext *IDC) const;

public:
  bool isCached() const { return true; }

  // Incremental dependencies
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           const ProtocolConformanceLookupResult &r) const;
};

class CheckRedeclarationRequest
    : public SimpleRequest<
          CheckRedeclarationRequest,
          evaluator::SideEffect(ValueDecl *, NominalTypeDecl *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySource |
              RequestFlags::DependencySink> {
public:
  using SimpleRequest::SimpleRequest;

 private:
  friend SimpleRequest;

   // Evaluation.
  /// \p SelfNominalType is \c VD->getDeclContext()->getSelfNominalType().
  /// Passed as a parameter in here so this request doesn't tigger self nominal
  /// type computation.
  evaluator::SideEffect evaluate(Evaluator &evaluator, ValueDecl *VD,
                                 NominalTypeDecl *SelfNominalType) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<evaluator::SideEffect> getCachedResult() const;
  void cacheResult(evaluator::SideEffect) const;

public:
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &eval) const;
  void writeDependencySink(evaluator::DependencyCollector &tracker,
                           evaluator::SideEffect) const;
};

class UnresolvedMacroReference {
private:
  llvm::PointerUnion<FreestandingMacroExpansion *, CustomAttr *>
    pointer;

public:
  UnresolvedMacroReference(FreestandingMacroExpansion *exp) : pointer(exp) {}
  UnresolvedMacroReference(CustomAttr *attr) : pointer(attr) {}
  UnresolvedMacroReference(
      llvm::PointerUnion<FreestandingMacroExpansion *, CustomAttr *> pointer)
      : pointer(pointer) {}

  FreestandingMacroExpansion *getFreestanding() const {
    return pointer.dyn_cast<FreestandingMacroExpansion *>();
  }
  CustomAttr *getAttr() const {
    return pointer.dyn_cast<CustomAttr *>();
  }
  void *getOpaqueValue() const {
    return pointer.getOpaqueValue();
  }

  SourceLoc getSigilLoc() const;
  DeclNameRef getMacroName() const;
  DeclNameLoc getMacroNameLoc() const;
  SourceRange getGenericArgsRange() const;
  ArrayRef<TypeRepr *> getGenericArgs() const;
  ArgumentList *getArgs() const;

  /// Returns the macro roles corresponding to this macro reference.
  MacroRoles getMacroRoles() const;

  friend bool operator==(const UnresolvedMacroReference &lhs,
                         const UnresolvedMacroReference &rhs) {
    return lhs.getOpaqueValue() == rhs.getOpaqueValue();
  }

  friend llvm::hash_code hash_value(const UnresolvedMacroReference &ref) {
    return reinterpret_cast<ptrdiff_t>(ref.pointer.getOpaqueValue());
  }

  friend SourceLoc extractNearestSourceLoc(
      const UnresolvedMacroReference &ref
  ) {
    return ref.getSigilLoc();
  }
};

void simple_display(llvm::raw_ostream &out,
                    const UnresolvedMacroReference &ref);

/// Resolve a given custom attribute to an attached macro declaration.
class ResolveMacroRequest
    : public SimpleRequest<ResolveMacroRequest,
                           ConcreteDeclRef(UnresolvedMacroReference,
                                           DeclContext *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ConcreteDeclRef evaluate(Evaluator &evaluator,
                           UnresolvedMacroReference macroRef,
                           DeclContext *decl) const;

public:
  bool isCached() const { return true; }

  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

class ResolveTypeEraserTypeRequest
    : public SimpleRequest<ResolveTypeEraserTypeRequest,
                           Type (ProtocolDecl *, TypeEraserAttr *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, ProtocolDecl *PD,
                TypeEraserAttr *attr) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

/// Determines whether this is a "simple" didSet i.e one that either does not
/// use the implicit oldValue parameter in the body or does not take an explicit
/// parameter (ex: 'didSet(oldValue)').
class SimpleDidSetRequest
    : public SimpleRequest<SimpleDidSetRequest, bool(AccessorDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AccessorDecl *decl) const;

public:
  bool isCached() const {
    return std::get<0>(getStorage())->getAccessorKind() == AccessorKind::DidSet;
  }
};

/// Computes the loaded modules that should be implicitly imported by each file
/// of a given module.
class ModuleImplicitImportsRequest
    : public SimpleRequest<ModuleImplicitImportsRequest,
                           ImplicitImportList(ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ImplicitImportList evaluate(Evaluator &evaluator, ModuleDecl *module) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Checks whether a file contains any import declarations with the given flag.
class HasImportsMatchingFlagRequest
    : public SimpleRequest<HasImportsMatchingFlagRequest,
                           bool(SourceFile *, ImportFlags),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, SourceFile *SF, ImportFlags flag) const;

public:
  // Cached.
  bool isCached() const { return true; }
  llvm::Optional<bool> getCachedResult() const;
  void cacheResult(bool value) const;
};

/// Get the library level of a module.
class ModuleLibraryLevelRequest
    : public SimpleRequest<ModuleLibraryLevelRequest,
                           LibraryLevel(const ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  LibraryLevel evaluate(Evaluator &evaluator, const ModuleDecl *module) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

class ResolveTypeRequest
    : public SimpleRequest<ResolveTypeRequest,
                           Type(const TypeResolution *, TypeRepr *),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

public:
  // Cycle handling.
  void noteCycleStep(DiagnosticEngine &diags) const;

  static Type evaluate(const TypeResolution &resolution,
                       TypeRepr *repr);

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, const TypeResolution *resolution,
                TypeRepr *repr) const;
};

void simple_display(llvm::raw_ostream &out, const TypeResolution *resolution);
SourceLoc extractNearestSourceLoc(const TypeRepr *repr);

/// Checks to see if any of the imports in a module use \c @_implementationOnly
/// in one file and not in another.
///
/// Like redeclaration checking, but for imports.
///
/// This is a request purely to ensure that we don't need to perform the same
/// checking for each file we resolve imports for.
/// FIXME: Once import resolution operates at module-level, this checking can
/// be integrated into it.
class CheckInconsistentImplementationOnlyImportsRequest
    : public SimpleRequest<CheckInconsistentImplementationOnlyImportsRequest,
                           evaluator::SideEffect(ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  evaluator::SideEffect evaluate(Evaluator &evaluator, ModuleDecl *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Check that if one import in a file uses \c @_spiOnly, all imports from the
/// same file are consistently using \c @_spiOnly.
class CheckInconsistentSPIOnlyImportsRequest
    : public SimpleRequest<CheckInconsistentSPIOnlyImportsRequest,
                           evaluator::SideEffect(SourceFile *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  evaluator::SideEffect evaluate(Evaluator &evaluator, SourceFile *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Report default imports if other imports of the same target from this
/// module have an explicitly defined access level. In such a case, all imports
/// of the target module need an explicit access level or it may be made public
/// by error. This applies only to pre-Swift 6 mode.
class CheckInconsistentAccessLevelOnImport
    : public SimpleRequest<CheckInconsistentAccessLevelOnImport,
                           evaluator::SideEffect(SourceFile *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  evaluator::SideEffect evaluate(Evaluator &evaluator, SourceFile *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Checks to see if any of the imports in a module use \c @_weakLinked
/// in one file and not in another.
///
/// Like redeclaration checking, but for imports.
///
/// This is a request purely to ensure that we don't need to perform the same
/// checking for each file we resolve imports for.
/// FIXME: Once import resolution operates at module-level, this checking can
/// be integrated into it.
class CheckInconsistentWeakLinkedImportsRequest
    : public SimpleRequest<CheckInconsistentWeakLinkedImportsRequest,
                           evaluator::SideEffect(ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  evaluator::SideEffect evaluate(Evaluator &evaluator, ModuleDecl *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Retrieves the primary source files in the main module.
// FIXME: This isn't really a type-checking request, if we ever split off a
// zone for more basic AST requests, this should be moved there.
class PrimarySourceFilesRequest
    : public SimpleRequest<PrimarySourceFilesRequest,
                           ArrayRef<SourceFile *>(ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<SourceFile *> evaluate(Evaluator &evaluator, ModuleDecl *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Retrieve the file being used for code completion in the main module.
// FIXME: This isn't really a type-checking request, if we ever split off a
// zone for more basic AST requests, this should be moved there.
class IDEInspectionFileRequest
    : public SimpleRequest<IDEInspectionFileRequest,
                           SourceFile *(ModuleDecl *), RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  SourceFile *evaluate(Evaluator &evaluator, ModuleDecl *mod) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Kinds of types for CustomAttr.
enum class CustomAttrTypeKind {
  /// The type is required to not be expressed in terms of
  /// any contextual type parameters.
  NonGeneric,

  /// Property wrappers have some funky rules, like allowing
  /// unbound generic types.
  PropertyWrapper,

  /// Global actors are represented as custom type attributes. They don't
  /// have any particularly interesting semantics.
  GlobalActor,

  /// Attributes that are discoverable/constructable at runtime given a name.
  /// They allow unbound generic types.
  RuntimeMetadata,
};

void simple_display(llvm::raw_ostream &out, CustomAttrTypeKind value);

class CustomAttrTypeRequest
    : public SimpleRequest<CustomAttrTypeRequest,
                           Type(CustomAttr *, DeclContext *,
                                CustomAttrTypeKind),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  Type evaluate(Evaluator &evaluator, CustomAttr *, DeclContext *,
                CustomAttrTypeKind) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
  llvm::Optional<Type> getCachedResult() const;
  void cacheResult(Type value) const;
};

class SynthesizeMainFunctionRequest
    : public SimpleRequest<SynthesizeMainFunctionRequest,
                           FuncDecl *(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  FuncDecl *evaluate(Evaluator &evaluator, Decl *) const;

public:
  bool isCached() const { return true; }
};

/// Retrieve the implicit conformance for the given nominal type to
/// the Sendable protocol.
class GetImplicitSendableRequest :
    public SimpleRequest<GetImplicitSendableRequest,
                         ProtocolConformance *(NominalTypeDecl *),
                         RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ProtocolConformance *evaluate(
      Evaluator &evaluator, NominalTypeDecl *nominal) const;

public:
  // Caching
  bool isCached() const { return true; }
};

class ConditionalRequirementsRequest
    : public SimpleRequest<ConditionalRequirementsRequest,
                           llvm::ArrayRef<Requirement>(
                               NormalProtocolConformance *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  llvm::ArrayRef<Requirement> evaluate(Evaluator &evaluator,
                                       NormalProtocolConformance *decl) const;

public:
  bool isCached() const { return true; }
};

class RenamedDeclRequest
    : public SimpleRequest<RenamedDeclRequest,
                           ValueDecl *(const ValueDecl *, const AvailableAttr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ValueDecl *evaluate(Evaluator &evaluator, const ValueDecl *attached,
                      const AvailableAttr *attr) const;

public:
  bool isCached() const { return true; }
};

using AvailableAttrDeclPair = std::pair<const AvailableAttr *, const Decl *>;

class SemanticAvailableRangeAttrRequest
    : public SimpleRequest<SemanticAvailableRangeAttrRequest,
                           llvm::Optional<AvailableAttrDeclPair>(const Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  llvm::Optional<AvailableAttrDeclPair> evaluate(Evaluator &evaluator,
                                                 const Decl *decl) const;

public:
  bool isCached() const { return true; }
};

class SemanticUnavailableAttrRequest
    : public SimpleRequest<SemanticUnavailableAttrRequest,
                           llvm::Optional<AvailableAttrDeclPair>(const Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  llvm::Optional<AvailableAttrDeclPair> evaluate(Evaluator &evaluator,
                                                 const Decl *decl) const;

public:
  bool isCached() const { return true; }
};

class ClosureEffectsRequest
    : public SimpleRequest<ClosureEffectsRequest,
                           FunctionType::ExtInfo(ClosureExpr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  FunctionType::ExtInfo evaluate(
      Evaluator &evaluator, ClosureExpr *closure) const;

public:
  bool isCached() const { return true; }
};

class GetSourceFileAsyncNode
    : public SimpleRequest<GetSourceFileAsyncNode, ASTNode(const SourceFile *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ASTNode evaluate(Evaluator &evaluator, const SourceFile *) const;

public:
  bool isCached() const { return true; }
};

/// Lookup the target of a break statement.
class BreakTargetRequest
    : public SimpleRequest<BreakTargetRequest,
                           LabeledStmt *(const BreakStmt *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  LabeledStmt *evaluate(Evaluator &evaluator, const BreakStmt *BS) const;

public:
  bool isCached() const { return true; }
};

/// Lookup the target of a continue statement.
class ContinueTargetRequest
    : public SimpleRequest<ContinueTargetRequest,
                           LabeledStmt *(const ContinueStmt *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  LabeledStmt *evaluate(Evaluator &evaluator, const ContinueStmt *CS) const;

public:
  bool isCached() const { return true; }
};

/// Precheck a ReturnStmt, which involves some initial validation, as well as
/// applying a conversion to a FailStmt if needed.
class PreCheckReturnStmtRequest
    : public SimpleRequest<PreCheckReturnStmtRequest,
                           Stmt *(ReturnStmt *, DeclContext *DC),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  Stmt *evaluate(Evaluator &evaluator, ReturnStmt *RS, DeclContext *DC) const;

public:
  bool isCached() const { return true; }
};

/// The result of the query for whether a statement can produce a single value.
class IsSingleValueStmtResult {
public:
  enum class Kind {
    /// The statement may become a SingleValueStmtExpr.
    Valid,

    /// There are non-single-expression branches that do not end in a throw.
    UnterminatedBranches,

    /// The statement is an 'if' statement without an unconditional 'else'.
    NonExhaustiveIf,

    /// There are no single-expression branches.
    NoExpressionBranches,

    /// There is an unhandled statement branch. This should only be the case
    /// for invalid AST.
    UnhandledStmt,

    /// There was a circular reference when evaluating the request. This can be
    /// ignored, as we will have already diagnosed it.
    CircularReference,

    /// There is a 'break' or 'continue' within the statement that prevents it
    /// from being treated as an expression.
    InvalidJumps,

    /// The statement has a jump label, which is invalid for an expression.
    HasLabel
  };

private:
  Kind TheKind;
  TinyPtrVector<Stmt *> InvalidJumps;
  TinyPtrVector<Stmt *> UnterminatedBranches;

  IsSingleValueStmtResult(Kind kind) : TheKind(kind) {
    assert(kind != Kind::UnterminatedBranches && kind != Kind::InvalidJumps);
  }

  IsSingleValueStmtResult(Kind kind, TinyPtrVector<Stmt *> stmts)
      : TheKind(kind) {
    switch (kind) {
    case Kind::UnterminatedBranches: {
      UnterminatedBranches = std::move(stmts);
      break;
    }
    case Kind::InvalidJumps: {
      InvalidJumps = std::move(stmts);
      break;
    }
    default:
      llvm_unreachable("Unhandled case in switch!");
    }
  }

public:
  static IsSingleValueStmtResult valid() {
    return IsSingleValueStmtResult(Kind::Valid);
  }
  static IsSingleValueStmtResult
  unterminatedBranches(TinyPtrVector<Stmt *> branches) {
    return IsSingleValueStmtResult(Kind::UnterminatedBranches,
                                   std::move(branches));
  }
  static IsSingleValueStmtResult nonExhaustiveIf() {
    return IsSingleValueStmtResult(Kind::NonExhaustiveIf);
  }
  static IsSingleValueStmtResult noExpressionBranches() {
    return IsSingleValueStmtResult(Kind::NoExpressionBranches);
  }
  static IsSingleValueStmtResult unhandledStmt() {
    return IsSingleValueStmtResult(Kind::UnhandledStmt);
  }
  static IsSingleValueStmtResult circularReference() {
    return IsSingleValueStmtResult(Kind::CircularReference);
  }
  static IsSingleValueStmtResult invalidJumps(TinyPtrVector<Stmt *> jumps) {
    return IsSingleValueStmtResult(Kind::InvalidJumps, std::move(jumps));
  }
  static IsSingleValueStmtResult hasLabel() {
    return IsSingleValueStmtResult(Kind::HasLabel);
  }

  Kind getKind() const { return TheKind; }

  /// For an unterminated branch kind, retrieves the branch.
  const TinyPtrVector<Stmt *> &getUnterminatedBranches() const {
    assert(TheKind == Kind::UnterminatedBranches);
    return UnterminatedBranches;
  }

  /// For an invalid jump kind, retrieves the list of invalid jumps.
  const TinyPtrVector<Stmt *> &getInvalidJumps() const {
    assert(TheKind == Kind::InvalidJumps);
    return InvalidJumps;
  }

  explicit operator bool() const {
    return TheKind == Kind::Valid;
  }
};

/// Computes whether a given statement can be treated as a SingleValueStmtExpr.
class IsSingleValueStmtRequest
    : public SimpleRequest<IsSingleValueStmtRequest,
                           IsSingleValueStmtResult(const Stmt *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  IsSingleValueStmtResult
  evaluate(Evaluator &evaluator, const Stmt *stmt) const;

public:
  bool isCached() const { return true; }
};

/// Find the definition of a given macro.
class MacroDefinitionRequest
    : public SimpleRequest<MacroDefinitionRequest,
                           MacroDefinition(MacroDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  MacroDefinition evaluate(Evaluator &evaluator, MacroDecl *macro) const;

public:
  // Source location
  SourceLoc getNearestLoc() const;

  bool isCached() const { return true; }
};

/// Find the definition of a given macro.
class ExpandMacroExpansionDeclRequest
    : public SimpleRequest<ExpandMacroExpansionDeclRequest,
                           llvm::Optional<unsigned>(MacroExpansionDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  llvm::Optional<unsigned> evaluate(Evaluator &evaluator,
                                    MacroExpansionDecl *med) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand a 'MacroExpansionExpr',
class ExpandMacroExpansionExprRequest
    : public SimpleRequest<ExpandMacroExpansionExprRequest,
                           llvm::Optional<unsigned>(MacroExpansionExpr *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  llvm::Optional<unsigned> evaluate(Evaluator &evaluator,
                                    MacroExpansionExpr *mee) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand all accessor macros attached to the given declaration.
///
/// Produces the set of macro expansion buffer IDs.
class ExpandAccessorMacros
    : public SimpleRequest<ExpandAccessorMacros,
                           ArrayRef<unsigned>(AbstractStorageDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(
      Evaluator &evaluator, AbstractStorageDecl *storage) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand all conformance macros attached to the given declaration.
///
/// Produces the set of macro expansion buffer IDs.
class ExpandConformanceMacros
    : public SimpleRequest<ExpandConformanceMacros,
                           ArrayRef<unsigned>(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(Evaluator &evaluator,
                              NominalTypeDecl *nominal) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand all extension macros attached to the given declaration.
///
/// Produces the set of macro expansion buffer IDs.
class ExpandExtensionMacros
    : public SimpleRequest<ExpandExtensionMacros,
                           ArrayRef<unsigned>(NominalTypeDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(Evaluator &evaluator,
                              NominalTypeDecl *nominal) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand all member attribute macros attached to the given
/// declaration.
///
/// Produces the set of macro expansion buffer IDs.
class ExpandMemberAttributeMacros
    : public SimpleRequest<ExpandMemberAttributeMacros,
                           ArrayRef<unsigned>(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(Evaluator &evaluator, Decl *decl) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Expand synthesized member macros attached to the given declaration.
///
/// Produces the set of macro expansion buffer IDs.
class ExpandSynthesizedMemberMacroRequest
    : public SimpleRequest<ExpandSynthesizedMemberMacroRequest,
                           ArrayRef<unsigned>(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(Evaluator &evaluator, Decl *decl) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Represent a loaded plugin either an in-process library or an executable.
class LoadedCompilerPlugin {
  llvm::PointerUnion<LoadedLibraryPlugin *, LoadedExecutablePlugin *> ptr;

public:
  LoadedCompilerPlugin(std::nullptr_t) : ptr(nullptr) {}
  LoadedCompilerPlugin(LoadedLibraryPlugin *ptr) : ptr(ptr){};
  LoadedCompilerPlugin(LoadedExecutablePlugin *ptr) : ptr(ptr){};

  LoadedLibraryPlugin *getAsLibraryPlugin() const {
    return ptr.dyn_cast<LoadedLibraryPlugin *>();
  }
  LoadedExecutablePlugin *getAsExecutablePlugin() const {
    return ptr.dyn_cast<LoadedExecutablePlugin *>();
  }
};

class CompilerPluginLoadRequest
    : public SimpleRequest<CompilerPluginLoadRequest,
                           LoadedCompilerPlugin(ASTContext *, Identifier),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  LoadedCompilerPlugin evaluate(Evaluator &evaluator, ASTContext *ctx,
                                Identifier moduleName) const;

public:
  // Source location
  SourceLoc getNearestLoc() const { return SourceLoc(); }

  bool isCached() const { return true; }
};

/// Expand peer macros attached to the given declaration.
class ExpandPeerMacroRequest
    : public SimpleRequest<ExpandPeerMacroRequest,
                           ArrayRef<unsigned>(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<unsigned> evaluate(Evaluator &evaluator, Decl *decl) const;

public:
  bool isCached() const { return true; }
  void diagnoseCycle(DiagnosticEngine &diags) const;
  void noteCycleStep(DiagnosticEngine &diags) const;
};

/// Resolve an external macro given its module and type name.
class ExternalMacroDefinitionRequest
    : public SimpleRequest<ExternalMacroDefinitionRequest,
                           llvm::Optional<ExternalMacroDefinition>(
                               ASTContext *, Identifier, Identifier),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  llvm::Optional<ExternalMacroDefinition> evaluate(
      Evaluator &evaluator, ASTContext *ctx,
      Identifier moduleName, Identifier typeName
  ) const;

public:
  // Source location
  SourceLoc getNearestLoc() const { return SourceLoc(); }

  bool isCached() const { return true; }
};

class GetRuntimeDiscoverableAttributes
    : public SimpleRequest<GetRuntimeDiscoverableAttributes,
                           ArrayRef<CustomAttr *>(Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<CustomAttr *> evaluate(Evaluator &evaluator, Decl *decl) const;

public:
  bool isCached() const { return true; }
};

class SynthesizeRuntimeMetadataAttrGenerator
    : public SimpleRequest<SynthesizeRuntimeMetadataAttrGenerator,
                           Expr *(CustomAttr *, ValueDecl *),
                           RequestFlags::Cached> {

public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  Expr *evaluate(Evaluator &evaluator, CustomAttr *attr,
                 ValueDecl *attachedTo) const;

public:
  bool isCached() const { return true; }
};

class SynthesizeRuntimeMetadataAttrGeneratorBody
    : public SimpleRequest<SynthesizeRuntimeMetadataAttrGeneratorBody,
                           BraceStmt *(CustomAttr *, ValueDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  BraceStmt *evaluate(Evaluator &evaluator, CustomAttr *attr,
                      ValueDecl *attachedTo) const;

public:
  bool isCached() const { return true; }
};

/// Compute the local discriminators for the given declaration context.
///
/// This is a state-changing operation for closures within the context, which
/// produces the discriminator value that any subsequent requests should use.
class LocalDiscriminatorsRequest
    : public SimpleRequest<LocalDiscriminatorsRequest,
                           unsigned(DeclContext *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  unsigned evaluate(Evaluator &evaluator, DeclContext *dc) const;

public:
  bool isCached() const { return true; }
};

/// Retrieve the raw documentation comment of a declaration.
class RawCommentRequest
    : public SimpleRequest<RawCommentRequest,
                           RawComment(const Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  RawComment evaluate(Evaluator &evaluator, const Decl *D) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

/// Retrieve the brief portion of a declaration's document comment, potentially
/// walking to find the comment providing decl if needed.
class SemanticBriefCommentRequest
    : public SimpleRequest<SemanticBriefCommentRequest,
                           StringRef(const Decl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  StringRef evaluate(Evaluator &evaluator, const Decl *D) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

/// Checks that all of a class's \c \@objcImplementation extensions provide
/// complete and correct implementations for their corresponding interfaces.
/// This is done on all of a class's implementations at once to improve diagnostics.
class TypeCheckObjCImplementationRequest
    : public SimpleRequest<TypeCheckObjCImplementationRequest,
                           evaluator::SideEffect(ExtensionDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  evaluator::SideEffect
  evaluate(Evaluator &evaluator, ExtensionDecl *ED) const;

public:
  // Separate caching.
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out, ASTNode node);
void simple_display(llvm::raw_ostream &out, Type value);
void simple_display(llvm::raw_ostream &out, const TypeRepr *TyR);
void simple_display(llvm::raw_ostream &out, ImplicitMemberAction action);
void simple_display(llvm::raw_ostream &out, ResultBuilderBodyPreCheck pck);

/// Computes whether a module is part of the stdlib or contained within the
/// SDK. If no SDK was specified, falls back to whether the module was
/// specified as a system module (ie. it's on the system search path).
class IsNonUserModuleRequest
    : public SimpleRequest<IsNonUserModuleRequest,
                           bool(ModuleDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  bool evaluate(Evaluator &evaluator, ModuleDecl *mod) const;

public:
  bool isCached() const { return true; }
};

class HasInitAccessorRequest
    : public SimpleRequest<HasInitAccessorRequest, bool(AbstractStorageDecl *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, AbstractStorageDecl *decl) const;

public:
  bool isCached() const { return true; }
};

class InitAccessorReferencedVariablesRequest
    : public SimpleRequest<InitAccessorReferencedVariablesRequest,
                           ArrayRef<VarDecl *>(DeclAttribute *, AccessorDecl *,
                                               ArrayRef<Identifier>),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  ArrayRef<VarDecl *> evaluate(Evaluator &evaluator, DeclAttribute *attr,
                               AccessorDecl *attachedTo,
                               ArrayRef<Identifier>) const;

public:
  bool isCached() const { return true; }
};

#define SWIFT_TYPEID_ZONE TypeChecker
#define SWIFT_TYPEID_HEADER "swift/AST/TypeCheckerTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

// Set up reporting of evaluated requests.
#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
  template<>                                                                   \
  inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,              \
                              const RequestType &request) {                    \
    ++stats.getFrontendCounters().RequestType;                                 \
  }
#include "swift/AST/TypeCheckerTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_TYPE_CHECK_REQUESTS_H

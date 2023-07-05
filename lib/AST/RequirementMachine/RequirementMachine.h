//===--- RequirementMachine.h - Generics with term rewriting ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REQUIREMENTMACHINE_H
#define SWIFT_REQUIREMENTMACHINE_H

#include "swift/AST/GenericSignature.h"
#include "swift/AST/RequirementSignature.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

#include "Diagnostics.h"
#include "PropertyMap.h"
#include "RewriteContext.h"
#include "RewriteSystem.h"

namespace llvm {
class raw_ostream;
}

namespace swift {

class AbstractGenericSignatureRequest;
class ASTContext;
class AssociatedTypeDecl;
class CanType;
class GenericTypeParamType;
class InferredGenericSignatureRequest;
class LayoutConstraint;
class ProtocolDecl;
class Requirement;
class RequirementSignatureRequest;
class Type;
class UnifiedStatsReporter;

namespace rewriting {
class RewriteContext;

/// Wraps a rewrite system with higher-level operations in terms of
/// generic signatures and interface types.
class RequirementMachine final {
  friend class swift::ASTContext;
  friend class swift::rewriting::RewriteContext;
  friend class swift::RequirementSignatureRequest;
  friend class swift::AbstractGenericSignatureRequest;
  friend class swift::InferredGenericSignatureRequest;

  GenericSignature Sig;
  SmallVector<GenericTypeParamType *, 2> Params;

  RewriteContext &Context;
  RewriteSystem System;
  PropertyMap Map;

  bool Dump = false;
  bool Complete = false;

  /// Parameters to prevent runaway completion and property map construction.
  unsigned MaxRuleCount;
  unsigned MaxRuleLength;
  unsigned MaxConcreteNesting;

  UnifiedStatsReporter *Stats;

  /// All conformance paths computed so far.
  llvm::DenseMap<std::pair<Term, ProtocolDecl *>,
                 ConformancePath> ConformancePaths;

  /// Conformance access paths computed during the last round. All elements
  /// have the same length. If a conformance access path of greater length
  /// is requested, we refill CurrentConformancePaths with all paths of
  /// length N+1, and add them to the ConformancePaths map.
  std::vector<std::pair<Term, ConformancePath>> CurrentConformancePaths;

  explicit RequirementMachine(RewriteContext &rewriteCtx);

  RequirementMachine(const RequirementMachine &) = delete;
  RequirementMachine(RequirementMachine &&) = delete;
  RequirementMachine &operator=(const RequirementMachine &) = delete;
  RequirementMachine &operator=(RequirementMachine &&) = delete;

  void checkCompletionResult(CompletionResult result) const;

  std::pair<CompletionResult, unsigned>
  initWithProtocolSignatureRequirements(
      ArrayRef<const ProtocolDecl *> protos);

  std::pair<CompletionResult, unsigned>
  initWithGenericSignature(GenericSignature sig);

  std::pair<CompletionResult, unsigned>
  initWithProtocolWrittenRequirements(
      ArrayRef<const ProtocolDecl *> component,
      const llvm::DenseMap<const ProtocolDecl *,
                           SmallVector<StructuralRequirement, 4>> protos);

  std::pair<CompletionResult, unsigned>
  initWithWrittenRequirements(
      ArrayRef<GenericTypeParamType *> genericParams,
      ArrayRef<StructuralRequirement> requirements);

  bool isComplete() const;

  std::pair<CompletionResult, unsigned>
  computeCompletion(RewriteSystem::ValidityPolicy policy);

  void freeze();

  void computeRequirementDiagnostics(SmallVectorImpl<RequirementError> &errors,
                                     SourceLoc signatureLoc);

  MutableTerm getLongestValidPrefix(const MutableTerm &term) const;

  void buildRequirementsFromRules(
    ArrayRef<unsigned> requirementRules,
    ArrayRef<unsigned> typeAliasRules,
    ArrayRef<GenericTypeParamType *> genericParams,
    bool reconstituteSugar,
    std::vector<Requirement> &reqs,
    std::vector<ProtocolTypeAlias> &aliases) const;

  ArrayRef<GenericTypeParamType *> getGenericParams() const {
    return Params;
  }

public:
  ~RequirementMachine();

  // Generic signature queries. Generally you shouldn't have to construct a
  // RequirementMachine instance; instead, call the corresponding methods on
  // GenericSignature, which lazily create a RequirementMachine for you.
  GenericSignature::LocalRequirements getLocalRequirements(Type depType,
                      ArrayRef<GenericTypeParamType *> genericParams) const;
  bool requiresClass(Type depType) const;
  LayoutConstraint getLayoutConstraint(Type depType) const;
  bool requiresProtocol(Type depType, const ProtocolDecl *proto) const;
  GenericSignature::RequiredProtocols getRequiredProtocols(Type depType) const;
  Type getSuperclassBound(Type depType,
                          ArrayRef<GenericTypeParamType *> genericParams) const;
  bool isConcreteType(Type depType,
                      const ProtocolDecl *proto=nullptr) const;
  Type getConcreteType(Type depType,
                       ArrayRef<GenericTypeParamType *> genericParams,
                       const ProtocolDecl *proto=nullptr) const;
  bool areReducedTypeParametersEqual(Type depType1, Type depType2) const;
  bool isReducedType(Type type) const;
  Type getReducedType(Type type,
                      ArrayRef<GenericTypeParamType *> genericParams) const;
  bool isValidTypeParameter(Type type) const;
  ConformancePath getConformancePath(Type type, ProtocolDecl *protocol);
  TypeDecl *lookupNestedType(Type depType, Identifier name) const;

private:
  MutableTerm getReducedShapeTerm(Type type) const;

public:
  Type getReducedShape(Type type,
                       ArrayRef<GenericTypeParamType *> genericParams) const;

  bool haveSameShape(Type type1, Type type2) const;

  llvm::DenseMap<const ProtocolDecl *, RequirementSignature>
  computeMinimalProtocolRequirements();

  GenericSignature
  computeMinimalGenericSignature(bool reconstituteSugar);

  ArrayRef<Rule> getLocalRules() const;

  std::string getRuleAsStringForDiagnostics(unsigned ruleID) const;

  GenericSignatureErrors getErrors() const;

  void verify(const MutableTerm &term) const;
  void dump(llvm::raw_ostream &out) const;

  DebugOptions getDebugOptions() const { return Context.getDebugOptions(); }
};

} // end namespace rewriting

} // end namespace swift

#endif

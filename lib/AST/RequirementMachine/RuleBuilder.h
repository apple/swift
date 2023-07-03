//===--- RuleBuilder.h - Lowering desugared requirements to rules ---------===//
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

#ifndef SWIFT_RULEBUILDER_H
#define SWIFT_RULEBUILDER_H

#include "swift/AST/ASTContext.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>
#include "RewriteContext.h"
#include "Rule.h"
#include "Symbol.h"
#include "Term.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

class AssociatedTypeDecl;
class ProtocolDecl;
class ProtocolTypeAlias;
class Requirement;

namespace rewriting {

/// A utility class for building rewrite rules from the top-level requirements
/// of a generic signature.
///
/// This also collects requirements from the transitive closure of all protocols
/// appearing on the right hand side of conformance requirements.
struct RuleBuilder {
  RewriteContext &Context;

  /// The transitive closure of all protocols appearing on the right hand
  /// side of conformance requirements.
  llvm::DenseSet<const ProtocolDecl *> &ReferencedProtocols;

  /// A subset of the above in insertion order, consisting of the protocols
  /// whose rules we are going to import.
  ///
  /// If this is a rewrite system built from a generic signature, this vector
  /// contains all elements in the above set.
  ///
  /// If this is a rewrite system built from a strongly connected component
  /// of the protocol, this vector contains all elements in the above set
  /// except for the protocols belonging to the component representing the
  /// rewrite system itself; those protocols are added directly instead of
  /// being imported.
  std::vector<const ProtocolDecl *> ProtocolsToImport;

  /// The rules representing a complete rewrite system for the above vector,
  /// pulled in by collectRulesFromReferencedProtocols().
  std::vector<Rule> ImportedRules;

  /// New rules to add which will be marked 'permanent'. These are rules for
  /// introducing associated types, and relationships between layout,
  /// superclass and concrete type symbols. They are not eliminated by
  /// homotopy reduction, since they are always added when the rewrite system
  /// is built.
  std::vector<std::pair<MutableTerm, MutableTerm>> PermanentRules;

  /// New rules derived from requirements written by the user, which can be
  /// eliminated by homotopy reduction.
  std::vector<std::tuple<MutableTerm, MutableTerm, llvm::Optional<unsigned>>>
      RequirementRules;

  /// Requirements written in source code. The requirement ID in the above
  /// \c RequirementRules vector is an index into this array.
  std::vector<StructuralRequirement> WrittenRequirements;

  /// Enables debugging output. Controlled by the -dump-requirement-machine
  /// frontend flag.
  unsigned Dump : 1;

  /// Used to ensure the initWith*() methods are only called once.
  unsigned Initialized : 1;

  RuleBuilder(RewriteContext &ctx,
              llvm::DenseSet<const ProtocolDecl *> &referencedProtocols)
      : Context(ctx), ReferencedProtocols(referencedProtocols) {
    Dump = ctx.getASTContext().LangOpts.DumpRequirementMachine;
    Initialized = 0;
  }

  void initWithGenericSignatureRequirements(ArrayRef<Requirement> requirements);
  void initWithWrittenRequirements(ArrayRef<StructuralRequirement> requirements);
  void initWithProtocolSignatureRequirements(ArrayRef<const ProtocolDecl *> proto);
  void initWithProtocolWrittenRequirements(
      ArrayRef<const ProtocolDecl *> component,
      const llvm::DenseMap<const ProtocolDecl *,
                           SmallVector<StructuralRequirement, 4>> protos);
  void initWithConditionalRequirements(ArrayRef<Requirement> requirements,
                                       ArrayRef<Term> substitutions);
  void addReferencedProtocol(const ProtocolDecl *proto);
  void collectRulesFromReferencedProtocols();

private:
  void addPermanentProtocolRules(const ProtocolDecl *proto);
  void addAssociatedType(const AssociatedTypeDecl *type,
                         const ProtocolDecl *proto);
  void addRequirement(const Requirement &req, const ProtocolDecl *proto,
                      llvm::Optional<ArrayRef<Term>> substitutions = llvm::None,
                      llvm::Optional<unsigned> requirementID = llvm::None);
  void addRequirement(const StructuralRequirement &req,
                      const ProtocolDecl *proto);
  void addTypeAlias(const ProtocolTypeAlias &alias,
                    const ProtocolDecl *proto);
};

} // end namespace rewriting

} // end namespace swift

#endif

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

namespace llvm {
class raw_ostream;
}

namespace swift {

class ASTContext;
class AssociatedTypeDecl;
class CanType;
class LayoutConstraint;
class ProtocolDecl;
class Requirement;
class Type;

/// Wraps a rewrite system with higher-level operations in terms of
/// generic signatures and interface types.
class RequirementMachine final {
  friend class ASTContext;

  struct Implementation;

  ASTContext &Context;
  Implementation *Impl;

  explicit RequirementMachine(ASTContext &ctx);

  RequirementMachine(const RequirementMachine &) = delete;
  RequirementMachine(RequirementMachine &&) = delete;
  RequirementMachine &operator=(const RequirementMachine &) = delete;
  RequirementMachine &operator=(RequirementMachine &&) = delete;

  void addGenericSignature(CanGenericSignature sig);

  bool isComplete() const;
  void computeCompletion();

public:
  ~RequirementMachine();

  // Generic signature queries
  bool requiresClass(Type depType) const;
  LayoutConstraint getLayoutConstraint(Type depType) const;
  bool requiresProtocol(Type depType, const ProtocolDecl *proto) const;
  GenericSignature::RequiredProtocols getRequiredProtocols(Type depType) const;
  bool isConcreteType(Type depType) const;
  bool areSameTypeParameterInContext(Type depType1, Type depType2) const;

  void dump(llvm::raw_ostream &out) const;
};

} // end namespace swift

#endif
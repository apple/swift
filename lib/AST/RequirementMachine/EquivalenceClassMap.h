//===--- EquivalenceClassMap.h - Facts about generic parameters -*- C++ -*-===//
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
//
// A description of this data structure and its purpose can be found in
// EquivalenceClassMap.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_EQUIVALENCECLASSMAP_H
#define SWIFT_EQUIVALENCECLASSMAP_H

#include "swift/AST/LayoutConstraint.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <algorithm>
#include <memory>
#include <vector>

#include "ProtocolGraph.h"
#include "RewriteSystem.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

class ProtocolDecl;

namespace rewriting {

class MutableTerm;
class RewriteContext;
class Term;

/// Stores all rewrite rules of the form T.[p] => T, where [p] is a property atom,
/// for a single term 'T'.
class EquivalenceClass {
  friend class EquivalenceClassMap;

  /// The fully reduced term whose properties are recorded in this equivalence
  /// class.
  MutableTerm Key;

  /// All protocols this type conforms to.
  llvm::TinyPtrVector<const ProtocolDecl *> ConformsTo;

  /// The most specific layout constraint this type satisfies.
  LayoutConstraint Layout;

  /// The most specific superclass constraint this type satisfies.
  Optional<Atom> Superclass;

  /// The most specific concrete type constraint this type satisfies.
  Optional<Atom> ConcreteType;

  explicit EquivalenceClass(const MutableTerm &key) : Key(key) {}

  void addProperty(Atom property,
                   RewriteContext &ctx,
                   SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
                   bool debug);
  void copyPropertiesFrom(const EquivalenceClass *next,
                          RewriteContext &ctx);

  EquivalenceClass(const EquivalenceClass &) = delete;
  EquivalenceClass(EquivalenceClass &&) = delete;
  EquivalenceClass &operator=(const EquivalenceClass &) = delete;
  EquivalenceClass &operator=(EquivalenceClass &&) = delete;

public:
  const MutableTerm &getKey() const { return Key; }
  void dump(llvm::raw_ostream &out) const;

  bool hasSuperclassBound() const {
    return Superclass.hasValue();
  }

  bool isConcreteType() const {
    return ConcreteType.hasValue();
  }

  Type getConcreteType(
      TypeArrayView<GenericTypeParamType> genericParams,
      const ProtocolGraph &protos,
      RewriteContext &ctx) const;

  LayoutConstraint getLayoutConstraint() const {
    return Layout;
  }

  ArrayRef<const ProtocolDecl *> getConformsTo() const {
    return ConformsTo;
  }
};

/// Stores all rewrite rules of the form T.[p] => T, where [p] is a property
/// atom, for all terms 'T'.
///
/// Out-of-line methods are documented in EquivalenceClassMap.cpp.
class EquivalenceClassMap {
  RewriteContext &Context;
  std::vector<std::unique_ptr<EquivalenceClass>> Map;

  using ConcreteTypeInDomain = std::pair<CanType, ArrayRef<const ProtocolDecl *>>;
  llvm::DenseMap<ConcreteTypeInDomain, MutableTerm> ConcreteTypeInDomainMap;

  const ProtocolGraph &Protos;
  unsigned DebugConcreteUnification : 1;
  unsigned DebugConcretizeNestedTypes : 1;

  EquivalenceClass *getEquivalenceClassIfPresent(const MutableTerm &key) const;
  EquivalenceClass *getOrCreateEquivalenceClass(const MutableTerm &key);

  EquivalenceClassMap(const EquivalenceClassMap &) = delete;
  EquivalenceClassMap(EquivalenceClassMap &&) = delete;
  EquivalenceClassMap &operator=(const EquivalenceClassMap &) = delete;
  EquivalenceClassMap &operator=(EquivalenceClassMap &&) = delete;

public:
  explicit EquivalenceClassMap(RewriteContext &ctx,
                               const ProtocolGraph &protos)
      : Context(ctx), Protos(protos) {
    DebugConcreteUnification = false;
    DebugConcretizeNestedTypes = false;
  }

  EquivalenceClass *lookUpEquivalenceClass(const MutableTerm &key) const;

  void dump(llvm::raw_ostream &out) const;

  void clear();
  void addProperty(const MutableTerm &key, Atom property,
                   SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules);

  void computeConcreteTypeInDomainMap();
  void concretizeNestedTypesFromConcreteParents(
                   SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const;

private:
  void concretizeNestedTypesFromConcreteParent(
                   const MutableTerm &key,
                   CanType concreteType,
                   ArrayRef<Term> substitutions,
                   ArrayRef<const ProtocolDecl *> conformsTo,
                   SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const;

  MutableTerm computeConstraintTermForTypeWitness(
      const MutableTerm &key,
      CanType concreteType,
      CanType typeWitness,
      const MutableTerm &subjectType,
      ArrayRef<Term> substitutions) const;
};

} // end namespace rewriting

} // end namespace swift

#endif
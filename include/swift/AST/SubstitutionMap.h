//===--- SubstitutionMap.h - Swift Substitution Map ASTs --------*- C++ -*-===//
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
// This file defines the SubstitutionMap class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SUBSTITUTION_MAP_H
#define SWIFT_AST_SUBSTITUTION_MAP_H

#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeExpansionContext.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Optional.h"

namespace llvm {
  class FoldingSetNodeID;
}

namespace swift {

class GenericEnvironment;
class GenericParamList;
class SubstitutableType;
typedef CanTypeWrapper<GenericTypeParamType> CanGenericTypeParamType;

template<class Type> class CanTypeWrapper;
typedef CanTypeWrapper<SubstitutableType> CanSubstitutableType;

enum class CombineSubstitutionMaps {
  AtDepth,
  AtIndex
};

/// SubstitutionMap is a data structure type that describes the mapping of
/// abstract types to replacement types, together with associated conformances
/// to use for deriving nested types and conformances.
///
/// Substitution maps are primarily used when performing substitutions into
/// any entity that can reference type parameters, e.g., types (via
/// Type::subst()) and conformances (via ProtocolConformanceRef::subst()).
///
/// SubstitutionMaps are constructed by calling the an overload of the static
/// method \c SubstitutionMap::get(). However, most substitution maps are
/// computed using higher-level entry points such as
/// TypeBase::getContextSubstitutionMap().
///
/// Substitution maps are ASTContext-allocated and are uniqued on construction,
/// so they can be used as fields in AST nodes.
class SubstitutionMap {
public:
  /// Stored data for a substitution map, which uses tail allocation for the
  /// replacement types and conformances.
  class Storage;

private:
  /// The storage needed to describe the set of substitutions.
  ///
  /// When null, this substitution map is empty, having neither a generic
  /// signature nor any replacement types/conformances.
  Storage *storage = nullptr;

public:
  /// Retrieve the array of replacement types, which line up with the
  /// generic parameters.
  ///
  /// Note that the types may be null, for cases where the generic parameter
  /// is concrete but hasn't been queried yet.
  ///
  /// Prefer \c getReplacementTypes, this is public for printing purposes.
  ArrayRef<Type> getReplacementTypesBuffer() const;

private:
  MutableArrayRef<Type> getReplacementTypesBuffer();

  /// Retrieve a mutable reference to the buffer of conformances.
  MutableArrayRef<ProtocolConformanceRef> getConformancesBuffer();

  /// Form a substitution map for the given generic signature with the
  /// specified replacement types and conformances.
  SubstitutionMap(GenericSignature genericSig,
                  ArrayRef<Type> replacementTypes,
                  ArrayRef<ProtocolConformanceRef> conformances);

  explicit SubstitutionMap(Storage *storage) : storage(storage) { }

public:
  /// Build an empty substitution map.
  SubstitutionMap() { }

  /// Build an interface type substitution map for the given generic
  /// signature and a vector of Substitutions that correspond to the
  /// requirements of this generic signature.
  static SubstitutionMap get(GenericSignature genericSig,
                             ArrayRef<Type> replacementTypes,
                             ArrayRef<ProtocolConformanceRef> conformances) {
    return SubstitutionMap(genericSig, replacementTypes, conformances);
  }

  /// Build an interface type substitution map for the given generic
  /// signature using the mapping in the given substitutions.
  static SubstitutionMap get(GenericSignature genericSig,
                             SubstitutionMap substitutions);

  /// Build an interface type substitution map for the given generic signature
  /// from a type substitution function and conformance lookup function.
  static SubstitutionMap get(GenericSignature genericSig,
                             TypeSubstitutionFn subs,
                             LookupConformanceFn lookupConformance);

  /// Build a substitution map from the substitutions represented by
  /// the given in-flight substitution.
  ///
  /// This function should generally only be used by the substitution
  /// subsystem.
  static SubstitutionMap get(GenericSignature genericSig,
                             InFlightSubstitution &IFS);

  /// Retrieve the generic signature describing the environment in which
  /// substitutions occur.
  GenericSignature getGenericSignature() const;

  /// Retrieve the array of protocol conformances, which line up with the
  /// requirements of the generic signature.
  ArrayRef<ProtocolConformanceRef> getConformances() const;

  /// Look up a conformance for the given type to the given protocol.
  ProtocolConformanceRef lookupConformance(CanType type,
                                           ProtocolDecl *proto) const;

  /// Whether the substitution map is empty.
  bool empty() const;

  /// Whether the substitution has any substitutable parameters, i.e.,
  /// it is non-empty and at least one of the type parameters can be
  /// substituted (i.e., is not mapped to a concrete type).
  bool hasAnySubstitutableParams() const;
  
  /// True if this substitution map is an identity mapping.
  bool isIdentity() const;

  /// Whether the substitution map is non-empty.
  explicit operator bool() const { return !empty(); }

  /// Retrieve the array of replacement types, which line up with the
  /// generic parameters.
  ArrayRef<Type> getReplacementTypes() const;

  /// Retrieve the array of replacement types for the innermost generic
  /// parameters.
  ArrayRef<Type> getInnermostReplacementTypes() const;

  /// Query whether any replacement types in the map contain archetypes.
  bool hasArchetypes() const;

  /// Query whether any replacement types in the map contain an opened
  /// existential.
  bool hasLocalArchetypes() const;

  /// Query whether any replacement types in the map contain dynamic Self.
  bool hasDynamicSelf() const;

  /// Whether the replacement types are all canonical.
  bool isCanonical() const;

  /// Return the canonical form of this substitution map.
  SubstitutionMap getCanonical(bool canonicalizeSignature = true) const;

  /// Apply a substitution to all replacement types in the map. Does not
  /// change keys.
  SubstitutionMap subst(SubstitutionMap subMap,
                        SubstOptions options = llvm::None) const;

  /// Apply a substitution to all replacement types in the map. Does not
  /// change keys.
  SubstitutionMap subst(TypeSubstitutionFn subs,
                        LookupConformanceFn conformances,
                        SubstOptions options = llvm::None) const;

  /// Apply an in-flight substitution to all replacement types in the map.
  /// Does not change keys.
  ///
  /// This should generally not be used outside of the substitution
  /// subsystem.
  SubstitutionMap subst(InFlightSubstitution &subs) const;

  /// Apply type expansion lowering to all types in the substitution map. Opaque
  /// archetypes will be lowered to their underlying types if the type expansion
  /// context allows.
  SubstitutionMap mapIntoTypeExpansionContext(
      TypeExpansionContext context) const;

  /// Create a substitution map for a protocol conformance.
  static SubstitutionMap
  getProtocolSubstitutions(ProtocolDecl *protocol,
                           Type selfType,
                           ProtocolConformanceRef conformance);

  /// Given that 'derivedDecl' is an override of 'baseDecl' in a subclass,
  /// and 'derivedSubs' is a set of substitutions written in terms of the
  /// generic signature of 'derivedDecl', produce a set of substitutions
  /// written in terms of the generic signature of 'baseDecl'.
  static SubstitutionMap
  getOverrideSubstitutions(const ValueDecl *baseDecl,
                           const ValueDecl *derivedDecl);

  /// Variant of the above for when we have the generic signatures but not
  /// the decls for 'derived' and 'base'.
  static SubstitutionMap
  getOverrideSubstitutions(const NominalTypeDecl *baseNominal,
                           const NominalTypeDecl *derivedNominal,
                           GenericSignature baseSig,
                           const GenericParamList *derivedParams);

  /// Combine two substitution maps as follows.
  ///
  /// The result is written in terms of the generic parameters of 'genericSig'.
  ///
  /// Generic parameters with a depth or index less than 'firstDepthOrIndex'
  /// come from 'firstSubMap'.
  ///
  /// Generic parameters with a depth greater than 'firstDepthOrIndex' come
  /// from 'secondSubMap', but are looked up starting with a depth or index of
  /// 'secondDepthOrIndex'.
  ///
  /// The 'how' parameter determines if we're looking at the depth or index.
  static SubstitutionMap
  combineSubstitutionMaps(SubstitutionMap firstSubMap,
                          SubstitutionMap secondSubMap,
                          CombineSubstitutionMaps how,
                          unsigned baseDepthOrIndex,
                          unsigned origDepthOrIndex,
                          GenericSignature genericSig);

  /// Swap archetypes in the substitution map's replacement types with their
  /// interface types.
  SubstitutionMap mapReplacementTypesOutOfContext() const;

  /// Verify that this substitution map is valid.
  void verify() const;

  /// Whether to dump the full substitution map, or just a minimal useful subset
  /// (on a single line).
  enum class DumpStyle { Minimal, Full };
  /// Dump the contents of this substitution map for debugging purposes.
  void dump(llvm::raw_ostream &out, DumpStyle style = DumpStyle::Full,
            unsigned indent = 0) const;

  SWIFT_DEBUG_DUMP;

  /// Profile the substitution map, for use with LLVM's FoldingSet.
  void profile(llvm::FoldingSetNodeID &id) const;

  const void *getOpaqueValue() const { return storage; }

  static SubstitutionMap getFromOpaqueValue(const void *ptr) {
    return SubstitutionMap(const_cast<Storage *>((const Storage *)ptr));
  }

  static SubstitutionMap getEmptyKey() {
    return SubstitutionMap(
             (Storage *)llvm::DenseMapInfo<void*>::getEmptyKey());
  }

  static SubstitutionMap getTombstoneKey() {
    return SubstitutionMap(
               (Storage *)llvm::DenseMapInfo<void*>::getTombstoneKey());
  }

  friend bool operator ==(SubstitutionMap lhs, SubstitutionMap rhs) {
    return lhs.storage == rhs.storage;
  }

  friend bool operator !=(SubstitutionMap lhs, SubstitutionMap rhs) {
    return lhs.storage != rhs.storage;
  }

private:
  friend class GenericSignature;
  friend class GenericEnvironment;
  friend struct QuerySubstitutionMap;

  /// Look up the replacement for the given type parameter or interface type.
  /// Note that this only finds replacements for maps that are directly
  /// stored inside the map. In most cases, you should call Type::subst()
  /// instead, since that will resolve member types also.
  Type lookupSubstitution(CanSubstitutableType type) const;
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SubstitutionMap &subs) {
  subs.dump(OS);
  return OS;
}

/// A function object suitable for use as a \c TypeSubstitutionFn that
/// queries an underlying \c SubstitutionMap.
struct QuerySubstitutionMap {
  SubstitutionMap subMap;

  Type operator()(SubstitutableType *type) const;
};

/// Functor class suitable for use as a \c LookupConformanceFn to look up a
/// conformance in a \c SubstitutionMap.
class LookUpConformanceInSubstitutionMap {
  SubstitutionMap Subs;
public:
  explicit LookUpConformanceInSubstitutionMap(SubstitutionMap Subs)
    : Subs(Subs) {}

  ProtocolConformanceRef operator()(CanType dependentType,
                                    Type conformingReplacementType,
                                    ProtocolDecl *conformedProtocol) const;
};

struct OverrideSubsInfo {
  ASTContext &Ctx;
  unsigned BaseDepth;
  unsigned OrigDepth;
  SubstitutionMap BaseSubMap;
  const GenericParamList *DerivedParams;

  OverrideSubsInfo(const NominalTypeDecl *baseNominal,
                   const NominalTypeDecl *derivedNominal,
                   GenericSignature baseSig,
                   const GenericParamList *derivedParams);
};

struct QueryOverrideSubs {
  OverrideSubsInfo info;

  explicit QueryOverrideSubs(const OverrideSubsInfo &info)
    : info(info) {}

  Type operator()(SubstitutableType *type) const;
};

struct LookUpConformanceInOverrideSubs {
  OverrideSubsInfo info;

  explicit LookUpConformanceInOverrideSubs(const OverrideSubsInfo &info)
    : info(info) {}

  ProtocolConformanceRef operator()(CanType type,
                                    Type substType,
                                    ProtocolDecl *proto) const;
};

} // end namespace swift

namespace llvm {
  template <>
  struct PointerLikeTypeTraits<swift::SubstitutionMap> {
    static void *getAsVoidPointer(swift::SubstitutionMap map) {
      return const_cast<void *>(map.getOpaqueValue());
    }
    static swift::SubstitutionMap getFromVoidPointer(const void *ptr) {
      return swift::SubstitutionMap::getFromOpaqueValue(ptr);
    }

    /// Note: Assuming storage is at least 4-byte aligned.
    enum { NumLowBitsAvailable = 2 };
  };

  // Substitution maps hash just like pointers.
  template<> struct DenseMapInfo<swift::SubstitutionMap> {
    static swift::SubstitutionMap getEmptyKey() {
      return swift::SubstitutionMap::getEmptyKey();
    }
    static swift::SubstitutionMap getTombstoneKey() {
      return swift::SubstitutionMap::getTombstoneKey();
    }
    static unsigned getHashValue(swift::SubstitutionMap map) {
      return DenseMapInfo<void*>::getHashValue(map.getOpaqueValue());
    }
    static bool isEqual(swift::SubstitutionMap lhs,
                        swift::SubstitutionMap rhs) {
      return lhs.getOpaqueValue() == rhs.getOpaqueValue();
    }
  };

}

#endif

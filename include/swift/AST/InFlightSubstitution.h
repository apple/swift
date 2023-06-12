//===--- InFlightSubstitution.h - In-flight substitution data ---*- C++ -*-===//
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
// This file defines the InFlightSubstitution structure, which captures
// all the information about a type substitution that's currently in
// progress.  For now, this is meant to be an internal implementation
// detail of the substitution system, and other systems should not use
// it (unless they are part of the extended substitution system, such as
// the SIL type substituter)
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_INFLIGHTSUBSTITUTION_H
#define SWIFT_AST_INFLIGHTSUBSTITUTION_H

#include "swift/AST/SubstitutionMap.h"

namespace swift {
class SubstitutionMap;

class InFlightSubstitution {
  SubstOptions Options;
  TypeSubstitutionFn BaselineSubstType;
  LookupConformanceFn BaselineLookupConformance;

  struct ActivePackExpansion {
    bool isSubstExpansion = false;
    unsigned expansionIndex = 0;
  };
  SmallVector<ActivePackExpansion, 4> ActivePackExpansions;

public:
  InFlightSubstitution(TypeSubstitutionFn substType,
                       LookupConformanceFn lookupConformance,
                       SubstOptions options)
    : Options(options),
      BaselineSubstType(substType),
      BaselineLookupConformance(lookupConformance) {}

  InFlightSubstitution(const InFlightSubstitution &) = delete;
  InFlightSubstitution &operator=(const InFlightSubstitution &) = delete;

  /// Perform primitive substitution on the given type.  Returns Type()
  /// if the type should not be substituted as a whole.
  Type substType(SubstitutableType *origType, unsigned level);

  /// Perform primitive conformance lookup on the given type.
  ProtocolConformanceRef lookupConformance(CanType dependentType,
                                           Type conformingReplacementType,
                                           ProtocolDecl *conformedProtocol,
                                           unsigned level);

  /// Given the shape type of a pack expansion, invoke the given callback
  /// for each expanded component of it.  If the substituted component
  /// is an expansion component, the desired shape of that expansion
  /// is passed as the argument; otherwise, the argument is Type().
  /// In either case, an active expansion is entered on this IFS for
  /// the duration of the call to handleComponent, and subsequent
  /// pack-element type references will substitute to the corresponding
  /// element of the substitution of the pack.
  void expandPackExpansionShape(Type origShape,
      llvm::function_ref<void(Type substComponentShape)> handleComponent);

  /// Call the given function for each expanded component type of the
  /// given pack expansion type.  The function will be invoked with the
  /// active expansion still active.
  void expandPackExpansionType(PackExpansionType *origExpansionType,
      llvm::function_ref<void(Type substType)> handleComponentType) {
    expandPackExpansionShape(origExpansionType->getCountType(),
                             [&](Type substComponentShape) {
      auto origPatternType = origExpansionType->getPatternType();
      auto substEltType = origPatternType.subst(*this);

      auto substComponentType =
        (substComponentShape
           ? PackExpansionType::get(substEltType, substComponentShape)
           : substEltType);
      handleComponentType(substComponentType);
    });
  }

  /// Return a list of component types that the pack expansion expands to.
  SmallVector<Type, 8>
  expandPackExpansionType(PackExpansionType *origExpansionType) {
    SmallVector<Type, 8> substComponentTypes;
    expandPackExpansionType(origExpansionType, substComponentTypes);
    return substComponentTypes;
  }

  /// Expand the list of component types that the pack expansion expands
  /// to into the given array.
  void expandPackExpansionType(PackExpansionType *origExpansionType,
                               SmallVectorImpl<Type> &substComponentTypes) {
    expandPackExpansionType(origExpansionType, [&](Type substComponentType) {
      substComponentTypes.push_back(substComponentType);
    });
  }

  class OptionsAdjustmentScope {
    InFlightSubstitution &IFS;
    SubstOptions SavedOptions;

  public:
    OptionsAdjustmentScope(InFlightSubstitution &IFS, SubstOptions newOptions)
      : IFS(IFS), SavedOptions(IFS.Options) {
      IFS.Options = newOptions;
    }

    OptionsAdjustmentScope(const OptionsAdjustmentScope &) = delete;
    OptionsAdjustmentScope &operator=(const OptionsAdjustmentScope &) = delete;

    ~OptionsAdjustmentScope() {
      IFS.Options = SavedOptions;
    }
  };

  template <class Fn>
  auto withNewOptions(SubstOptions options, Fn &&fn)
      -> decltype(std::forward<Fn>(fn)()) {
    OptionsAdjustmentScope scope(*this, options);
    return std::forward<Fn>(fn)();
  }

  SubstOptions getOptions() const {
    return Options;
  }

  bool shouldSubstituteOpaqueArchetypes() const {
    return Options.contains(SubstFlags::SubstituteOpaqueArchetypes);
  }

  /// Is the given type invariant to substitution?
  bool isInvariant(Type type) const;
};

/// A helper classes that provides stable storage for the query
/// functions against a SubstitutionMap.
struct InFlightSubstitutionViaSubMapHelper {
  QuerySubstitutionMap QueryType;
  LookUpConformanceInSubstitutionMap QueryConformance;

  InFlightSubstitutionViaSubMapHelper(SubstitutionMap subMap)
    : QueryType{subMap}, QueryConformance(subMap) {}
};
class InFlightSubstitutionViaSubMap :
  private InFlightSubstitutionViaSubMapHelper,
  public InFlightSubstitution {

public:
  InFlightSubstitutionViaSubMap(SubstitutionMap subMap,
                                SubstOptions options)
    : InFlightSubstitutionViaSubMapHelper(subMap),
      InFlightSubstitution(QueryType, QueryConformance, options) {}
};

} // end namespace swift

#endif

//===--- Existential.h - Existential related Analyses. -------*- C++ //-*-===//
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

#ifndef SWIFT_SILOPTIMIZER_UTILS_EXISTENTIAL_H
#define SWIFT_SILOPTIMIZER_UTILS_EXISTENTIAL_H

#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Analysis/ProtocolConformanceAnalysis.h"

namespace swift {

/// Record information about an opened archetype.
///
/// This is used to determine whether a generic call argument originates from
/// an opened existential. For example:
/// %o = open_existential_ref %e : $P & Q to $@opened("PQ") P & Q
/// %r = apply %f<@opened("PQ") P & Q>(%o)
///   : $@convention(method) <τ_0_0 where τ_0_0 : P, τ_0_0 : Q>
///     (@guaranteed τ_0_0) -> @owned τ_0_0
///
/// When successful, ConcreteExistentialInfo can be used to determine the
/// concrete type of the opened existential.
struct OpenedArchetypeInfo {
  OpenedArchetypeType *OpenedArchetype = nullptr;
  // The opened value.
  SingleValueInstruction *OpenedArchetypeValue;
  // The existential value.
  SILValue ExistentialValue;
  // True if the openedValue is copied from another stack location
  bool isOpenedValueCopied = false;

  // Construct a valid instance if the given use originates from a recognizable
  // OpenedArchetype instruction.
  OpenedArchetypeInfo(Operand &use);

  bool isValid() const {
    assert(!OpenedArchetype || (OpenedArchetypeValue && ExistentialValue));
    return OpenedArchetype;
  }
  
  void dump() const;
};

/// Record conformance and concrete type info derived from an init_existential
/// value that is reopened before it's use. This is useful for finding the
/// concrete type of an apply's self argument. For example, an important pattern
/// for a class existential is:
///
/// %e = init_existential_ref %c : $C : $C, $P & Q
/// %o = open_existential_ref %e : $P & Q to $@opened("PQ") P & Q
/// %r = apply %f<@opened("PQ") P & Q>(%o)
///   : $@convention(method) <τ_0_0 where τ_0_0 : P, τ_0_0 : Q>
///     (@guaranteed τ_0_0) -> @owned τ_0_0
struct ConcreteExistentialInfo {
  // The existential type of the self argument before it is opened,
  // produced by an init_existential.
  CanType ExistentialType;
  // The concrete type of self from the init_existential. `$C` above.
  CanType ConcreteType;
  // The concrete value used to initialize the opened existential.
  // `%c` in the above comment.
  SILValue ConcreteValue;
  // True if the ConcreteValue is copied from another stack location
  bool isConcreteValueCopied = false;
  // When ConcreteType is itself an opened existential, record the type
  // definition. May be nullptr for a valid AppliedConcreteType.
  SingleValueInstruction *ConcreteTypeDef = nullptr;
  // The Substitution map derived from the init_existential.
  // This maps a single generic parameter to the replacement ConcreteType
  // and includes the full list of existential conformances.
  // signature: <P & Q>, replacement: $C : conformances: [$P, $Q]
  SubstitutionMap ExistentialSubs;

  // Search for a recognized pattern in which the given existential value is
  // initialized to a concrete type. Constructs a valid ConcreteExistentialInfo
  // object if successful.
  ConcreteExistentialInfo(SILValue existential, SILInstruction *user);

  // This constructor initializes a ConcreteExistentialInfo based on already
  // known ConcreteType and ProtocolDecl pair.
  ConcreteExistentialInfo(SILValue existential, SILInstruction *user,
                          CanType ConcreteType, ProtocolDecl *Protocol);

  /// For scenarios where ConcreteExistentialInfo is created using a known
  /// ConcreteType and ProtocolDecl, the ConcreteValue can be null.
  bool isValid() const { return ConcreteType && !ExistentialSubs.empty(); }

  // Do a conformance lookup on ConcreteType with the given requirement, P. If P
  // is satisfiable based on the existential's conformance, return the new
  // conformance on P. Otherwise return None.
  ProtocolConformanceRef lookupExistentialConformance(ProtocolDecl *P) const {
    CanType selfTy = P->getSelfInterfaceType()->getCanonicalType();
    return ExistentialSubs.lookupConformance(selfTy, P);
  }
  
  void dump() const;

private:
  void initializeSubstitutionMap(
      ArrayRef<ProtocolConformanceRef> ExistentialConformances, SILModule *M);

  void initializeConcreteTypeDef(SILInstruction *typeConversionInst);
};

// Convenience for tracking both the OpenedArchetypeInfo and
// ConcreteExistentialInfo from the same SILValue.
struct ConcreteOpenedExistentialInfo {
  OpenedArchetypeInfo OAI;
  // If CEI has a value, it must be valid.
  llvm::Optional<ConcreteExistentialInfo> CEI;

  ConcreteOpenedExistentialInfo(Operand &use);

  // Provide a whole module type-inferred ConcreteType to fall back on if the
  // concrete type cannot be determined from data flow.
  ConcreteOpenedExistentialInfo(Operand &use, CanType concreteType,
                                ProtocolDecl *protocol);

  bool isValid() const {
    if (!CEI)
      return false;

    assert(CEI->isValid());
    return true;
  }
  
  void dump() const;
};

} // end namespace swift

#endif

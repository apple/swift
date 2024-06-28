//===--- ProtocolConformance.h - AST Protocol Conformance -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the protocol conformance data structures.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_PROTOCOLCONFORMANCE_H
#define SWIFT_AST_PROTOCOLCONFORMANCE_H

#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/AST/Witness.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include <utility>

namespace swift {

class ASTContext;
class DiagnosticEngine;
class GenericParamList;
class NormalProtocolConformance;
class RootProtocolConformance;
class ProtocolConformance;
class ModuleDecl;
class SubstitutableType;
enum class AllocationArena;

/// Type substitution mapping from substitutable types to their
/// replacements.
typedef llvm::DenseMap<SubstitutableType *, Type> TypeSubstitutionMap;

/// Map from non-type requirements to the corresponding conformance witnesses.
typedef llvm::DenseMap<ValueDecl *, Witness> WitnessMap;

/// Map from associated type requirements to the corresponding type and
/// the type declaration that was used to satisfy the requirement.
typedef llvm::DenseMap<AssociatedTypeDecl *, TypeWitnessAndDecl>
  TypeWitnessMap;

/// Describes the kind of protocol conformance structure used to encode
/// conformance.
enum class ProtocolConformanceKind : unsigned {
  /// "Normal" conformance of a (possibly generic) nominal type, which
  /// contains complete mappings.
  Normal,
  /// Self-conformance of a protocol to itself.
  Self,
  /// Conformance for a specialization of a generic type, which projects the
  /// underlying generic conformance.
  Specialized,
  /// Conformance of a generic class type projected through one of its
  /// superclass's conformances.
  Inherited,
  /// Builtin conformances are special conformances that the runtime handles
  /// and isn't implemented directly in Swift.
  Builtin,

  Last_Kind = Builtin
};
enum : unsigned {
  NumProtocolConformanceKindBits =
      countBitsUsed(static_cast<unsigned>(ProtocolConformanceKind::Last_Kind))
};

/// Describes the state of a protocol conformance, which may be complete,
/// incomplete, or currently being checked.
enum class ProtocolConformanceState {
  /// The conformance has been fully checked.
  Complete = 0,
  /// The conformance is known but is not yet complete.
  Incomplete,
  /// The conformance is being checked.
  Checking,

  Last_State = Checking
};

/// Describes the kind of a builtin conformance.
enum class BuiltinConformanceKind {
  // A builtin conformance that has been synthesized by the implementation.
  Synthesized = 0,
  // A missing conformance that we have nonetheless synthesized so that
  // we can diagnose it later.
  Missing,

  Last_Kind = Missing
};

enum : unsigned {
  NumProtocolConformanceStateBits =
      countBitsUsed(static_cast<unsigned>(ProtocolConformanceState::Last_State))
};

enum : unsigned {
  NumConformanceEntryKindBits =
      countBitsUsed(static_cast<unsigned>(ConformanceEntryKind::Last_Kind))
};

enum : unsigned {
  NumBuiltinConformanceKindBits =
      countBitsUsed(static_cast<unsigned>(BuiltinConformanceKind::Last_Kind))
};

/// Describes how a particular type conforms to a given protocol,
/// providing the mapping from the protocol members to the type (or extension)
/// members that provide the functionality for the concrete type.
///
/// ProtocolConformance is an abstract base class, implemented by subclasses
/// for the various kinds of conformance (normal, specialized, inherited).
class alignas(1 << DeclAlignInBits) ProtocolConformance
    : public ASTAllocated<ProtocolConformance> {
  /// The type that conforms to the protocol, in the context of the
  /// conformance definition.
  Type ConformingType;

protected:
  // clang-format off
  //
  // We format these different than clang-format wishes us to... so turn if off
  // for the inline bitfields.
  union { uint64_t OpaqueBits;

    SWIFT_INLINE_BITFIELD_BASE(ProtocolConformance,
                               bitmax(NumProtocolConformanceKindBits, 8),
      /// The kind of protocol conformance.
      Kind : bitmax(NumProtocolConformanceKindBits, 8)
    );

    SWIFT_INLINE_BITFIELD_EMPTY(RootProtocolConformance, ProtocolConformance);

    SWIFT_INLINE_BITFIELD_FULL(NormalProtocolConformance, RootProtocolConformance,
                               1+1+1+1+bitmax(NumProtocolConformanceStateBits,8)+
                               bitmax(NumConformanceEntryKindBits,8),
      /// Indicates whether the conformance is invalid.
      IsInvalid : 1,
      /// The conformance was labeled with @unchecked.
      IsUnchecked : 1,
      /// The conformance was labeled with @preconcurrency.
      IsPreconcurrency : 1,
      /// We have allocated the AssociatedConformances array (but not necessarily
      /// populated any of its elements).
      HasComputedAssociatedConformances : 1,

      : NumPadBits,

      /// The current state of the conformance.
      State : bitmax(NumProtocolConformanceStateBits, 8),
      /// The reason that this conformance exists.
      ///
      /// Either Explicit (e.g. 'struct Foo: Protocol {}' or 'extension Foo:
      /// Protocol {}'), Synthesized (e.g. RawRepresentable for 'enum Foo: Int {}')
      /// or Implied (e.g. 'Foo : Protocol' in 'protocol Other: Protocol {} struct
      /// Foo: Other {}'). In only the latter case, the conformance is non-null and
      /// points to the conformance that implies this one.
      ///
      /// This should never be Inherited: that is handled by
      /// InheritedProtocolConformance.
      SourceKind : bitmax(NumConformanceEntryKindBits, 8)
    );

    SWIFT_INLINE_BITFIELD(BuiltinProtocolConformance, RootProtocolConformance,
                          bitmax(NumBuiltinConformanceKindBits, 8),
      /// The kind of the builtin conformance
      Kind: bitmax(NumBuiltinConformanceKindBits, 8)
    );
  } Bits;
  // clang-format on

  ProtocolConformance(ProtocolConformanceKind kind, Type conformingType)
    : ConformingType(conformingType) {
    Bits.ProtocolConformance.Kind = unsigned(kind);
  }

public:
  /// Determine the kind of protocol conformance.
  ProtocolConformanceKind getKind() const {
    return static_cast<ProtocolConformanceKind>(Bits.ProtocolConformance.Kind);
  }

  /// Get the conforming type.
  Type getType() const { return ConformingType; }

  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const;

  /// Get the declaration context that contains the conforming extension or
  /// nominal type declaration.
  DeclContext *getDeclContext() const;

  /// Retrieve the state of this conformance.
  ProtocolConformanceState getState() const;

  /// Get the kind of source from which this conformance comes.
  ConformanceEntryKind getSourceKind() const;
  /// Get the protocol conformance which implied this implied conformance.
  NormalProtocolConformance *getImplyingConformance() const;

  /// Determine whether this conformance is complete.
  bool isComplete() const {
    return getState() == ProtocolConformanceState::Complete;
  }

  /// Determine whether this conformance is invalid.
  bool isInvalid() const;

  /// Determine whether this conformance is incomplete.
  bool isIncomplete() const {
    return getState() == ProtocolConformanceState::Incomplete ||
           getState() == ProtocolConformanceState::Checking;
  }

  /// Determine whether this conformance is canonical.
  bool isCanonical() const;

  /// Create a canonical conformance from the current one.
  /// If the current conformance is canonical already, it will be returned.
  /// Otherwise a new conformance will be created.
  ProtocolConformance *getCanonicalConformance();

  /// Return true if the conformance has a witness for the given associated
  /// type.
  bool hasTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Retrieve the type witness for the given associated type.
  Type getTypeWitness(AssociatedTypeDecl *assocType,
                      SubstOptions options = std::nullopt) const;

  /// Retrieve the type witness and type decl (if one exists)
  /// for the given associated type.
  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const;

  /// Apply the given function object to each type witness within this
  /// protocol conformance.
  ///
  /// The function object should accept an \c AssociatedTypeDecl* for the
  /// requirement followed by the \c Type for the witness and a
  /// (possibly null) \c TypeDecl* that explicitly declared the type.
  /// It should return true to indicate an early exit.
  ///
  /// \returns true if the function ever returned true
  template<typename F>
  bool forEachTypeWitness(F f, bool useResolver=false) const {
    const ProtocolDecl *protocol = getProtocol();
    for (auto assocTypeReq : protocol->getAssociatedTypeMembers()) {
      if (assocTypeReq->isInvalid())
        continue;

      // If we don't have and cannot resolve witnesses, skip it.
      if (!useResolver && !hasTypeWitness(assocTypeReq))
        continue;

      const auto &TWInfo = getTypeWitnessAndDecl(assocTypeReq);
      if (f(assocTypeReq, TWInfo.getWitnessType(), TWInfo.getWitnessDecl()))
        return true;
    }

    return false;
  }

  /// Apply the given function object to each associated conformance requirement
  /// within this protocol conformance.
  ///
  /// \returns true if the function ever returned true
  template<typename F>
  bool forEachAssociatedConformance(F f) const {
    const ProtocolDecl *protocol = getProtocol();
    unsigned index = 0;
    for (auto req : protocol->getRequirementSignature().getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance)
        continue;

      if (f(req.getFirstType(), req.getProtocolDecl(), index))
        return true;

      ++index;
    }

    return false;
  }

  /// Retrieve the value witness declaration corresponding to the given
  /// requirement.
  ValueDecl *getWitnessDecl(ValueDecl *requirement) const;

  /// Retrieve the witness corresponding to the given value requirement.
  /// TODO: maybe this should return a Witness?
  ConcreteDeclRef getWitnessDeclRef(ValueDecl *requirement) const;

private:
  /// Determine whether we have a witness for the given requirement.
  bool hasWitness(ValueDecl *requirement) const;

public:
  /// Apply the given function object to each requirement, either type or value,
  /// that is not witnessed.
  ///
  /// The function object should accept a \c ValueDecl* for the requirement.
  template<typename F>
  void forEachNonWitnessedRequirement(F f) const {
    const ProtocolDecl *protocol = getProtocol();
    for (auto req : protocol->getMembers()) {
      auto valueReq = dyn_cast<ValueDecl>(req);
      if (!valueReq || valueReq->isInvalid())
        continue;

      if (auto assocTypeReq = dyn_cast<AssociatedTypeDecl>(req)) {
        // If we don't have witness for the associated type, apply the function.
        if (getTypeWitness(assocTypeReq)->hasError()) {
          f(valueReq);
        }
        continue;
      }

      if (!valueReq->isProtocolRequirement())
        continue;

      // If we don't have witness for the value, apply the function.
      if (!hasWitness(valueReq)) {
        f(valueReq);
      }
    }
  }

  /// Retrieve the protocol conformance for the inherited protocol.
  ProtocolConformance *getInheritedConformance(ProtocolDecl *protocol) const;

  /// Given a dependent type expressed in terms of the self parameter,
  /// map it into the context of this conformance.
  Type getAssociatedType(Type assocType) const;

  /// Given that the requirement signature of the protocol directly states
  /// that the given dependent type must conform to the given protocol,
  /// return its associated conformance.
  ProtocolConformanceRef
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const;

  /// Get the generic parameters open on the conforming type.
  GenericEnvironment *getGenericEnvironment() const;

  /// Get the generic signature containing the parameters open on the conforming
  /// interface type.
  GenericSignature getGenericSignature() const;

  /// Get the conformance substitution map.
  SubstitutionMap getSubstitutionMap() const;

  /// Get the underlying normal conformance.
  /// FIXME: remove uses of this.
  const NormalProtocolConformance *getRootNormalConformance() const;

  /// Get the underlying normal conformance.
  NormalProtocolConformance *getRootNormalConformance() {
    return const_cast<NormalProtocolConformance *>(
             const_cast<const ProtocolConformance *>(this)
               ->getRootNormalConformance());
  }

  /// Get the underlying root conformance.
  const RootProtocolConformance *getRootConformance() const;

  /// Get the underlying root conformance.
  RootProtocolConformance *getRootConformance() {
    return const_cast<RootProtocolConformance *>(
      const_cast<const ProtocolConformance *>(this)->getRootConformance());
  }

  /// Determine whether this protocol conformance is visible from the
  /// given declaration context.
  bool isVisibleFrom(const DeclContext *dc) const;

  /// Determine whether the witness for the given requirement
  /// is either the default definition or was otherwise deduced.
  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const;

  /// Determines whether this conformance is retroactive; that is, if the
  /// conformance's declaration is in a different module from both the
  /// conforming type and the protocol.
  bool isRetroactive() const;

  /// Print a parseable and human-readable description of the identifying
  /// information of the protocol conformance.
  void printName(raw_ostream &os,
                 const PrintOptions &PO = PrintOptions()) const;

  /// Get any additional requirements that are required for this conformance to
  /// be satisfied, if it is possible for them to be computed.
  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const;

  /// Get any additional requirements that are required for this conformance to
  /// be satisfied.
  ArrayRef<Requirement> getConditionalRequirements() const;

  /// Substitute the conforming type and produce a ProtocolConformanceRef that
  /// applies to the substituted type.
  ProtocolConformanceRef subst(SubstitutionMap subMap,
                               SubstOptions options = std::nullopt) const;

  /// Substitute the conforming type and produce a ProtocolConformanceRef that
  /// applies to the substituted type.
  ProtocolConformanceRef subst(TypeSubstitutionFn subs,
                               LookupConformanceFn conformances,
                               SubstOptions options = std::nullopt) const;

  /// Substitute the conforming type and produce a ProtocolConformanceRef that
  /// applies to the substituted type.
  ///
  /// This function should generally not be used outside of the substitution
  /// subsystem.
  ProtocolConformanceRef subst(InFlightSubstitution &IFS) const;

  SWIFT_DEBUG_DUMP;
  void dump(llvm::raw_ostream &out, unsigned indent = 0) const;
};

/// A "root" protocol conformance states some sort of ground truth
/// about the conforming type and the required protocol.  Either:
///
/// - the type is directly declared to conform to the protocol (a
///   normal conformance) or
/// - the protocol's existential type is known to conform to itself (a
///   self-conformance) or
/// - the type's conformance is declared within the runtime (a builtin
///   conformance).
class RootProtocolConformance : public ProtocolConformance {
protected:
  RootProtocolConformance(ProtocolConformanceKind kind, Type conformingType)
    : ProtocolConformance(kind, conformingType) {}

public:
  /// Retrieve the location of this conformance.
  SourceLoc getLoc() const;

  bool isInvalid() const;

  /// Whether this conformance is weak-imported.
  bool isWeakImported(ModuleDecl *fromModule) const;

  bool hasWitness(ValueDecl *requirement) const;
  Witness getWitness(ValueDecl *requirement) const;

  /// Retrieve the witness corresponding to the given value requirement.
  /// TODO: maybe this should return a Witness?
  ConcreteDeclRef getWitnessDeclRef(ValueDecl *requirement) const;

  /// Get the conformance substitution map.
  SubstitutionMap getSubstitutionMap() const;

  /// Whether this conformance was synthesized automatically and can have
  /// multiple copies in a single program.
  bool isSynthesized() const;

  /// Apply the given function object to each value witness within this
  /// protocol conformance.
  ///
  /// The function object should accept a \c ValueDecl* for the requirement
  /// followed by the \c Witness for the witness. Note that a generic
  /// witness will only be specialized if the conformance came from the current
  /// file.
  template<typename F>
  void forEachValueWitness(F f, bool useResolver=false) const {
    const ProtocolDecl *protocol = getProtocol();
    for (auto req : protocol->getMembers()) {
      auto valueReq = dyn_cast<ValueDecl>(req);
      if (!valueReq || isa<AssociatedTypeDecl>(valueReq) ||
          valueReq->isInvalid())
        continue;

      if (!valueReq->isProtocolRequirement())
        continue;

      // If we don't have and cannot resolve witnesses, skip it.
      if (!useResolver && !hasWitness(valueReq))
        continue;

      f(valueReq, getWitness(valueReq));
    }
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Normal ||
           conformance->getKind() == ProtocolConformanceKind::Self ||
           conformance->getKind() == ProtocolConformanceKind::Builtin;
  }
};

/// Normal protocol conformance, which involves mapping each of the protocol
/// requirements to a witness.
///
/// Normal protocol conformance is used for the explicit conformances placed on
/// nominal types and extensions. For example:
///
/// \code
/// protocol P { func foo() }
/// struct A : P { func foo() { } }
/// class B<T> : P { func foo() { } }
/// \endcode
///
/// Here, there is a normal protocol conformance for both \c A and \c B<T>,
/// providing the witnesses \c A.foo and \c B<T>.foo, respectively, for the
/// requirement \c foo.
class NormalProtocolConformance : public RootProtocolConformance,
                                  public llvm::FoldingSetNode
{
  friend class ValueWitnessRequest;
  friend class TypeWitnessRequest;

  /// The protocol being conformed to.
  ProtocolDecl *Protocol;

  /// The location of this protocol conformance in the source.
  SourceLoc Loc;

  /// The location of the protocol name within the conformance.
  SourceLoc ProtocolNameLoc;

  /// The location of the `@preconcurrency` attribute, if any.
  SourceLoc PreconcurrencyLoc;

  /// The declaration context containing the ExtensionDecl or
  /// NominalTypeDecl that declared the conformance.
  DeclContext *Context;

  NormalProtocolConformance *ImplyingConformance = nullptr;

  /// The mapping of individual requirements in the protocol over to
  /// the declarations that satisfy those requirements.
  mutable WitnessMap Mapping;

  /// The mapping from associated type requirements to their types.
  mutable TypeWitnessMap TypeWitnesses;

  /// Conformances that satisfy each of conformance requirements of the
  /// requirement signature of the protocol.
  MutableArrayRef<std::optional<ProtocolConformanceRef>> AssociatedConformances;

  /// The lazy member loader provides callbacks for populating imported and
  /// deserialized conformances.
  ///
  /// This is not use for parsed conformances -- those are lazily populated
  /// by the ASTContext's LazyResolver, which is really a Sema instance.
  LazyConformanceLoader *Loader = nullptr;
  uint64_t LoaderContextData;
  friend class ASTContext;

  void resolveLazyInfo() const;

public:
  NormalProtocolConformance(Type conformingType, ProtocolDecl *protocol,
                            SourceLoc loc, DeclContext *dc,
                            ProtocolConformanceState state, bool isUnchecked,
                            bool isPreconcurrency,
                            SourceLoc preconcurrencyLoc)
      : RootProtocolConformance(ProtocolConformanceKind::Normal,
                                conformingType),
        Protocol(protocol), Loc(extractNearestSourceLoc(dc)),
        ProtocolNameLoc(loc), PreconcurrencyLoc(preconcurrencyLoc),
        Context(dc) {
    assert(!conformingType->hasArchetype() &&
           "ProtocolConformances should store interface types");
    assert((preconcurrencyLoc.isInvalid() || isPreconcurrency) &&
           "Cannot have a @preconcurrency location without isPreconcurrency");
    setState(state);
    Bits.NormalProtocolConformance.IsInvalid = false;
    Bits.NormalProtocolConformance.IsUnchecked = isUnchecked;
    Bits.NormalProtocolConformance.IsPreconcurrency = isPreconcurrency;
    Bits.NormalProtocolConformance.HasComputedAssociatedConformances = false;
    Bits.NormalProtocolConformance.SourceKind =
        unsigned(ConformanceEntryKind::Explicit);
  }

  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const { return Protocol; }

  /// Retrieve the location of this conformance.
  SourceLoc getLoc() const { return Loc; }

  /// Retrieve the name of the protocol location.
  SourceLoc getProtocolNameLoc() const { return ProtocolNameLoc; }

  /// Get the declaration context that contains the conforming extension or
  /// nominal type declaration.
  DeclContext *getDeclContext() const { return Context; }

  /// Get any additional requirements that are required for this conformance to
  /// be satisfied, e.g. for Array<T>: Equatable, T: Equatable also needs
  /// to be satisfied.
  ArrayRef<Requirement> getConditionalRequirements() const;

  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const;

  /// Retrieve the state of this conformance.
  ProtocolConformanceState getState() const {
    return static_cast<ProtocolConformanceState>(
        Bits.NormalProtocolConformance.State);
  }

  /// Set the state of this conformance.
  void setState(ProtocolConformanceState state) {
    Bits.NormalProtocolConformance.State = unsigned(state);
  }

  /// Determine whether this conformance is invalid.
  bool isInvalid() const { return Bits.NormalProtocolConformance.IsInvalid; }

  /// Mark this conformance as invalid.
  void setInvalid() { Bits.NormalProtocolConformance.IsInvalid = true; }

  /// Whether this is an "unchecked" conformance.
  bool isUnchecked() const {
    return Bits.NormalProtocolConformance.IsUnchecked;
  }

  /// Mark the conformance as unchecked (equivalent to the @unchecked
  /// conformance attribute).
  void setUnchecked() {
    // OK to mutate because the flags are not part of the folding set node ID.
    Bits.NormalProtocolConformance.IsUnchecked = true;
  }

  /// Whether this is an preconcurrency conformance.
  bool isPreconcurrency() const {
    return Bits.NormalProtocolConformance.IsPreconcurrency;
  }

  /// Retrieve the location of `@preconcurrency`, if there is one and it is
  /// known.
  SourceLoc getPreconcurrencyLoc() const { return PreconcurrencyLoc; }

  /// Determine whether we've lazily computed the associated conformance array
  /// already.
  bool hasComputedAssociatedConformances() const {
    return Bits.NormalProtocolConformance.HasComputedAssociatedConformances;
  }

  /// Mark this conformance as having computed the assocaited conformance array.
  void setHasComputedAssociatedConformances() {
    Bits.NormalProtocolConformance.HasComputedAssociatedConformances = true;
  }

  /// Get the kind of source from which this conformance comes.
  ConformanceEntryKind getSourceKind() const {
    return static_cast<ConformanceEntryKind>(
        Bits.NormalProtocolConformance.SourceKind);
  }

  /// Get the protocol conformance which implied this implied conformance.
  NormalProtocolConformance *getImplyingConformance() const {
    assert(getSourceKind() == ConformanceEntryKind::Implied);
    return ImplyingConformance;
  }

  void setSourceKindAndImplyingConformance(
      ConformanceEntryKind sourceKind,
      NormalProtocolConformance *implyingConformance) {
    assert(sourceKind != ConformanceEntryKind::Inherited &&
           "a normal conformance cannot be inherited");
    assert((sourceKind == ConformanceEntryKind::Implied) ==
               (bool)implyingConformance &&
           "an implied conformance needs something that implies it");
    assert(sourceKind != ConformanceEntryKind::PreMacroExpansion &&
           "cannot create conformance pre-macro-expansion");
    Bits.NormalProtocolConformance.SourceKind = unsigned(sourceKind);
    if (auto implying = implyingConformance) {
      ImplyingConformance = implying;
      PreconcurrencyLoc = implying->getPreconcurrencyLoc();
      Bits.NormalProtocolConformance.IsPreconcurrency =
          implying->isPreconcurrency();
    }
  }

  /// Determine whether this conformance is lazily loaded.
  ///
  /// This only matters to the AST verifier.
  bool isLazilyLoaded() const { return Loader != nullptr; }

  /// A "retroactive" conformance is one that is defined in a module that
  /// is neither the module that defines the protocol nor the module that
  /// defines the conforming type.
  bool isRetroactive() const;

  /// Whether this conformance was synthesized automatically in multiple
  /// modules, but in a manner that ensures that all copies are equivalent.
  bool isSynthesizedNonUnique() const;

  /// Whether this conformance represents the conformance of one protocol's
  /// conforming types to another protocol.
  ///
  /// Such conformances cannot generally be written in the surface language, but
  /// can be made available for specific tasks. The only such instance at the
  /// time of this writing is that a (local) distributed actor can conform to
  /// a local actor, but the witness table can only be used via a specific
  /// builtin to form an existential.
  bool isConformanceOfProtocol() const;

  /// Whether clients from outside the module can rely on the value witnesses
  /// being consistent across versions of the framework.
  bool isResilient() const;

  /// Retrieve the type witness and type decl (if one exists)
  /// for the given associated type.
  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const;

  TypeWitnessAndDecl
  getTypeWitnessUncached(AssociatedTypeDecl *requirement) const;

  /// Determine whether the protocol conformance has a type witness for the
  /// given associated type.
  bool hasTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Set the type witness for the given associated type.
  /// \param typeDecl the type decl the witness type came from, if one exists.
  void setTypeWitness(AssociatedTypeDecl *assocType, Type type,
                      TypeDecl *typeDecl) const;

  /// Given that the requirement signature of the protocol directly states
  /// that the given dependent type must conform to the given protocol,
  /// return its associated conformance.
  ProtocolConformanceRef
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const;

  /// Allocate the backing array if needed, computing its size from the
  ///protocol's requirement signature.
  void createAssociatedConformanceArray();

  std::optional<ProtocolConformanceRef>
  getAssociatedConformance(unsigned index) const;

  void
  setAssociatedConformance(unsigned index, ProtocolConformanceRef assocConf);

  /// Retrieve the value witness corresponding to the given requirement.
  Witness getWitness(ValueDecl *requirement) const;

  Witness getWitnessUncached(ValueDecl *requirement) const;

  /// Determine whether the protocol conformance has a witness for the given
  /// requirement.
  bool hasWitness(ValueDecl *requirement) const {
    if (Loader)
      resolveLazyInfo();
    return Mapping.count(requirement) > 0;
  }

  /// Set the witness for the given requirement.
  void setWitness(ValueDecl *requirement, Witness witness) const;

  /// Override the witness for a given requirement.
  void overrideWitness(ValueDecl *requirement, Witness newWitness);

  /// Triggers a request that resolves all of the conformance's value witnesses.
  void resolveValueWitnesses() const;

  /// Determine whether the witness for the given type requirement
  /// is the default definition.
  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
    TypeDecl *witnessDecl = getTypeWitnessAndDecl(requirement).getWitnessDecl();
    if (witnessDecl)
      return witnessDecl->isImplicit();
    // Conservatively assume it does not.
    return false;
  }

  void setLazyLoader(LazyConformanceLoader *resolver, uint64_t contextData);

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getProtocol(), getDeclContext());
  }

  static void Profile(llvm::FoldingSetNodeID &ID, ProtocolDecl *protocol,
                      DeclContext *dc) {
    ID.AddPointer(protocol);
    ID.AddPointer(dc);
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Normal;
  }
};

/// The conformance of a protocol to itself.
///
/// For now, we generally do not use this type in ProtocolConformanceRefs;
/// it's only used to anchor structures relating to emitting witness tables
/// for self-conformances.
class SelfProtocolConformance : public RootProtocolConformance {
  friend class ASTContext;

  SelfProtocolConformance(Type conformingType)
    : RootProtocolConformance(ProtocolConformanceKind::Self, conformingType) {
  }

public:
  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const {
    return dyn_cast<ProtocolDecl>(getType()->getAnyNominal());
  }

  /// Get the declaration context in which this conformance was declared.
  DeclContext *getDeclContext() const {
    return getProtocol();
  }

  /// Retrieve the location of this conformance.
  SourceLoc getLoc() const {
    return getProtocol()->getLoc();
  }

  ProtocolConformanceState getState() const {
    return ProtocolConformanceState::Complete;
  }

  bool isInvalid() const {
    return false;
  }

  ConformanceEntryKind getSourceKind() const {
    return ConformanceEntryKind::Explicit; // FIXME?
  }

  NormalProtocolConformance *getImplyingConformance() const {
    llvm_unreachable("never an implied conformance");
  }

  bool hasTypeWitness(AssociatedTypeDecl *assocType) const {
    llvm_unreachable("self-conformances never have associated types");
  }

  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const {
    llvm_unreachable("self-conformances never have associated types");
  }

  Type getTypeWitness(AssociatedTypeDecl *assocType,
                      SubstOptions options = std::nullopt) const {
    llvm_unreachable("self-conformances never have associated types");
  }

  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
    llvm_unreachable("self-conformances never have associated types");
  }

  ProtocolConformanceRef getAssociatedConformance(Type assocType,
                                                  ProtocolDecl *protocol) const{
    llvm_unreachable("self-conformances never have associated types");
  }

  bool hasWitness(ValueDecl *requirement) const {
    return true;
  }
  Witness getWitness(ValueDecl *requirement) const;

  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const {
    return ArrayRef<Requirement>();
  }

  /// Get any additional requirements that are required for this conformance to
  /// be satisfied.
  ArrayRef<Requirement> getConditionalRequirements() const {
    return ArrayRef<Requirement>();
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Self;
  }
};

/// Specialized protocol conformance, which projects a generic protocol
/// conformance to one of the specializations of the generic type.
///
/// For example:
/// \code
/// protocol P { func foo() }
/// class A<T> : P { func foo() { } }
/// \endcode
///
/// \c A<T> conforms to \c P via normal protocol conformance. Any specialization
/// of \c A<T> conforms to \c P via a specialized protocol conformance. For
/// example, \c A<Int> conforms to \c P via a specialized protocol conformance
/// that refers to the normal protocol conformance \c A<T> to \c P with the
/// substitution \c T -> \c Int.
class SpecializedProtocolConformance : public ProtocolConformance,
                                       public llvm::FoldingSetNode {
  /// The generic conformance from which this conformance was derived.
  NormalProtocolConformance *GenericConformance;

  /// The substitutions applied to the generic conformance to produce this
  /// conformance.
  SubstitutionMap GenericSubstitutions;

  /// The mapping from associated type requirements to their substitutions.
  ///
  /// This mapping is lazily produced by specializing the underlying,
  /// generic conformance.
  mutable TypeWitnessMap TypeWitnesses;

  /// Any conditional requirements, in substituted form. (E.g. given Foo<T>: Bar
  /// where T: Bar, Foo<Baz<U>> will include Baz<U>: Bar.)
  mutable std::optional<ArrayRef<Requirement>> ConditionalRequirements;

  friend class ASTContext;

  SpecializedProtocolConformance(Type conformingType,
                                 NormalProtocolConformance *genericConformance,
                                 SubstitutionMap substitutions);

  void computeConditionalRequirements() const;

public:
  /// Get the generic conformance from which this conformance was derived,
  /// if there is one.
  NormalProtocolConformance *getGenericConformance() const {
    return GenericConformance;
  }

  /// Get the substitution map representing the substitutions used to produce
  /// this specialized conformance.
  SubstitutionMap getSubstitutionMap() const { return GenericSubstitutions; }

  /// Get any requirements that must be satisfied for this conformance to apply.
  ///
  /// If \c computeIfPossible is false, this will not do the lazy computation of
  /// the conditional requirements and will just query the current state. This
  /// should almost certainly only be used for debugging purposes, prefer \c
  /// getConditionalRequirementsIfAvailable (these are separate because
  /// CONFORMANCE_SUBCLASS_DISPATCH does some type checks and a defaulted
  /// parameter gets in the way of that).
  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailableOrCached(bool computeIfPossible) const {
    if (computeIfPossible)
      computeConditionalRequirements();
    return ConditionalRequirements;
  }
  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const {
    return getConditionalRequirementsIfAvailableOrCached(
        /*computeIfPossible=*/true);
  }

  /// Get any requirements that must be satisfied for this conformance to apply.
  ArrayRef<Requirement> getConditionalRequirements() const {
    return *getConditionalRequirementsIfAvailable();
  }

  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const {
    return GenericConformance->getProtocol();
  }

  /// Get the declaration context that contains the conforming extension or
  /// nominal type declaration.
  DeclContext *getDeclContext() const {
    return GenericConformance->getDeclContext();
  }

  /// Retrieve the state of this conformance.
  ProtocolConformanceState getState() const {
    return GenericConformance->getState();
  }

  /// Get the kind of source from which this conformance comes.
  ConformanceEntryKind getSourceKind() const {
    return GenericConformance->getSourceKind();
  }
  /// Get the protocol conformance which implied this implied conformance.
  NormalProtocolConformance *getImplyingConformance() const {
    return GenericConformance->getImplyingConformance();
  }

  bool hasTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Retrieve the type witness and type decl (if one exists)
  /// for the given associated type.
  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const;

  /// Given that the requirement signature of the protocol directly states
  /// that the given dependent type must conform to the given protocol,
  /// return its associated conformance.
  ProtocolConformanceRef
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const;

  /// Retrieve the witness corresponding to the given value requirement.
  ConcreteDeclRef getWitnessDeclRef(ValueDecl *requirement) const;

  /// Determine whether the witness for the given requirement
  /// is either the default definition or was otherwise deduced.
  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
    return GenericConformance->usesDefaultDefinition(requirement);
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getType(), getGenericConformance(), getSubstitutionMap());
  }

  static void Profile(llvm::FoldingSetNodeID &ID, Type type,
                      NormalProtocolConformance *genericConformance,
                      SubstitutionMap subs) {
    ID.AddPointer(type.getPointer());
    ID.AddPointer(genericConformance);
    subs.profile(ID);
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Specialized;
  }
};

/// Inherited protocol conformance, which projects the conformance of a
/// superclass to its subclasses.
///
/// An example:
/// \code
/// protocol P { func foo() }
/// class A : P { func foo() { } }
/// class B : A { }
/// \endcode
///
/// \c A conforms to \c P via normal protocol conformance. The subclass \c B
/// of \c A conforms to \c P via an inherited protocol conformance.
class InheritedProtocolConformance : public ProtocolConformance,
                                     public llvm::FoldingSetNode {
  /// The conformance inherited from the superclass.
  ProtocolConformance *InheritedConformance;

  friend class ASTContext;

  InheritedProtocolConformance(Type conformingType,
                               ProtocolConformance *inheritedConformance)
    : ProtocolConformance(ProtocolConformanceKind::Inherited, conformingType),
      InheritedConformance(inheritedConformance)
  {
  }

public:
  /// Retrieve the conformance for the inherited type.
  ProtocolConformance *getInheritedConformance() const {
    return InheritedConformance;
  }

  /// Get the conformance substitution map.
  SubstitutionMap getSubstitutionMap() const {
    return InheritedConformance->getSubstitutionMap();
  }

  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const {
    return InheritedConformance->getProtocol();
  }

  /// Get any requirements that must be satisfied for this conformance to apply.
  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const {
    return InheritedConformance->getConditionalRequirementsIfAvailable();
  }

  /// Get any requirements that must be satisfied for this conformance to apply.
  ArrayRef<Requirement> getConditionalRequirements() const {
    return InheritedConformance->getConditionalRequirements();
  }

  /// Get the declaration context that contains the conforming extension or
  /// nominal type declaration.
  DeclContext *getDeclContext() const {
    auto bgc = getType()->getClassOrBoundGenericClass();

    // In some cases, we may not have a BGC handy, in which case we should
    // delegate to the inherited conformance for the decl context.
    return bgc ? bgc : InheritedConformance->getDeclContext();
  }

  /// Retrieve the state of this conformance.
  ProtocolConformanceState getState() const {
    return InheritedConformance->getState();
  }

  /// Get the kind of source from which this conformance comes.
  ConformanceEntryKind getSourceKind() const {
    return ConformanceEntryKind::Inherited;
  }
  /// Get the protocol conformance which implied this implied conformance.
  NormalProtocolConformance *getImplyingConformance() const { return nullptr; }

  bool hasTypeWitness(AssociatedTypeDecl *assocType) const {
    return InheritedConformance->hasTypeWitness(assocType);
  }

  /// Retrieve the type witness and type decl (if one exists)
  /// for the given associated type.
  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const {
    return InheritedConformance->getTypeWitnessAndDecl(assocType, options);
  }

  /// Given that the requirement signature of the protocol directly states
  /// that the given dependent type must conform to the given protocol,
  /// return its associated conformance.
  ProtocolConformanceRef
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const;

  /// Retrieve the witness corresponding to the given value requirement.
  ConcreteDeclRef getWitnessDeclRef(ValueDecl *requirement) const;

  /// Determine whether the witness for the given requirement
  /// is either the default definition or was otherwise deduced.
  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
    return InheritedConformance->usesDefaultDefinition(requirement);
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getType(), getInheritedConformance());
  }

  static void Profile(llvm::FoldingSetNodeID &ID, Type type,
                      ProtocolConformance *inheritedConformance) {
    ID.AddPointer(type.getPointer());
    ID.AddPointer(inheritedConformance);
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Inherited;
  }
};

/// A builtin conformance appears when a non-nominal type has a
/// conformance that is synthesized by the implementation.
class BuiltinProtocolConformance final : public RootProtocolConformance {
  friend ASTContext;

  ProtocolDecl *protocol;

  BuiltinProtocolConformance(Type conformingType, ProtocolDecl *protocol,
                             BuiltinConformanceKind kind);

public:
  /// Get the protocol being conformed to.
  ProtocolDecl *getProtocol() const {
    return protocol;
  }

  BuiltinConformanceKind getBuiltinConformanceKind() const {
    return static_cast<BuiltinConformanceKind>(
        Bits.BuiltinProtocolConformance.Kind);
  }

  GenericSignature getGenericSignature() const {
    return GenericSignature();
  }

  /// Whether this represents a "missing" conformance that should be diagnosed
  /// later.
  bool isMissing() const {
    return getBuiltinConformanceKind() == BuiltinConformanceKind::Missing;
  }

  bool isInvalid() const {
    switch (getBuiltinConformanceKind()) {
    case BuiltinConformanceKind::Synthesized:
      return false;
    case BuiltinConformanceKind::Missing:
      return true;
    }
  }

  SourceLoc getLoc() const {
    return SourceLoc();
  }

  /// Get any requirements that must be satisfied for this conformance to apply.
  std::optional<ArrayRef<Requirement>>
  getConditionalRequirementsIfAvailable() const {
    return ArrayRef<Requirement>();
  }

  /// Get any requirements that must be satisfied for this conformance to apply.
  ArrayRef<Requirement> getConditionalRequirements() const {
    return {};
  }

  /// Get the declaration context that contains the nominal type declaration.
  DeclContext *getDeclContext() const {
    return getProtocol();
  }

  /// Retrieve the state of this conformance.
  ProtocolConformanceState getState() const {
    return ProtocolConformanceState::Complete;
  }

  /// Get the kind of source from which this conformance comes.
  ConformanceEntryKind getSourceKind() const {
    return ConformanceEntryKind::Synthesized;
  }
  /// Get the protocol conformance which implied this implied conformance.
  NormalProtocolConformance *getImplyingConformance() const {
    return nullptr;
  }

  bool hasTypeWitness(AssociatedTypeDecl *assocType) const {
    llvm_unreachable("builtin-conformances never have associated types");
  }

  bool hasWitness(ValueDecl *requirement) const {
    llvm_unreachable("builtin-conformances never have requirement witnesses");
  }

  /// Retrieve the type witness and type decl (if one exists)
  /// for the given associated type.
  TypeWitnessAndDecl
  getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                        SubstOptions options = std::nullopt) const {
    llvm_unreachable("builtin-conformances never have associated types");
  }

  Witness getWitness(ValueDecl *requirement) const {
    llvm_unreachable("builtin-conformances never have requirement witnesses");
  }

  /// Given that the requirement signature of the protocol directly states
  /// that the given dependent type must conform to the given protocol,
  /// return its associated conformance.
  ProtocolConformanceRef
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const {
    llvm_unreachable("builtin-conformances never have associated types");
  }

  /// Retrieve the witness corresponding to the given value requirement.
  ConcreteDeclRef getWitnessDeclRef(ValueDecl *requirement) const {
    return ConcreteDeclRef(requirement);
  }

  /// Determine whether the witness for the given requirement
  /// is either the default definition or was otherwise deduced.
  bool usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
    llvm_unreachable("builtin-conformances never have associated types");
  }

  static bool classof(const ProtocolConformance *conformance) {
    return conformance->getKind() == ProtocolConformanceKind::Builtin;
  }
};

inline bool ProtocolConformance::isInvalid() const {
  return getRootConformance()->isInvalid();
}

inline bool ProtocolConformance::hasWitness(ValueDecl *requirement) const {
  return getRootConformance()->hasWitness(requirement);
}

SourceLoc extractNearestSourceLoc(const ProtocolConformance *conf);
void simple_display(llvm::raw_ostream &out, const ProtocolConformance *conf);

} // end namespace swift

#endif // LLVM_SWIFT_AST_PROTOCOLCONFORMANCE_H

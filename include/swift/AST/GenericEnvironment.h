//===--- GenericEnvironment.h - Generic Environment AST ---------*- C++ -*-===//
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
// This file defines the GenericEnvironment class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_GENERIC_ENVIRONMENT_H
#define SWIFT_AST_GENERIC_ENVIRONMENT_H

#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/GenericParamKey.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/GenericSignature.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/UUID.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"
#include <utility>

namespace swift {

class ArchetypeType;
class ASTContext;
class GenericTypeParamType;
class OpaqueTypeDecl;
class ElementArchetypeType;
class OpenedArchetypeType;
class PackArchetypeType;
class PackExpansionType;
class SILModule;
class SILType;
template <class> class CanTypeWrapper;

/// Query function suitable for use as a \c TypeSubstitutionFn that queries
/// the mapping of interface types to archetypes.
class QueryInterfaceTypeSubstitutions {
  const GenericEnvironment *self;
  
public:
  QueryInterfaceTypeSubstitutions(const GenericEnvironment *self)
  : self(self) { }
  
  Type operator()(SubstitutableType *type) const;
};

/// Extra data in a generic environment for an opaque type.
struct OpaqueEnvironmentData {
  OpaqueTypeDecl *decl;
  SubstitutionMap subMap;
};

/// Extra data in a generic environment for an opened existential.
struct OpenedExistentialEnvironmentData {
  Type existential;
  GenericSignature parentSig;
  UUID uuid;
};

/// Extra data in a generic environment for an opened pack element.
struct OpenedElementEnvironmentData {
  UUID uuid;
  CanGenericTypeParamType shapeClass;
  SubstitutionMap outerSubstitutions;
};

/// Describes the mapping between archetypes and interface types for the
/// generic parameters of a DeclContext.
///
/// The most frequently used method here is mapTypeIntoContext(), which
/// maps an interface type to a type written in terms of the generic
/// environment's archetypes; to go in the other direction, use
/// TypeBase::mapTypeOutOfContext().
///
class alignas(1 << DeclAlignInBits) GenericEnvironment final
    : private llvm::TrailingObjects<
        GenericEnvironment,
        OpaqueEnvironmentData,
        OpenedExistentialEnvironmentData,
        OpenedElementEnvironmentData,
        Type> {
public:
  enum class Kind {
    /// A normal generic environment, determined only by its generic
    /// signature.
    Primary,
    /// A generic environment describing an opaque type archetype.
    Opaque,
    /// A generic environment describing an opened existential archetype.
    OpenedExistential,
    /// A generic environment describing an opened element type of a
    /// pack archetype inside a pack expansion expression.
    OpenedElement,
  };

  class NestedTypeStorage;

private:
  mutable llvm::PointerIntPair<GenericSignature, 2, Kind> SignatureAndKind{
      GenericSignature(), Kind::Primary};
  NestedTypeStorage *nestedTypeStorage = nullptr;

  friend TrailingObjects;
  friend OpaqueTypeArchetypeType;

  size_t numTrailingObjects(OverloadToken<OpaqueEnvironmentData>) const;
  size_t numTrailingObjects(OverloadToken<OpenedExistentialEnvironmentData>) const;
  size_t numTrailingObjects(OverloadToken<OpenedElementEnvironmentData>) const;
  size_t numTrailingObjects(OverloadToken<Type>) const;

  /// Retrieve the array containing the context types associated with the
  /// generic parameters, stored in parallel with the generic parameters of the
  /// generic signature.
  MutableArrayRef<Type> getContextTypes();

  /// Retrieve the array containing the context types associated with the
  /// generic parameters, stored in parallel with the generic parameters of the
  /// generic signature.
  ArrayRef<Type> getContextTypes() const;

  /// Retrieve the array of opened pack parameters for this opened-element
  /// environment.  This is parallel to the array of element parameters,
  /// i.e. the innermost generic context.
  MutableArrayRef<Type> getOpenedPackParams();
  ArrayRef<Type> getOpenedPackParams() const;

  /// Get the nested type storage, allocating it if required.
  NestedTypeStorage &getOrCreateNestedTypeStorage();

  /// Private constructor for primary environments.
  explicit GenericEnvironment(GenericSignature signature);

  /// Private constructor for opaque type environments.
  explicit GenericEnvironment(
      GenericSignature signature, OpaqueTypeDecl *opaque, SubstitutionMap subs);

  /// Private constructor for opened existential environments.
  explicit GenericEnvironment(
      GenericSignature signature,
      Type existential, GenericSignature parentSig, UUID uuid);

  /// Private constructor for opened element environments.
  explicit GenericEnvironment(GenericSignature signature,
                              UUID uuid,
                              CanGenericTypeParamType shapeClass,
                              SubstitutionMap outerSubs);

  friend ArchetypeType;
  friend QueryInterfaceTypeSubstitutions;

  Type getOrCreateArchetypeFromInterfaceType(Type depType);

  /// Add a mapping of a generic parameter to a specific type (which may be
  /// an archetype)
  void addMapping(GenericParamKey key, Type contextType);

  /// Retrieve the mapping for the given generic parameter, if present.
  ///
  /// This is only useful when lazily populating a generic environment.
  llvm::Optional<Type> getMappingIfPresent(GenericParamKey key) const;

public:
  GenericSignature getGenericSignature() const {
    return SignatureAndKind.getPointer();
  }

  Kind getKind() const { return SignatureAndKind.getInt(); }

  ArrayRef<GenericTypeParamType *> getGenericParams() const;

  /// Retrieve the existential type for an opened existential environment.
  Type getOpenedExistentialType() const;

  /// Retrieve the UUID for an opened existential environment.
  UUID getOpenedExistentialUUID() const;

  /// Retrieve the parent signature for an opened existential environment.
  GenericSignature getOpenedExistentialParentSignature() const;

  /// Retrieve the opaque type declaration for a generic environment describing
  /// opaque types.
  OpaqueTypeDecl *getOpaqueTypeDecl() const;

  /// Retrieve the substitutions applied to an opaque type declaration to
  /// create a generic environment.
  SubstitutionMap getOpaqueSubstitutions() const;

  /// Retrieve the substitutions for the outer generic parameters of an
  /// opened pack element generic environment.
  SubstitutionMap getPackElementContextSubstitutions() const;

  /// Retrieve the shape equivalence class for an opened element environment.
  /// This is always a pack parameter.
  CanGenericTypeParamType getOpenedElementShapeClass() const;

  /// Retrieve the UUID for an opened element environment.
  UUID getOpenedElementUUID() const;

  /// Return the number of opened pack parameters.
  unsigned getNumOpenedPackParams() const;

  void forEachPackElementArchetype(
          llvm::function_ref<void(ElementArchetypeType*)> function) const;

  void forEachPackElementGenericTypeParam(
      llvm::function_ref<void(GenericTypeParamType *)> function) const;

  using PackElementBindingCallback =
    llvm::function_ref<void(ElementArchetypeType *elementType,
                            PackType *packSubstitution)>;

  /// Given that this is an opened element environment, iterate the
  /// opened pack element bindings: the pack archetype that's been opened
  /// (which may not be meaningful in the surrounding context), the element
  /// archetype that it has been opened as, and the pack type whose elements
  /// are opened.
  void forEachPackElementBinding(PackElementBindingCallback function) const;

  /// Create a new, primary generic environment.
  static GenericEnvironment *forPrimary(GenericSignature signature);

  /// Create a new generic environment for an opaque type with the given set of
  /// outer substitutions.
  static GenericEnvironment *forOpaqueType(
      OpaqueTypeDecl *opaque, SubstitutionMap subs);

  /// Create a new generic environment for an opened existential.
  ///
  /// \param existential The subject existential type
  /// \param parentSig The signature of the context where this existential type is being opened
  /// \param uuid The unique identifier for this opened existential
  static GenericEnvironment *
  forOpenedExistential(Type existential, GenericSignature parentSig, UUID uuid);

  /// Create a new generic environment for an opened element.
  ///
  /// \param signature The opened element signature, which is the same as the
  /// signature of the context whose element type is being opened, but with
  /// the pack parameter bit erased from one or more generic parameters
  /// \param uuid The unique identifier for this opened element
  /// \param shapeClass The shape equivalence class for the originating packs
  /// \param outerSubs The substitution map containing archetypes from the
  /// outer generic context.
  static GenericEnvironment *
  forOpenedElement(GenericSignature signature,
                   UUID uuid, CanGenericTypeParamType shapeClass,
                   SubstitutionMap outerSubs);

  /// Make vanilla new/delete illegal.
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) = delete;

  /// Only allow placement new.
  void *operator new(size_t Bytes, void *Mem) {
    assert(Mem); 
    return Mem; 
  }

  /// For an opaque or pack element archetype environment, apply the
  /// substitutions.
  Type maybeApplyOuterContextSubstitutions(Type type) const;

  /// Compute the canonical interface type within this environment.
  Type getCanonicalInterfaceType(Type interfaceType);

  /// Map an interface type to a contextual type.
  static Type mapTypeIntoContext(GenericEnvironment *genericEnv,
                                 Type type);

  /// Map an interface type to a contextual type.
  Type mapTypeIntoContext(Type type) const;

  /// Map an interface type to a contextual type.
  Type mapTypeIntoContext(Type type,
                          LookupConformanceFn lookupConformance) const;

  /// Map a generic parameter type to a contextual type.
  Type mapTypeIntoContext(GenericTypeParamType *type) const;

  /// Map an interface type containing parameter packs to a contextual
  /// type in the opened element generic context.
  Type mapPackTypeIntoElementContext(Type type) const;

  /// Map a contextual type containing parameter packs to a contextual
  /// type in the opened element generic context.
  ///
  /// This operation only makes sense if the generic environment that the
  /// pack archetypes are contextual in matches the generic signature
  /// of this environment.  That will be true for opened element
  /// environments coming straight out of the type checker, such as
  /// the one in a PackExpansionExpr, or opened element environments
  /// created directly from the current environment.  It is not
  /// reliable for opened element environments in arbitrary SIL functions.
  Type mapContextualPackTypeIntoElementContext(Type type) const;

  /// Map a contextual type containing parameter packs to a contextual
  /// type in the opened element generic context.
  ///
  /// This operation only makes sense if the generic environment that the
  /// pack archetypes are contextual in matches the generic signature
  /// of this environment.  That will be true for opened element
  /// environments coming straight out of the type checker, such as
  /// the one in a PackExpansionExpr, or opened element environments
  /// created directly from the current environment.  It is not
  /// reliable for opened element environments in arbitrary SIL functions.
  CanType mapContextualPackTypeIntoElementContext(CanType type) const;

  /// Map a type containing pack element type parameters to a contextual
  /// type in the pack generic context.
  Type mapElementTypeIntoPackContext(Type type) const;

  /// Map the given SIL interface type to a contextual type.
  ///
  /// This operation will also reabstract dependent types according to the
  /// abstraction level of their associated type requirements.
  SILType mapTypeIntoContext(SILModule &M, SILType type) const;

  /// Map an interface type's protocol conformance into the corresponding
  /// conformance for the contextual type.
  static std::pair<Type, ProtocolConformanceRef>
  mapConformanceRefIntoContext(GenericEnvironment *genericEnv,
                               Type conformingType,
                               ProtocolConformanceRef conformance);

  /// Map an interface type's protocol conformance into the corresponding
  /// conformance for the contextual type.
  std::pair<Type, ProtocolConformanceRef>
  mapConformanceRefIntoContext(Type conformingType,
                               ProtocolConformanceRef conformance) const;

  /// Returns a substitution map that sends every generic parameter to its
  /// corresponding archetype in this generic environment.
  SubstitutionMap getForwardingSubstitutionMap() const;

  void dump(raw_ostream &os) const;

  SWIFT_DEBUG_DUMP;
};

/// A pair of an opened-element generic signature and an opened-element
/// generic environment.
struct OpenedElementContext {
  /// The opened-element environment for this expansion.
  GenericEnvironment *environment;

  /// The opened-element signature for this expansion.
  CanGenericSignature signature;

  /// Create a fresh opened element context from a contextual pack
  /// expansion type.  This is useful when writing code that needs to
  /// break down the components of a pack expansion.
  static OpenedElementContext
  createForContextualExpansion(ASTContext &ctx,
                   CanTypeWrapper<PackExpansionType> expansionType);
};
  
} // end namespace swift

#endif // SWIFT_AST_GENERIC_ENVIRONMENT_H


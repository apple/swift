//===--- MetadataRequest.h - Operations for accessing metadata --*- C++ -*-===//
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
// This file defines some types and operations for accessing type metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_METADATAREQUEST_H
#define SWIFT_IRGEN_METADATAREQUEST_H

#include "swift/ABI/MetadataValues.h"
#include "swift/AST/Types.h"

namespace llvm {
class Constant;
class Function;
class GlobalVariable;
class PHINode;
class Type;
class Value;
}

namespace swift {
enum ForDefinition_t : bool;

namespace irgen {
class ConstantReference;
class Explosion;
struct GenericArguments;
class IRGenFunction;
class IRGenModule;
class MetadataDependencyCollector;
class MetadataResponse;
enum class OperationCost : unsigned;
enum class SymbolReferenceKind : unsigned char;

/// Given the metadata state for a generic type metadata, return the presumed
/// state of type metadata for its arguments.
inline MetadataState
getPresumedMetadataStateForTypeArgument(MetadataState typeState) {
  return (typeState == MetadataState::Complete
            ? MetadataState::Complete
            : MetadataState::Abstract);
}

/// The compile-time representation of a metadata request.  There are
/// essentially three patterns of use here:
///
/// - In the first pattern, the request is static and blocking, meaning
///   that the operation which produces the metadata should not return
///   until it's able to return a metadata that's in the requested state.
///   Most places in IRGen will use blocking requests, often for complete
///   metadata, because operations like allocating a value or passing a
///   metadata off as a generic parameter (or metatype value) (1) cannot be
///   performed without the metadata in the right state but also (2) cannot
///   be suspended to await the production of the metadata.
///
/// - In the second pattern, the request is static and non-blocking, and
///   there is a dependency collector associated with the request.  This is
///   used in the special case of a type metadata's initialization phase.
///   This phase can fail, but when it does so, it must report a type
///   metadata (and its requested state) that it is blocked on.  The runtime
///   will then delay the initialization of that type metadata until its
///   dependency is resolved.
///
/// - In the final pattern, the request is a dynamic value.  This is used
///   primarily in type metadata access functions like the global
///   access functions for generic types or the associated type access
///   functions on protocol witness tables.  These functions must report
///   the actual best-known dynamic state of the metadata that they return.
///   Access functions that memoize their result must therefore suppress the
///   memoization when the metadata is not yet complete.
class DynamicMetadataRequest {
  MetadataRequest StaticRequest;
  llvm::Value *DynamicRequest = nullptr;
  MetadataDependencyCollector *Dependencies = nullptr;
public:
  /// Create a blocking request for the given metadata state.
  DynamicMetadataRequest(MetadataState request)
    : DynamicMetadataRequest(MetadataRequest(request)) {}

  /// Create a request for the given static request.
  ///
  /// Note that non-blocking requests will generally just propagate out
  /// in the MetadataResponse, treated as statically abstract, unless
  /// there's a dependency collector installed.
  DynamicMetadataRequest(MetadataRequest request)
    : StaticRequest(request), DynamicRequest(nullptr) {}

  /// Create a request for the given dynamic request.
  explicit DynamicMetadataRequest(llvm::Value *request)
    : StaticRequest(), DynamicRequest(request) {}

  /// If a collector is provided, create a non-blocking request that
  /// will branch to the collector's destination if the response doesn't
  /// satisfy the request.  Otherwise, use a blocking request.
  ///
  /// FIXME: ensure the existence of a collector in all the places that
  /// need one.
  static DynamicMetadataRequest
  getNonBlocking(MetadataState requiredState,
                 MetadataDependencyCollector *collector) {
    if (!collector) return requiredState;

    DynamicMetadataRequest request =
      MetadataRequest(requiredState, /*non-blocking*/ true);
    request.Dependencies = collector;
    return request;
  }

  bool isStatic() const { return DynamicRequest == nullptr; }
  MetadataRequest getStaticRequest() const {
    assert(isStatic());
    return StaticRequest;
  }

  llvm::Value *getDynamicRequest() const {
    assert(!isStatic());
    return DynamicRequest;
  }

  MetadataDependencyCollector *getDependencyCollector() const {
    return Dependencies;
  }

  /// If a function call taking this request returns a MetadataResponse,
  /// can the status of a MetadataResponse be generally ignored?
  bool canResponseStatusBeIgnored() const {
    // If we have a statically satisfied request, it cannot have failed.
    // If have a dependency collector, we'll have checked the result when
    // first forming the MetadataResponse.
    return isStaticallyAlwaysSatisfied() || Dependencies != nullptr;
  }

  /// Is this request statically known to yield a result that satisfies
  /// the request?
  bool isStaticallyAlwaysSatisfied() const {
    return isStatic() &&
           (StaticRequest.isBlocking() ||
            StaticRequest.getState() == MetadataState::Abstract);
  }

  /// Is this request statically known to yield a result that's fully
  /// completed?
  bool isStaticallyBlockingComplete() const {
    return isStatic() && StaticRequest == MetadataState::Complete;
  }

  /// Is this request statically known to be an abstract request?
  bool isStaticallyAbstract() const {
    return isStatic() && StaticRequest == MetadataState::Abstract;
  }

  /// Does state of the given kind definitely satisfy this request?
  bool isSatisfiedBy(MetadataState state) const {
    return isAtLeast(state, getStaticUpperBoundOnRequestedState());
  }

  /// Is the given metadata response statically known to satisfy this request?
  bool isSatisfiedBy(MetadataResponse response) const;

  /// Return a conservative bound on the state being requested by this
  /// request.
  MetadataState getStaticUpperBoundOnRequestedState() const {
    return (isStatic() ? StaticRequest.getState() : MetadataState::Complete);
  }

  /// Return a conservative bound on the result of a MetadataResponse
  /// arising from this request.
  MetadataState getStaticLowerBoundOnResponseState() const {
    // If we have a static request, and it's either blocking or we're
    // collecting failures as dependencies immediately, then we can use
    // the static request's kind.
    //
    // The dependency-collection rule works because the emitter is obliged
    // to deal with dominance points appropriately if it's going to try to
    // recover after the dependency collection.
    if (isStatic() && canResponseStatusBeIgnored())
      return StaticRequest.getState();

    // Otherwise, the response can't be assumed to be better than abstract.
    return MetadataState::Abstract;
  }

  /// Return this request value as an IR value.
  llvm::Value *get(IRGenFunction &IGF) const;

  /// Project the required state (the basic kind) of this request as
  /// an IR value.
  llvm::Value *getRequiredState(IRGenFunction &IGF) const;
};

/// A response to a metadata request (but see below for other ways in which
/// this type is used).  In addition to a type metadata reference, the response
/// includes static and dynamic information about the state of the metadata.
///
/// A type metadata technically goes through a progression of phases.  Many
/// metadata just start in the final "complete" phase, but it's important
/// to understand this phase progression in order to understand what happens
/// in the more complex cases.
///
/// - The first phase of the metadata is called "allocation", and it ensures
///   that there is exactly one metadata for a particular type.  We don't
///   need to say anything else about metadata allocation here, however,
///   because a metadata object in this phase is never exposed outside of
///   the runtime (and narrow confines of the metadata allocation function).
///   This is also why MetadataState does not have an enumerator for this state.
///
/// - Once a metadata has been allocated, it is considered to be "abstract".
///   An abstract metadata only has enough structure to represent its basic
///   identity as a type: its type kind and the basic components of its
///   identity.  For example, A<T> will always store the type descriptor
///   for A and the type metadata for T, but it does not necessarily have
///   anything else, like a value witness table or a superclass reference.
///
/// - A metadata is said to be "layout-complete" if it has a meaningful
///   "external layout", the principal component of which is a meaningful
///   value witness table.  Such a metadata allows other types which store
///   values of the metadata's type to be laid out.  However, there may still
///   be restrictions on the use of the metadata which make it unready
///   for general use.
///
///   Some types may have no observable abstract phase.  For example, all
///   class types are layout-complete immediately upon allocation, as are
///   value types with a fixed layout for all instantiations.
///
/// - A metadata is said to be "complete" if it supports all possible
///   operations on the type.  This includes any necessary "internal layout",
///   such as the field layout of a class type, as well as any sundry
///   initialization tasks like the registration of a class type with the
///   Objective-C runtime.  This is also the first phase of a class metadata
///   that promises that it has a superclass pointer.  Furthermore, some
///   types make stronger transitive guarantees about the state of their
///   stored component types when they are complete than when they are in
///   earlier stages.
///
/// --
///
/// A MetadataResponse stores two pieces of information about its state.
///
/// The first piece of information is a static lower bound (i.e. a
/// conservative estimate) on the state.  This will always be at least
/// MetadataState::Abstract, since no metadata in general circulation can
/// still be undergoing allocation.  It may be higher than that because of
/// static information about the request or the type being requested.
///
/// The second piece of information is the dynamic state of the metadata.
/// When metadata is fetched from the runtime, the runtime will report
/// its current known state.  We cache this state and generally assume that
/// nothing in the function will cause it to change.  This is okay because:
///
/// - with blocking requests, we will generally ignore the dynamic state
///   and instead just block waiting for the runtime to report that the
///   metadata has reached the requested state, whereas
///
/// - with non-blocking and dynamic requests, ephemeral staleness is
///   recoverable --- in fact, it has to be, because of course as soon as
///   the runtime has finished reporting a state, that state has become
///   potentially stale due to concurrent initialization).
///
/// Other than fetching metadata from the runtime, most of the ways that
/// we produce metadata guarantee that the metadata is complete.  For example,
/// fixed-layout non-generic value types just provide complete metadata at
/// known global symbols.  There are a few rare exceptions where we do need
/// pull metadata from somewhere but don't actually know its dynamic state.
/// For example, it would be unwise to try to store the dynamic state of
/// a generic type argument in the generic type's metadata; unlike the
/// arguments above, such an approach would be highly non-ephemeral and
/// create substantial staleness problems.  For these situations, the
/// runtime provides a function, swift_checkMetadataState, to request (or
/// block on) the current state of a type metadata.
///
/// --
/// 
/// While this class is principally used to report the response to a type
/// metadata request, it does have some secondary uses.
///
/// The first secondary use is that it is sometimes used to store other
/// forms of local type data so that common code structures can work with
/// both them and type metadata.  For example, concrete local type data are
/// stored in a MetadataResponse even though they aren't always type metadata.
/// When this is happening, the metadata state will always be statically
/// complete.
///
/// The second secondary use is that it's sometimes used to just store the
/// known state of a fetched metadata in a more persistent way.  This is
/// because many of the places that store metadata have at least some case
/// where it's important to known either the static bound on the metadata
/// state or, more importantly, to report the actual dynamic state that was
/// reported when the metadata was first acquired.  Propagating the metadata
/// as a MetadataResponse makes this straightforward.
class MetadataResponse {
  llvm::Value *Metadata;
  llvm::Value *DynamicState;
  MetadataState StaticState;

public:
  MetadataResponse() : Metadata(nullptr) {}

  /// A metadata response that might not be dynamically complete.
  explicit MetadataResponse(llvm::Value *metadata, llvm::Value *state,
                            MetadataState staticLowerBoundState)
      : Metadata(metadata), DynamicState(state),
        StaticState(staticLowerBoundState) {
    assert(metadata && "must be valid");
  }

  /// A metadata response whose actual dynamic state is unknown but for
  /// which we do have a static lower-bound.
  static MetadataResponse forBounded(llvm::Value *metadata,
                                     MetadataState staticLowerBoundState) {
    return MetadataResponse(metadata, nullptr, staticLowerBoundState);
  }

  /// A metadata response that's known to be complete.
  static MetadataResponse forComplete(llvm::Value *metadata) {
    return MetadataResponse::forBounded(metadata, MetadataState::Complete);
  }

  /// An undef metadata response.
  static MetadataResponse getUndef(IRGenFunction &IGF);

  bool isValid() const { return Metadata != nullptr; }
  explicit operator bool() const { return isValid(); }

  bool isStaticallyKnownComplete() const {
    assert(isValid());
    return getStaticLowerBoundOnState() == MetadataState::Complete;
  }

  llvm::Value *getMetadata() const {
    assert(isValid());
    return Metadata;
  }

  /// Does this response have a dynamic state value?
  bool hasDynamicState() const {
    assert(isValid());
    return DynamicState != nullptr;
  }

  /// Ensure that this response has a dynamic state value, by re-checking it
  /// if necessary.
  void ensureDynamicState(IRGenFunction &IGF) &;

  llvm::Value *getDynamicState() const {
    assert(isValid());
    assert(hasDynamicState() && "must ensure dynamic state before fetching it");
    return DynamicState;
  }

  /// Return the best lower bound state on the state of this metadata.
  MetadataState getStaticLowerBoundOnState() const {
    assert(isValid());
    return StaticState;
  }

  static MetadataResponse handle(IRGenFunction &IGF,
                                 DynamicMetadataRequest request,
                                 llvm::Value *responsePair);
  llvm::Value *combine(IRGenFunction &IGF) const;

  /// Return a constant value representing the fully-completed state
  /// (MetadataState::Complete).
  static llvm::Constant *getCompletedState(IRGenModule &IGM);
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const MetadataResponse &MR) {
  if (!MR.isValid())
    return OS;
  OS << MR.getMetadata();
  if (MR.hasDynamicState())
    OS << MR.getDynamicState();
  // FIXME
  // OS << MR.getStaticLowerBoundOnState();
  return OS;
}

inline bool
DynamicMetadataRequest::isSatisfiedBy(MetadataResponse response) const {
  return isSatisfiedBy(response.getStaticLowerBoundOnState());
}

/// A dependency that is blocking a metadata initialization from completing.
class MetadataDependency {
  llvm::Value *RequiredMetadata;
  llvm::Value *RequiredState;
public:
  /// Construct the null dependency, i.e. the initialization is not blocked.
  constexpr MetadataDependency()
      : RequiredMetadata(nullptr), RequiredState(nullptr) {}

  /// Construct a non-trivial dependency.
  MetadataDependency(llvm::Value *requiredMetadata,
                     llvm::Value *requiredState)
      : RequiredMetadata(requiredMetadata), RequiredState(requiredState) {
    assert(requiredMetadata != nullptr);
    assert(requiredState != nullptr);
  }

  bool isTrivial() const { return RequiredMetadata == nullptr; }
  bool isNonTrivial() const { return RequiredMetadata != nullptr; }
  explicit operator bool() const { return isNonTrivial(); }

  /// Return the metadata that the initialization depends on.
  llvm::Value *getRequiredMetadata() const {
    assert(isNonTrivial());
    return RequiredMetadata;
  }

  /// Return the state that the metadata needs to reach before the
  /// initialization is unblocked.
  llvm::Value *getRequiredState() const {
    assert(isNonTrivial());
    return RequiredState;
  }

  static llvm::Constant *getTrivialCombinedDependency(IRGenModule &IGM);

  llvm::Value *combine(IRGenFunction &IGF) const;
};

/// A class for dynamically collecting metadata dependencies.
class MetadataDependencyCollector {
  llvm::PHINode *RequiredMetadata = nullptr;
  llvm::PHINode *RequiredState = nullptr;

public:
  MetadataDependencyCollector() = default;
  MetadataDependencyCollector(const MetadataDependencyCollector &) = delete;
  MetadataDependencyCollector &operator=(const MetadataDependencyCollector &) = delete;
  ~MetadataDependencyCollector() {
    assert(RequiredMetadata == nullptr &&
           "failed to finish MetadataDependencyCollector");
  }

  /// Given the result of fetching metadata, check whether it creates a
  /// metadata dependency, and branch if so.
  ///
  /// This takes a metadata and state separately instead of taking a
  /// MetadataResponse pair because it's quite important that we not rely on
  /// anything from MetadataResponse that might assume that we've already
  /// done dependency collection.
  void checkDependency(IRGenFunction &IGF, DynamicMetadataRequest request,
                       llvm::Value *metadata, llvm::Value *state);

  /// Given an optional MetadataDependency value (e.g. the result of calling
  /// a dependency-returning function, in which a dependency is signalled
  /// by a non-null metadata value), check whether it indicates a dependency
  /// and branch if so.
  void collect(IRGenFunction &IGF, llvm::Value *dependencyPair);

  MetadataDependency finish(IRGenFunction &IGF);

private:
  void emitCheckBranch(IRGenFunction &IGF, llvm::Value *satisfied,
                       llvm::Value *metadata, llvm::Value *requiredState);
};

enum class MetadataAccessStrategy {
  /// There is a unique public accessor function for the given type metadata.
  PublicUniqueAccessor,

  /// There is a unique hidden accessor function for the given type metadata.
  HiddenUniqueAccessor,

  /// There is a unique private accessor function for the given type metadata.
  PrivateAccessor,

  /// The given type metadata is for a foreign type; its accessor function
  /// is built as a side-effect of emitting a metadata candidate.
  ForeignAccessor,

  /// There is no unique accessor function for the given type metadata, but
  /// one should be made automatically.
  NonUniqueAccessor
};

/// Does the given access strategy rely on an accessor that's generated
/// on-demand and thus may be shared across object files?
static inline bool isAccessorLazilyGenerated(MetadataAccessStrategy strategy) {
  switch (strategy) {
  case MetadataAccessStrategy::PublicUniqueAccessor:
  case MetadataAccessStrategy::HiddenUniqueAccessor:
  case MetadataAccessStrategy::PrivateAccessor:
    return false;
  case MetadataAccessStrategy::ForeignAccessor:
  case MetadataAccessStrategy::NonUniqueAccessor:
    return true;
  }
  llvm_unreachable("bad kind");
}

/// Is non-canonical complete metadata for the given type available at a fixed
/// address?
bool isNoncanonicalCompleteTypeMetadataStaticallyAddressable(IRGenModule &IGM,
                                                             CanType type);
/// Is canonical complete metadata for the given type available at a fixed
/// address?
bool isCanonicalCompleteTypeMetadataStaticallyAddressable(IRGenModule &IGM,
                                                          CanType type);
/// Should requests for the given type's metadata be cached?
bool shouldCacheTypeMetadataAccess(IRGenModule &IGM, CanType type);

enum SpecializedMetadataUsageIsOnlyFromAccessor : bool {
  /// The metadata must be accessed through an accessor function so that it can
  /// be initialized.
  ForUseOnlyFromAccessor = true,
  /// The metadata may be accessed directly.
  NotForUseOnlyFromAccessor = false
};

enum SpecializedMetadataCanonicality : bool {
  /// The metadata is canonical and can be used directly (subject to
  /// initialization).
  CanonicalSpecializedMetadata = true,
  /// The metadata is not canonical and must be canonicalized before usage.
  NoncanonicalSpecializedMetadata = false
};

/// Is the address of a specialization of the generic metadata statically known?
///
/// In other words, can a specialization be formed for the specified type.
///
/// If onlyFromAccessor is ForUseOnlyFromAccessor, then metadata's address is
/// known, but access to the metadata must go through the canonical specialized
/// accessor so that initialization of the metadata can occur.
bool isSpecializedNominalTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, NominalTypeDecl &nominal, CanType type,
    SpecializedMetadataCanonicality canonicality,
    SpecializedMetadataUsageIsOnlyFromAccessor onlyFromAccessor);

/// Is the address of a specialization of the generic metadata which does not
/// require runtime initialization statically known?
bool isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, NominalTypeDecl &nominal, CanType type,
    SpecializedMetadataCanonicality canonicality);

/// Is the address of canonical metadata which may need to be initialized (e.g.
/// by registering it with the Objective-C runtime) statically known?
bool isCanonicalInitializableTypeMetadataStaticallyAddressable(IRGenModule &IGM,
                                                               CanType type);

/// Determine how the given type metadata should be accessed.
MetadataAccessStrategy getTypeMetadataAccessStrategy(CanType type);

/// Return the address of a function that will return type metadata 
/// for the given non-dependent type.
llvm::Function *getOrCreateTypeMetadataAccessFunction(IRGenModule &IGM,
                                                      CanType type);

/// Return the type metadata access function for the given type, given that
/// some other code will be defining it.
llvm::Function *
getOtherwiseDefinedTypeMetadataAccessFunction(IRGenModule &IGM, CanType type);

/// Emit a type metadata access function that just directly accesses
/// the metadata.
llvm::Function *
createDirectTypeMetadataAccessFunction(IRGenModule &IGM, CanType type,
                                       bool allowExistingDefinition);

using MetadataAccessGenerator =
  llvm::function_ref<MetadataResponse(IRGenFunction &IGF,
                                      DynamicMetadataRequest request,
                                      llvm::Constant *cache)>;

enum class CacheStrategy {
  /// No cache.
  None,

  /// A simple lazy cache.
  Lazy,

  /// An SingletonMetadataCache initialization cache.
  SingletonInitialization,
};

/// Emit a type metadata access function using the given generator function.
llvm::Function *
createTypeMetadataAccessFunction(IRGenModule &IGM,
                                 CanType type,
                                 CacheStrategy cacheStrategy,
                                 MetadataAccessGenerator generator,
                                 bool allowExistingDefinition = false);

/// Either create or return a reference to a generic type metadata
/// access function.
///
/// Note that a generic type metadata access function is a somewhat
/// different kind of thing from an ordinary type metadata access function:
///
///   - A generic type metadata access function is associated with a type
///     object of kind `(...) -> type` --- typically, an unapplied
///     generic type (like `Dictionary`, without any type arguments).
///     It takes the concrete witnesses to the generic signature as
///     parameters and builds an appropriately instantiated type for those
///     arguments.
///
///   - An ordinary type metadata access function is associated with
///     a type object of kind `type` --- which is to say, an ordinary type
///     like `Float` or `Dictionary<String, Int>`.  There may be ordinary
///     access functions for various specializations of generic types;
///     these will be created on demand.
///
/// The definitions of generic type metadata access functions currently
/// always follow the same pattern, so we don't need to take a closure to
/// define the body.
llvm::Function *
getGenericTypeMetadataAccessFunction(IRGenModule &IGM,
                                     NominalTypeDecl *nominal,
                                     ForDefinition_t shouldDefine);

using CacheEmitter =
  llvm::function_ref<MetadataResponse(IRGenFunction &IGF, Explosion &params)>;

/// Emit the body of a lazy cache access function.
void emitCacheAccessFunction(IRGenModule &IGM, llvm::Function *accessor,
                             llvm::Constant *cache, llvm::Type *cacheTy,
                             CacheStrategy cacheStrategy, CacheEmitter getValue,
                             bool isReadNone = true);
MetadataResponse
emitGenericTypeMetadataAccessFunction(IRGenFunction &IGF, Explosion &params,
                                      NominalTypeDecl *nominal,
                                      GenericArguments &genericArgs);

MetadataResponse emitCanonicalSpecializedGenericTypeMetadataAccessFunction(
    IRGenFunction &IGF, Explosion &params, CanType theType);

/// Emit a declaration reference to a metatype object.
void emitMetatypeRef(IRGenFunction &IGF, CanMetatypeType type,
                     Explosion &explosion);

/// Emit a reference to a compile-time constant piece of type metadata, or
/// return a null pointer if the type's metadata cannot be represented by a
/// constant.
ConstantReference tryEmitConstantTypeMetadataRef(IRGenModule &IGM,
                                                 CanType type,
                                                 SymbolReferenceKind refKind);

/// Emit a reference to a compile-time constant piece of heap metadata, or
/// return a null pointer if the type's heap metadata cannot be represented
/// by a constant.
llvm::Constant *tryEmitConstantHeapMetadataRef(IRGenModule &IGM,
                                               CanType type,
                                               bool allowUninitialized);

enum class MetadataValueType { ObjCClass, TypeMetadata };

/// Emit a reference to the heap metadata for a class.
///
/// Only requests whose response status can be ignored can be used.
///
/// \returns a value of type ObjCClassPtrTy or TypeMetadataPtrTy,
///    depending on desiredType
llvm::Value *emitClassHeapMetadataRef(IRGenFunction &IGF, CanType type,
                                      MetadataValueType desiredType,
                                      DynamicMetadataRequest request,
                                      bool allowUninitialized = false);

/// Emit a reference to the (initialized) ObjC heap metadata for a class.
///
/// \returns a value of type ObjCClassPtrTy
llvm::Value *emitObjCHeapMetadataRef(IRGenFunction &IGF, ClassDecl *theClass,
                                     bool allowUninitialized = false);

/// Emit a reference to type metadata corresponding to the given
/// heap metadata.  This may be ObjCWrapper metadata if the heap metadata
/// is not a class.
llvm::Value *emitObjCMetadataRefForMetadata(IRGenFunction &IGF,
                                            llvm::Value *classPtr);

/// Given a class metadata reference, produce the appropriate heap
/// metadata reference for it.
llvm::Value *emitClassHeapMetadataRefForMetatype(IRGenFunction &IGF,
                                                 llvm::Value *metatype,
                                                 CanType type);

/// Emit a reference to a type layout record for the given type. The referenced
/// data is enough to lay out an aggregate containing a value of the type, but
/// can't uniquely represent the type or perform value witness operations on
/// it.
llvm::Value *emitTypeLayoutRef(IRGenFunction &IGF, SILType type,
                               MetadataDependencyCollector *collector);

/// Given type metadata that we don't know the dynamic state of,
/// fetch its dynamic state under the rules of the given request.
MetadataResponse emitGetTypeMetadataDynamicState(IRGenFunction &IGF,
                                                 DynamicMetadataRequest request,
                                                 llvm::Value *metadata);

/// Given a metadata response, ensure that it satisfies the requirements
/// of the given request.
MetadataResponse emitCheckTypeMetadataState(IRGenFunction &IGF,
                                            DynamicMetadataRequest request,
                                            MetadataResponse response);

/// Return the abstract operational cost of a checkTypeMetadataState operation.
OperationCost getCheckTypeMetadataStateCost(DynamicMetadataRequest request,
                                            MetadataResponse response);

ParameterFlags getABIParameterFlags(ParameterTypeFlags flags);

} // end namespace irgen
} // end namespace swift

#endif

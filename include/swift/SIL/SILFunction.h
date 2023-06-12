//===--- SILFunction.h - Defines the SILFunction class ----------*- C++ -*-===//
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
// This file defines the SILFunction class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILFUNCTION_H
#define SWIFT_SIL_SILFUNCTION_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Module.h"
#include "swift/AST/ResilienceExpansion.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/Basic/SwiftObjectHeader.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILPrintContext.h"

namespace swift {

class ASTContext;
class SILInstruction;
class SILModule;
class SILFunctionBuilder;
class SILProfiler;
class BasicBlockBitfield;
class NodeBitfield;
class SILPassManager;

namespace Lowering {
class TypeLowering;
class AbstractionPattern;
}

enum IsBare_t { IsNotBare, IsBare };
enum IsTransparent_t { IsNotTransparent, IsTransparent };
enum Inline_t { InlineDefault, NoInline, AlwaysInline };
enum IsThunk_t {
  IsNotThunk,
  IsThunk,
  IsReabstractionThunk,
  IsSignatureOptimizedThunk
};
enum IsDynamicallyReplaceable_t {
  IsNotDynamic,
  IsDynamic
};
enum IsExactSelfClass_t {
  IsNotExactSelfClass,
  IsExactSelfClass,
};
enum IsDistributed_t {
  IsNotDistributed,
  IsDistributed,
};
enum IsRuntimeAccessible_t {
  IsNotRuntimeAccessible,
  IsRuntimeAccessible
};

enum ForceEnableLexicalLifetimes_t {
  DoNotForceEnableLexicalLifetimes,
  DoForceEnableLexicalLifetimes
};

enum UseStackForPackMetadata_t {
  DoNotUseStackForPackMetadata,
  DoUseStackForPackMetadata,
};

enum class PerformanceConstraints : uint8_t {
  None = 0,
  NoAllocation = 1,
  NoLocks = 2,
};

class SILSpecializeAttr final {
  friend SILFunction;
public:
  enum class SpecializationKind {
    Full,
    Partial
  };

  static SILSpecializeAttr *create(SILModule &M,
                                   GenericSignature specializedSignature,
                                   ArrayRef<Type> typeErasedParams,
                                   bool exported, SpecializationKind kind,
                                   SILFunction *target, Identifier spiGroup,
                                   const ModuleDecl *spiModule,
                                   AvailabilityContext availability);

  bool isExported() const {
    return exported;
  }

  bool isFullSpecialization() const {
    return kind == SpecializationKind::Full;
  }

  bool isPartialSpecialization() const {
    return kind == SpecializationKind::Partial;
  }

  SpecializationKind getSpecializationKind() const {
    return kind;
  }

  GenericSignature getSpecializedSignature() const {
    return specializedSignature;
  }

  GenericSignature getUnerasedSpecializedSignature() const {
    return unerasedSpecializedSignature;
  }

  ArrayRef<Type> getTypeErasedParams() const {
    return typeErasedParams;
  }

  SILFunction *getFunction() const {
    return F;
  }

  SILFunction *getTargetFunction() const {
    return targetFunction;
  }

  Identifier getSPIGroup() const {
    return spiGroup;
  }

  const ModuleDecl *getSPIModule() const {
    return spiModule;
  }

  AvailabilityContext getAvailability() const {
    return availability;
  }

  void print(llvm::raw_ostream &OS) const;

private:
  SpecializationKind kind;
  bool exported;
  GenericSignature specializedSignature;
  GenericSignature unerasedSpecializedSignature;
  llvm::SmallVector<Type, 2> typeErasedParams;
  Identifier spiGroup;
  AvailabilityContext availability;
  const ModuleDecl *spiModule = nullptr;
  SILFunction *F = nullptr;
  SILFunction *targetFunction = nullptr;

  SILSpecializeAttr(bool exported, SpecializationKind kind,
                    GenericSignature specializedSignature,
                    GenericSignature unerasedSpecializedSignature,
                    ArrayRef<Type> typeErasedParams,
                    SILFunction *target, Identifier spiGroup,
                    const ModuleDecl *spiModule,
                    AvailabilityContext availability);
};

/// SILFunction - A function body that has been lowered to SIL. This consists of
/// zero or more SIL SILBasicBlock objects that contain the SILInstruction
/// objects making up the function.
class SILFunction
  : public llvm::ilist_node<SILFunction>, public SILAllocated<SILFunction>,
    public SwiftObjectHeader {
    
private:
  void *libswiftSpecificData[4];

public:
  using BlockListType = llvm::iplist<SILBasicBlock>;

  // For more information see docs/SIL.rst
  enum class Purpose : uint8_t {
    None,
    GlobalInit,
    GlobalInitOnceFunction,
    LazyPropertyGetter
  };

private:
  friend class SILBasicBlock;
  friend class SILModule;
  friend class SILFunctionBuilder;
  template <typename, unsigned> friend class BasicBlockData;
  template <class, class> friend class SILBitfield;
  friend class BasicBlockBitfield;
  friend class NodeBitfield;

  /// Module - The SIL module that the function belongs to.
  SILModule &Module;

  /// The mangled name of the SIL function, which will be propagated
  /// to the binary.  A pointer into the module's lookup table.
  StringRef Name;

  /// A single-linked list of snapshots of the function.
  ///
  /// Snapshots are copies of the current function at a given point in time.
  SILFunction *snapshots = nullptr;
  
  /// The snapshot ID of this function.
  ///
  /// 0 means, it's not a snapshot, but the original function.
  int snapshotID = 0;

  /// The lowered type of the function.
  CanSILFunctionType LoweredType;

  /// The context archetypes of the function.
  GenericEnvironment *GenericEnv = nullptr;

  /// The information about specialization.
  /// Only set if this function is a specialization of another function.
  const GenericSpecializationInformation *SpecializationInfo = nullptr;

  /// The forwarding substitution map, lazily computed.
  SubstitutionMap ForwardingSubMap;

  /// The collection of all BasicBlocks in the SILFunction. Empty for external
  /// function references.
  BlockListType BlockList;

  /// The owning declaration of this function's clang node, if applicable.
  ValueDecl *ClangNodeOwner = nullptr;

  /// The source location and scope of the function.
  const SILDebugScope *DebugScope = nullptr;

  /// The AST decl context of the function.
  DeclContext *DeclCtxt = nullptr;

  /// The module that defines this function. This member should only be set as
  /// a fallback when a \c DeclCtxt is unavailable.
  ModuleDecl *ParentModule = nullptr;

  /// The profiler for instrumentation based profiling, or null if profiling is
  /// disabled.
  SILProfiler *Profiler = nullptr;

  /// The function this function is meant to replace. Null if this is not a
  /// @_dynamicReplacement(for:) function.
  SILFunction *ReplacedFunction = nullptr;

  /// This SILFunction REFerences an ad-hoc protocol requirement witness in
  /// order to keep it alive, such that it main be obtained in IRGen. Without
  /// this explicit reference, the witness would seem not-used, and not be
  /// accessible for IRGen.
  ///
  /// Specifically, one such case is the DistributedTargetInvocationDecoder's
  /// 'decodeNextArgument' which must be retained, as it is only used from IRGen
  /// and such, appears as-if unused in SIL and would get optimized away.
  // TODO: Consider making this a general "references adhoc functions" and make it an array?
  SILFunction *RefAdHocRequirementFunction = nullptr;

  Identifier ObjCReplacementFor;

  /// The head of a single-linked list of currently alive BasicBlockBitfield.
  BasicBlockBitfield *newestAliveBlockBitfield = nullptr;

  /// The head of a single-linked list of currently alive NodeBitfield.
  NodeBitfield *newestAliveNodeBitfield = nullptr;

  /// A monotonically increasing ID which is incremented whenever a
  /// BasicBlockBitfield or NodeBitfield is constructed.
  /// For details see SILBitfield::bitfieldID;
  int64_t currentBitfieldID = 1;

  /// Unique identifier for vector indexing and deterministic sorting.
  /// May be reused when zombie functions are recovered.
  unsigned index;

  /// The function's set of semantics attributes.
  ///
  /// TODO: Why is this using a std::string? Why don't we use uniqued
  /// StringRefs?
  std::vector<std::string> SemanticsAttrSet;

  /// The function's remaining set of specialize attributes.
  std::vector<SILSpecializeAttr*> SpecializeAttrSet;

  /// Name of a section if @_section attribute was used, otherwise empty.
  StringRef Section;

  /// Has value if there's a profile for this function
  /// Contains Function Entry Count
  ProfileCounter EntryCount;

  /// The availability used to determine if declarations of this function
  /// should use weak linking.
  AvailabilityContext Availability;

  Purpose specialPurpose = Purpose::None;

  PerformanceConstraints perfConstraints = PerformanceConstraints::None;

  /// This is the number of uses of this SILFunction inside the SIL.
  /// It does not include references from debug scopes.
  unsigned RefCount = 0;

  /// Used to verify if a BasicBlockData is not valid anymore.
  /// This counter is incremented every time a BasicBlockData re-assigns new
  /// block indices.
  unsigned BlockListChangeIdx = 0;

  /// The function's bare attribute. Bare means that the function is SIL-only
  /// and does not require debug info.
  unsigned Bare : 1;

  /// The function's transparent attribute.
  unsigned Transparent : 1;

  /// The function's serialized attribute.
  bool Serialized : 1;

  /// Specifies if this function is a thunk or a reabstraction thunk.
  ///
  /// The inliner uses this information to avoid inlining (non-trivial)
  /// functions into the thunk.
  unsigned Thunk : 2;

  /// The scope in which the parent class can be subclassed, if this is a method
  /// which is contained in the vtable of that class.
  unsigned ClassSubclassScope : 2;

  /// The function's global_init attribute.
  unsigned GlobalInitFlag : 1;

  /// The function's noinline attribute.
  unsigned InlineStrategy : 2;

  /// The linkage of the function.
  unsigned Linkage : NumSILLinkageBits;

  /// Set if the function may be referenced from C code and should thus be
  /// preserved and exported more widely than its Swift linkage and usage
  /// would indicate.
  unsigned HasCReferences : 1;

  /// Whether attribute @_used was present
  unsigned MarkedAsUsed : 1;

  /// Whether cross-module references to this function should always use weak
  /// linking.
  unsigned IsAlwaysWeakImported : 1;

  /// Whether the implementation can be dynamically replaced.
  unsigned IsDynamicReplaceable : 1;

  /// If true, this indicates that a class method implementation will always be
  /// invoked with a `self` argument of the exact base class type.
  unsigned ExactSelfClass : 1;

  /// Check whether this is a distributed method.
  unsigned IsDistributed : 1;

  /// Check whether this function could be looked up at runtime via special API.
  unsigned IsRuntimeAccessible : 1;

  unsigned stackProtection : 1;

  /// True if this function is inlined at least once. This means that the
  /// debug info keeps a pointer to this function.
  unsigned Inlined : 1;

  /// True if this function is a zombie function. This means that the function
  /// is dead and not referenced from anywhere inside the SIL. But it is kept
  /// for other purposes:
  /// *) It is inlined and the debug info keeps a reference to the function.
  /// *) It is a dead method of a class which has higher visibility than the
  ///    method itself. In this case we need to create a vtable stub for it.
  /// *) It is a function referenced by the specialization information.
  unsigned Zombie : 1;

  /// True if this function is in Ownership SSA form and thus must pass
  /// ownership verification.
  ///
  /// This enables the verifier to easily prove that before the Ownership Model
  /// Eliminator runs on a function, we only see a non-semantic-arc world and
  /// after the pass runs, we only see a semantic-arc world.
  unsigned HasOwnership : 1;

  /// Set if the function body was deserialized from canonical SIL. This implies
  /// that the function's home module performed SIL diagnostics prior to
  /// serialization.
  unsigned WasDeserializedCanonical : 1;

  /// True if this is a reabstraction thunk of escaping function type whose
  /// single argument is a potentially non-escaping closure. This is an escape
  /// hatch to allow non-escaping functions to be stored or passed as an
  /// argument with escaping function type. The thunk argument's function type
  /// is not necessarily @noescape. The only relevant aspect of the argument is
  /// that it may have unboxed capture (i.e. @inout_aliasable parameters).
  unsigned IsWithoutActuallyEscapingThunk : 1;

  /// If != OptimizationMode::NotSet, the optimization mode specified with an
  /// function attribute.
  unsigned OptMode : NumOptimizationModeBits;

  /// The function's effects attribute.
  unsigned EffectsKindAttr : NumEffectsKindBits;

  /// The function is in a statically linked module.
  unsigned IsStaticallyLinked : 1;

  /// If true, the function has lexical lifetimes even if the module does not.
  unsigned ForceEnableLexicalLifetimes : 1;

  /// If true, the function contains an instruction that prevents stack nesting
  /// from running with pack metadata markers in place.
  unsigned UseStackForPackMetadata : 1;

  static void
  validateSubclassScope(SubclassScope scope, IsThunk_t isThunk,
                        const GenericSpecializationInformation *genericInfo) {
#ifndef NDEBUG
    // The _original_ function for a method can turn into a thunk through
    // signature optimization, meaning it needs to retain its subclassScope, but
    // other thunks and specializations are implementation details and so
    // shouldn't be connected to their parent class.
    bool thunkCanHaveSubclassScope;
    switch (isThunk) {
    case IsNotThunk:
    case IsSignatureOptimizedThunk:
      thunkCanHaveSubclassScope = true;
      break;
    case IsThunk:
    case IsReabstractionThunk:
      thunkCanHaveSubclassScope = false;
      break;
    }
    auto allowsInterestingScopes = thunkCanHaveSubclassScope && !genericInfo;
    assert(
        allowsInterestingScopes ||
        scope == SubclassScope::NotApplicable &&
            "SubclassScope on specialization or non-signature-optimized thunk");
#endif
  }

  SILFunction(SILModule &module, SILLinkage linkage, StringRef mangledName,
              CanSILFunctionType loweredType, GenericEnvironment *genericEnv,
              IsBare_t isBareSILFunction, IsTransparent_t isTrans,
              IsSerialized_t isSerialized, ProfileCounter entryCount,
              IsThunk_t isThunk, SubclassScope classSubclassScope,
              Inline_t inlineStrategy, EffectsKind E,
              const SILDebugScope *debugScope,
              IsDynamicallyReplaceable_t isDynamic,
              IsExactSelfClass_t isExactSelfClass,
              IsDistributed_t isDistributed,
              IsRuntimeAccessible_t isRuntimeAccessible);

  static SILFunction *
  create(SILModule &M, SILLinkage linkage, StringRef name,
         CanSILFunctionType loweredType, GenericEnvironment *genericEnv,
         Optional<SILLocation> loc, IsBare_t isBareSILFunction,
         IsTransparent_t isTrans, IsSerialized_t isSerialized,
         ProfileCounter entryCount, IsDynamicallyReplaceable_t isDynamic,
         IsDistributed_t isDistributed,
         IsRuntimeAccessible_t isRuntimeAccessible,
         IsExactSelfClass_t isExactSelfClass,
         IsThunk_t isThunk = IsNotThunk,
         SubclassScope classSubclassScope = SubclassScope::NotApplicable,
         Inline_t inlineStrategy = InlineDefault,
         EffectsKind EffectsKindAttr = EffectsKind::Unspecified,
         SILFunction *InsertBefore = nullptr,
         const SILDebugScope *DebugScope = nullptr);

  void init(SILLinkage Linkage, StringRef Name, CanSILFunctionType LoweredType,
            GenericEnvironment *genericEnv, IsBare_t isBareSILFunction,
            IsTransparent_t isTrans, IsSerialized_t isSerialized,
            ProfileCounter entryCount, IsThunk_t isThunk,
            SubclassScope classSubclassScope, Inline_t inlineStrategy,
            EffectsKind E, const SILDebugScope *DebugScope,
            IsDynamicallyReplaceable_t isDynamic,
            IsExactSelfClass_t isExactSelfClass, IsDistributed_t isDistributed,
            IsRuntimeAccessible_t isRuntimeAccessible);

  /// Set has ownership to the given value. True means that the function has
  /// ownership, false means it does not.
  ///
  /// Only for use by FunctionBuilders!
  void setHasOwnership(bool newValue) { HasOwnership = newValue; }

  void setName(StringRef name) {
    // All the snapshots share the same name.
    SILFunction *sn = this;
    do {
      sn->Name = name;
    } while ((sn = sn->snapshots) != nullptr);
  }

public:
  ~SILFunction();

  SILModule &getModule() const { return Module; }

  /// Creates a snapshot with a given `ID` from the current function.
  void createSnapshot(int ID);
  
  /// Returns the snapshot with the given `ID` or null if no such snapshot exists.
  SILFunction *getSnapshot(int ID);
  
  /// Restores the current function from a given snapshot.
  void restoreFromSnapshot(int ID);
  
  /// Deletes a snapshot with the `ID`.
  void deleteSnapshot(int ID);

  SILType getLoweredType() const {
    return SILType::getPrimitiveObjectType(LoweredType);
  }
  CanSILFunctionType getLoweredFunctionType() const {
    return LoweredType;
  }
  CanSILFunctionType
  getLoweredFunctionTypeInContext(TypeExpansionContext context) const;

  SILType getLoweredTypeInContext(TypeExpansionContext context) const {
    return SILType::getPrimitiveObjectType(
        getLoweredFunctionTypeInContext(context));
  }

  SILFunctionConventions getConventions() const {
    return SILFunctionConventions(LoweredType, getModule());
  }

  SILFunctionConventions getConventionsInContext() const {
    auto fnType = getLoweredFunctionTypeInContext(getTypeExpansionContext());
    return SILFunctionConventions(fnType, getModule());
  }

  unsigned getIndex() const { return index; }

  SILProfiler *getProfiler() const { return Profiler; }

  SILFunction *getDynamicallyReplacedFunction() const {
    return ReplacedFunction;
  }

  void setDynamicallyReplacedFunction(SILFunction *f) {
    assert(ReplacedFunction == nullptr && "already set");
    assert(!hasObjCReplacement());

    if (f == nullptr)
      return;
    ReplacedFunction = f;
    ReplacedFunction->incrementRefCount();
  }
  /// This function should only be called when SILFunctions are bulk deleted.
  void dropDynamicallyReplacedFunction() {
    if (!ReplacedFunction)
      return;
    ReplacedFunction->decrementRefCount();
    ReplacedFunction = nullptr;
  }

  SILFunction *getReferencedAdHocRequirementWitnessFunction() const {
    return RefAdHocRequirementFunction;
  }
  // Marks that this `SILFunction` uses the passed in ad-hoc protocol
  // requirement witness `f` and therefore must retain it explicitly,
  // otherwise we might not be able to get a reference to it.
  void setReferencedAdHocRequirementWitnessFunction(SILFunction *f) {
    assert(RefAdHocRequirementFunction == nullptr && "already set");

    if (f == nullptr)
      return;
    RefAdHocRequirementFunction = f;
    RefAdHocRequirementFunction->incrementRefCount();
  }
  void dropReferencedAdHocRequirementWitnessFunction() {
    if (!RefAdHocRequirementFunction)
      return;
    RefAdHocRequirementFunction->decrementRefCount();
    RefAdHocRequirementFunction = nullptr;
  }

  bool hasObjCReplacement() const {
    return !ObjCReplacementFor.empty();
  }

  Identifier getObjCReplacement() const {
    return ObjCReplacementFor;
  }

  void setObjCReplacement(AbstractFunctionDecl *replacedDecl);
  void setObjCReplacement(Identifier replacedDecl);

  void setProfiler(SILProfiler *InheritedProfiler) {
    assert(!Profiler && "Function already has a profiler");
    Profiler = InheritedProfiler;
  }

  void createProfiler(SILDeclRef Ref);

  ProfileCounter getEntryCount() const { return EntryCount; }

  void setEntryCount(ProfileCounter Count) { EntryCount = Count; }

  bool isNoReturnFunction(TypeExpansionContext context) const;

  /// Unsafely rewrite the lowered type of this function.
  ///
  /// This routine does not touch the entry block arguments
  /// or return instructions; you need to do that yourself
  /// if you care.
  ///
  /// This routine does not update all the references in the module
  /// You have to do that yourself
  void rewriteLoweredTypeUnsafe(CanSILFunctionType newType) {
    LoweredType = newType;
  }

  /// Return the number of entities referring to this function (other
  /// than the SILModule).
  unsigned getRefCount() const { return RefCount; }

  /// Increment the reference count.
  void incrementRefCount() {
    RefCount++;
    assert(RefCount != 0 && "Overflow of reference count!");
  }

  /// Decrement the reference count.
  void decrementRefCount() {
    assert(RefCount != 0 && "Expected non-zero reference count on decrement!");
    RefCount--;
  }

  /// Drops all uses belonging to instructions in this function. The only valid
  /// operation performable on this object after this is called is called the
  /// destructor or deallocation.
  void dropAllReferences() {
    for (SILBasicBlock &BB : *this)
      BB.dropAllReferences();
  }

  /// Notify that this function was inlined. This implies that it is still
  /// needed for debug info generation, even if it is removed afterwards.
  void setInlined() {
    assert(!isZombie() && "Can't inline a zombie function");
    Inlined = true;
  }

  /// Returns true if this function was inlined.
  bool isInlined() const { return Inlined; }

  /// Mark this function as removed from the module's function list, but kept
  /// as "zombie" for debug info or vtable stub generation.
  void setZombie() {
    assert(!isZombie() && "Function is a zombie function already");
    Zombie = true;
  }

  /// Returns true if this function is dead, but kept in the module's zombie list.
  bool isZombie() const { return Zombie; }

  /// Returns true if this function has qualified ownership instructions in it.
  bool hasOwnership() const { return HasOwnership; }

  /// Sets the HasOwnership flag to false. This signals to SIL that no
  /// ownership instructions should be in this function any more.
  void setOwnershipEliminated() { setHasOwnership(false); }

  /// Returns true if this function was deserialized from canonical
  /// SIL. (.swiftmodule files contain canonical SIL; .sib files may be 'raw'
  /// SIL). If so, diagnostics should not be reapplied.
  bool wasDeserializedCanonical() const { return WasDeserializedCanonical; }

  void setWasDeserializedCanonical(bool val = true) {
    WasDeserializedCanonical = val;
  }

  bool isStaticallyLinked() const { return IsStaticallyLinked; }

  void setIsStaticallyLinked(bool value) {
    IsStaticallyLinked = value;
  }

  ForceEnableLexicalLifetimes_t forceEnableLexicalLifetimes() const {
    return ForceEnableLexicalLifetimes_t(ForceEnableLexicalLifetimes);
  }

  void setForceEnableLexicalLifetimes(ForceEnableLexicalLifetimes_t value) {
    ForceEnableLexicalLifetimes = value;
  }

  UseStackForPackMetadata_t useStackForPackMetadata() const {
    return UseStackForPackMetadata_t(UseStackForPackMetadata);
  }

  void setUseStackForPackMetadata(UseStackForPackMetadata_t value) {
    UseStackForPackMetadata = value;
  }

  /// Returns true if this is a reabstraction thunk of escaping function type
  /// whose single argument is a potentially non-escaping closure. i.e. the
  /// thunks' function argument may itself have @inout_aliasable parameters.
  bool isWithoutActuallyEscapingThunk() const {
    return IsWithoutActuallyEscapingThunk;
  }

  void setWithoutActuallyEscapingThunk(bool val = true) {
    assert(!val || isThunk() == IsReabstractionThunk);
    IsWithoutActuallyEscapingThunk = val;
  }

  bool isAsync() const { return LoweredType->isAsync(); }

  /// Returns the calling convention used by this entry point.
  SILFunctionTypeRepresentation getRepresentation() const {
    return getLoweredFunctionType()->getRepresentation();
  }

  ResilienceExpansion getResilienceExpansion() const {
    return (isSerialized()
            ? ResilienceExpansion::Minimal
            : ResilienceExpansion::Maximal);
  }

  // Returns the type expansion context to be used inside this function.
  TypeExpansionContext getTypeExpansionContext() const {
    return TypeExpansionContext(*this);
  }

  const Lowering::TypeLowering &
  getTypeLowering(Lowering::AbstractionPattern orig, Type subst);

  const Lowering::TypeLowering &getTypeLowering(Type t) const;

  SILType getLoweredType(Lowering::AbstractionPattern orig, Type subst) const;

  SILType getLoweredType(Type t) const;

  CanType getLoweredRValueType(Lowering::AbstractionPattern orig, Type subst) const;

  CanType getLoweredRValueType(Type t) const;

  SILType getLoweredLoadableType(Type t) const;

  SILType getLoweredType(SILType t) const;

  const Lowering::TypeLowering &getTypeLowering(SILType type) const;

  bool isTypeABIAccessible(SILType type) const;

  /// Returns true if this function has a calling convention that has a self
  /// argument.
  bool hasSelfParam() const {
    return getLoweredFunctionType()->hasSelfParam();
  }

  /// Returns true if the function has parameters that are consumed by the
  // callee.
  bool hasOwnedParameters() const {
    for (auto &ParamInfo : getLoweredFunctionType()->getParameters()) {
      if (ParamInfo.isConsumed())
        return true;
    }
    return false;
  }

  // Returns true if the function has indirect out parameters.
  bool hasIndirectFormalResults() const {
    return getLoweredFunctionType()->hasIndirectFormalResults();
  }

  /// Returns true if this function ie either a class method, or a
  /// closure that captures the 'self' value or its metatype.
  ///
  /// If this returns true, DynamicSelfType can be used in the body
  /// of the function.
  ///
  /// Note that this is not the same as hasSelfParam().
  ///
  /// For closures that capture DynamicSelfType, hasDynamicSelfMetadata()
  /// is true and hasSelfParam() is false. For methods on value types,
  /// hasSelfParam() is true and hasDynamicSelfMetadata() is false.
  bool hasDynamicSelfMetadata() const;

  /// Return the mangled name of this SILFunction.
  StringRef getName() const { return Name; }

  /// A convenience function which checks if the function has a specific
  /// \p name. It is equivalent to getName() == Name, but as it is not
  /// inlined it can be called from the debugger.
  bool hasName(const char *Name) const;

  /// True if this is a declaration of a function defined in another module.
  bool isExternalDeclaration() const { return BlockList.empty(); }

  /// Returns true if this is a definition of a function defined in this module.
  bool isDefinition() const { return !isExternalDeclaration(); }

  /// Returns true if there exist pre-specializations.
  bool hasPrespecialization() const;

  /// Get this function's linkage attribute.
  SILLinkage getLinkage() const { return SILLinkage(Linkage); }

  /// Set the function's linkage attribute.
  void setLinkage(SILLinkage linkage) { Linkage = unsigned(linkage); }

  /// Returns true if this function can be inlined into a fragile function
  /// body.
  bool hasValidLinkageForFragileInline() const { return isSerialized(); }

  /// Returns true if this function can be referenced from a fragile function
  /// body.
  bool hasValidLinkageForFragileRef() const;

  /// Get's the effective linkage which is used to derive the llvm linkage.
  /// Usually this is the same as getLinkage(), except in one case: if this
  /// function is a method in a class which has higher visibility than the
  /// method itself, the function can be referenced from vtables of derived
  /// classes in other compilation units.
  SILLinkage getEffectiveSymbolLinkage() const {
    return effectiveLinkageForClassMember(getLinkage(),
                                          getClassSubclassScope());
  }
    
  /// Helper method which returns true if this function has "external" linkage.
  bool isAvailableExternally() const {
    return swift::isAvailableExternally(getLinkage());
  }

  /// Helper method which returns true if the linkage of the SILFunction
  /// indicates that the object's definition might be required outside the
  /// current SILModule.
  bool isPossiblyUsedExternally() const;

  /// In addition to isPossiblyUsedExternally() it returns also true if this
  /// is a (private or internal) vtable method which can be referenced by
  /// vtables of derived classes outside the compilation unit.
  bool isExternallyUsedSymbol() const;

  /// Return whether this function may be referenced by C code.
  bool hasCReferences() const { return HasCReferences; }
  void setHasCReferences(bool value) { HasCReferences = value; }

  /// Returns the availability context used to determine if the function's
  /// symbol should be weakly referenced across module boundaries.
  AvailabilityContext getAvailabilityForLinkage() const {
    return Availability;
  }

  void setAvailabilityForLinkage(AvailabilityContext availability) {
    Availability = availability;
  }

  /// Returns whether this function's symbol must always be weakly referenced
  /// across module boundaries.
  bool isAlwaysWeakImported() const { return IsAlwaysWeakImported; }

  void setIsAlwaysWeakImported(bool value) { IsAlwaysWeakImported = value; }

  bool isWeakImported(ModuleDecl *module) const;

  /// Returns whether this function implementation can be dynamically replaced.
  IsDynamicallyReplaceable_t isDynamicallyReplaceable() const {
    return IsDynamicallyReplaceable_t(IsDynamicReplaceable);
  }
  void setIsDynamic(IsDynamicallyReplaceable_t value = IsDynamic) {
    IsDynamicReplaceable = value;
    assert(!Transparent || !IsDynamicReplaceable);
  }

  IsExactSelfClass_t isExactSelfClass() const {
    return IsExactSelfClass_t(ExactSelfClass);
  }
  void setIsExactSelfClass(IsExactSelfClass_t t) {
    ExactSelfClass = t;
  }

  IsDistributed_t isDistributed() const {
    return IsDistributed_t(IsDistributed);
  }
  void
  setIsDistributed(IsDistributed_t value = IsDistributed_t::IsDistributed) {
    IsDistributed = value;
  }

  IsRuntimeAccessible_t isRuntimeAccessible() const {
    return IsRuntimeAccessible_t(IsRuntimeAccessible);
  }
  void setIsRuntimeAccessible(IsRuntimeAccessible_t value =
                                  IsRuntimeAccessible_t::IsRuntimeAccessible) {
    IsRuntimeAccessible = value;
  }

  bool needsStackProtection() const { return stackProtection; }
  void setNeedStackProtection(bool needSP) { stackProtection = needSP; }

  /// Get the DeclContext of this function.
  DeclContext *getDeclContext() const { return DeclCtxt; }

  /// \returns True if the function is marked with the @_semantics attribute
  /// and has special semantics that the optimizer can use to optimize the
  /// function.
  bool hasSemanticsAttrs() const { return !SemanticsAttrSet.empty(); }

  /// \returns True if the function has a semantic attribute that starts with a
  /// specific string.
  ///
  /// TODO: This needs a better name.
  bool hasSemanticsAttrThatStartsWith(StringRef S) {
    return count_if(getSemanticsAttrs(), [&S](const std::string &Attr) -> bool {
      return StringRef(Attr).startswith(S);
    });
  }

  /// \returns the semantics tag that describes this function.
  ArrayRef<std::string> getSemanticsAttrs() const { return SemanticsAttrSet; }

  /// \returns True if the function has the semantics flag \p Value;
  bool hasSemanticsAttr(StringRef Value) const {
    return count(SemanticsAttrSet, Value);
  }

  /// Add the given semantics attribute to the attr list set.
  void addSemanticsAttr(StringRef Ref) {
    if (hasSemanticsAttr(Ref))
      return;
    SemanticsAttrSet.push_back(Ref.str());
    std::sort(SemanticsAttrSet.begin(), SemanticsAttrSet.end());
  }

  /// Remove the semantics
  void removeSemanticsAttr(StringRef Ref) {
    auto Iter =
        std::remove(SemanticsAttrSet.begin(), SemanticsAttrSet.end(), Ref);
    SemanticsAttrSet.erase(Iter);
  }

  /// \returns the range of specialize attributes.
  ArrayRef<SILSpecializeAttr*> getSpecializeAttrs() const {
    return SpecializeAttrSet;
  }

  /// Removes all specialize attributes from this function.
  void clearSpecializeAttrs() {
    forEachSpecializeAttrTargetFunction(
        [](SILFunction *targetFun) { targetFun->decrementRefCount(); });
    SpecializeAttrSet.clear();
  }

  void addSpecializeAttr(SILSpecializeAttr *Attr);

  void removeSpecializeAttr(SILSpecializeAttr *attr);

  void forEachSpecializeAttrTargetFunction(
      llvm::function_ref<void(SILFunction *)> action);

  /// Get this function's optimization mode or OptimizationMode::NotSet if it is
  /// not set for this specific function.
  OptimizationMode getOptimizationMode() const {
    return OptimizationMode(OptMode);
  }

  /// Returns the optimization mode for the function. If no mode is set for the
  /// function, returns the global mode, i.e. the mode of the module's options.
  OptimizationMode getEffectiveOptimizationMode() const;

  void setOptimizationMode(OptimizationMode mode) {
    OptMode = unsigned(mode);
  }

  /// True if debug information must be preserved (-Onone).
  ///
  /// If this is false (-O), then the presence of debug info must not affect the
  /// outcome of any transformations.
  ///
  /// Typically used to determine whether a debug_value is a normal SSA use or
  /// incidental use.
  bool preserveDebugInfo() const;

  PerformanceConstraints getPerfConstraints() const { return perfConstraints; }

  void setPerfConstraints(PerformanceConstraints perfConstr) {
    perfConstraints = perfConstr;
  }

  /// \returns True if the function is optimizable (i.e. not marked as no-opt),
  ///          or is raw SIL (so that the mandatory passes still run).
  bool shouldOptimize() const;

  /// Returns true if this function should be optimized for size.
  bool optimizeForSize() const {
    return getEffectiveOptimizationMode() == OptimizationMode::ForSize;
  }

  /// Returns true if this is a function that should have its ownership
  /// verified.
  bool shouldVerifyOwnership() const;

  /// Check if the function has a location.
  /// FIXME: All functions should have locations, so this method should not be
  /// necessary.
  bool hasLocation() const {
    return DebugScope && !DebugScope->Loc.isNull();
  }

  /// Get the source location of the function.
  SILLocation getLocation() const {
    assert(DebugScope && "no scope/location");
    return getDebugScope()->Loc;
  }

  /// Initialize the debug scope of the function and also set the DeclCtxt.
  void setDebugScope(const SILDebugScope *DS) {
    DebugScope = DS;
    DeclCtxt = (DS ? DebugScope->Loc.getAsDeclContext() : nullptr);
  }

  /// Returns the module that defines this function.
  ModuleDecl *getParentModule() const {
    return DeclCtxt ? DeclCtxt->getParentModule() : ParentModule;
  }

  /// Sets \c ParentModule as fallback if \c DeclCtxt is not available to
  /// provide the parent module.
  void setParentModule(ModuleDecl *module) {
    assert(!DeclCtxt && "already have a DeclCtxt");
    ParentModule = module;
  }

  /// Initialize the debug scope for debug info on SIL level
  /// (-sil-based-debuginfo).
  void setSILDebugScope(const SILDebugScope *DS) {
    DebugScope = DS;
  }

  /// Get the source location of the function.
  const SILDebugScope *getDebugScope() const { return DebugScope; }

  /// Get this function's bare attribute.
  IsBare_t isBare() const { return IsBare_t(Bare); }
  void setBare(IsBare_t isB) { Bare = isB; }

  /// Get this function's transparent attribute.
  IsTransparent_t isTransparent() const { return IsTransparent_t(Transparent); }
  void setTransparent(IsTransparent_t isT) {
    Transparent = isT;
    assert(!Transparent || !IsDynamicReplaceable);
  }

  /// Get this function's serialized attribute.
  IsSerialized_t isSerialized() const { return IsSerialized_t(Serialized); }
  void setSerialized(IsSerialized_t isSerialized) {
    Serialized = isSerialized;
    assert(this->isSerialized() == isSerialized &&
           "too few bits for Serialized storage");
  }

  /// Get this function's thunk attribute.
  IsThunk_t isThunk() const { return IsThunk_t(Thunk); }
  void setThunk(IsThunk_t isThunk) {
    validateSubclassScope(getClassSubclassScope(), isThunk, SpecializationInfo);
    Thunk = isThunk;
  }

  /// Get the class visibility (relevant for class methods).
  SubclassScope getClassSubclassScope() const {
    return SubclassScope(ClassSubclassScope);
  }
  void setClassSubclassScope(SubclassScope scope) {
    validateSubclassScope(scope, isThunk(), SpecializationInfo);
    ClassSubclassScope = static_cast<unsigned>(scope);
  }

  /// Get this function's noinline attribute.
  Inline_t getInlineStrategy() const { return Inline_t(InlineStrategy); }
  void setInlineStrategy(Inline_t inStr) { InlineStrategy = inStr; }

  /// \return the function side effects information.
  EffectsKind getEffectsKind() const { return EffectsKind(EffectsKindAttr); }

  /// \return True if the function is annotated with the @_effects attribute.
  bool hasEffectsKind() const {
    return EffectsKind(EffectsKindAttr) != EffectsKind::Unspecified;
  }

  /// Set the function side effect information.
  void setEffectsKind(EffectsKind E) {
    EffectsKindAttr = unsigned(E);
  }
  
  std::pair<const char *, int>  parseArgumentEffectsFromSource(StringRef effectStr,
                                                              ArrayRef<StringRef> paramNames);
  std::pair<const char *, int>  parseArgumentEffectsFromSIL(StringRef effectStr,
                                                           int argumentIndex);
  std::pair<const char *, int>  parseGlobalEffectsFromSIL(StringRef effectStr);
  std::pair<const char *, int>  parseMultipleEffectsFromSIL(StringRef effectStr);
  void writeEffect(llvm::raw_ostream &OS, int effectIdx) const;
  void writeEffects(llvm::raw_ostream &OS) const {
    writeEffect(OS, -1);
  }
  void copyEffects(SILFunction *from);
  bool hasArgumentEffects() const;
  void visitArgEffects(std::function<void(int, int, bool)> c) const;
  MemoryBehavior getMemoryBehavior(bool observeRetains);

  Purpose getSpecialPurpose() const { return specialPurpose; }

  /// Get this function's global_init attribute.
  ///
  /// The implied semantics are:
  /// - side-effects can occur any time before the first invocation.
  /// - all calls to the same global_init function have the same side-effects.
  /// - any operation that may observe the initializer's side-effects must be
  ///   preceded by a call to the initializer.
  ///
  /// This is currently true if the function is an addressor that was lazily
  /// generated from a global variable access. Note that the initialization
  /// function itself does not need this attribute. It is private and only
  /// called within the addressor.
  bool isGlobalInit() const { return specialPurpose == Purpose::GlobalInit; }
    
  bool isGlobalInitOnceFunction() const {
    return specialPurpose == Purpose::GlobalInitOnceFunction;
  }

  bool isLazyPropertyGetter() const {
    return specialPurpose == Purpose::LazyPropertyGetter;
  }

  void setSpecialPurpose(Purpose purpose) { specialPurpose = purpose; }

  /// Return whether this function has a foreign implementation which can
  /// be emitted on demand.
  bool hasForeignBody() const;

  /// Return whether this function corresponds to a Clang node.
  bool hasClangNode() const {
    return ClangNodeOwner != nullptr;
  }

  /// Set the owning declaration of the Clang node associated with this
  /// function.  We have to store an owner (a Swift declaration) instead of
  /// directly referencing the original declaration due to current
  /// limitations in the serializer.
  void setClangNodeOwner(ValueDecl *owner) {
    assert(owner->hasClangNode());
    ClangNodeOwner = owner;
  }

  /// Return the owning declaration of the Clang node associated with this
  /// function.  This should only be used for serialization.
  ValueDecl *getClangNodeOwner() const {
    return ClangNodeOwner;
  }

  /// Return the Clang node associated with this function if it has one.
  ClangNode getClangNode() const {
    return (ClangNodeOwner ? ClangNodeOwner->getClangNode() : ClangNode());
  }
  const clang::Decl *getClangDecl() const {
    return (ClangNodeOwner ? ClangNodeOwner->getClangDecl() : nullptr);
  }

  /// Returns whether this function is a specialization.
  bool isSpecialization() const { return SpecializationInfo != nullptr; }

  /// Return the specialization information.
  const GenericSpecializationInformation *getSpecializationInfo() const {
    assert(isSpecialization());
    return SpecializationInfo;
  }

  void setSpecializationInfo(const GenericSpecializationInformation *Info) {
    assert(!isSpecialization());
    validateSubclassScope(getClassSubclassScope(), isThunk(), Info);
    SpecializationInfo = Info;
  }
  
  /// If this function is a specialization, return the original function from
  /// which this function was specialized.
  const SILFunction *getOriginOfSpecialization() const;

  /// Retrieve the generic environment containing the mapping from interface
  /// types to context archetypes for this function. Only present if the
  /// function has a body.
  GenericEnvironment *getGenericEnvironment() const {
    return GenericEnv;
  }
  void setGenericEnvironment(GenericEnvironment *env) {
    GenericEnv = env;
  }

  /// Retrieve the generic signature from the generic environment of this
  /// function, if any. Else returns the null \c GenericSignature.
  GenericSignature getGenericSignature() const;

  /// Map the given type, which is based on an interface SILFunctionType and may
  /// therefore be dependent, to a type based on the context archetypes of this
  /// SILFunction.
  Type mapTypeIntoContext(Type type) const;

  /// Map the given type, which is based on an interface SILFunctionType and may
  /// therefore be dependent, to a type based on the context archetypes of this
  /// SILFunction.
  SILType mapTypeIntoContext(SILType type) const;

  /// Converts the given function definition to a declaration.
  void convertToDeclaration() {
    assert(isDefinition() && "Can only convert definitions to declarations");
    clear();
  }

  void clear();

  /// Like `clear`, but does not call `dropAllReferences`, which is the
  /// responsibility of the caller.
  void eraseAllBlocks();

  /// Return the identity substitutions necessary to forward this call if it is
  /// generic.
  SubstitutionMap getForwardingSubstitutionMap();

  /// Returns true if this SILFunction must be a defer statement.
  ///
  /// NOTE: This may return false for defer statements that have been
  /// deserialized without a DeclContext. This means that this is guaranteed to
  /// be correct for SILFunctions in Raw SIL that were not deserialized as
  /// canonical. Thus one can use it for diagnostics.
  bool isDefer() const {
    if (auto *dc = getDeclContext())
      if (auto *decl = dyn_cast_or_null<FuncDecl>(dc->getAsDecl()))
        return decl->isDeferBody();
    return false;
  }

  /// Returns true if this function belongs to a declaration that
  /// has `@_alwaysEmitIntoClient` attribute.
  bool markedAsAlwaysEmitIntoClient() const {
    if (!hasLocation())
      return false;

    auto *V = getLocation().getAsASTNode<ValueDecl>();
    return V && V->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>();
  }

  /// Return whether this function has attribute @_used on it
  bool markedAsUsed() const { return MarkedAsUsed; }
  void setMarkedAsUsed(bool value) { MarkedAsUsed = value; }

  /// Return custom section name if @_section was used, otherwise empty
  StringRef section() const { return Section; }
  void setSection(StringRef value) { Section = value; }

  /// Returns true if this function belongs to a declaration that returns
  /// an opaque result type with one or more availability conditions that are
  /// allowed to produce a different underlying type at runtime.
  bool hasOpaqueResultTypeWithAvailabilityConditions() const {
    if (!hasLocation())
      return false;

    if (auto *V = getLocation().getAsASTNode<ValueDecl>()) {
      auto *opaqueResult = V->getOpaqueResultTypeDecl();
      return opaqueResult &&
             opaqueResult->hasConditionallyAvailableSubstitutions();
    }

    return false;
  }

  //===--------------------------------------------------------------------===//
  // Block List Access
  //===--------------------------------------------------------------------===//

  using iterator = BlockListType::iterator;
  using reverse_iterator = BlockListType::reverse_iterator;
  using const_iterator = BlockListType::const_iterator;

  bool empty() const { return BlockList.empty(); }
  iterator begin() { return BlockList.begin(); }
  iterator end() { return BlockList.end(); }
  reverse_iterator rbegin() { return BlockList.rbegin(); }
  reverse_iterator rend() { return BlockList.rend(); }
  const_iterator begin() const { return BlockList.begin(); }
  const_iterator end() const { return BlockList.end(); }
  unsigned size() const { return BlockList.size(); }

  SILBasicBlock &front() { return *begin(); }
  const SILBasicBlock &front() const { return *begin(); }

  SILBasicBlock *getEntryBlock() { return &front(); }
  const SILBasicBlock *getEntryBlock() const { return &front(); }

  SILBasicBlock *createBasicBlock();
  SILBasicBlock *createBasicBlock(llvm::StringRef debugName);
  SILBasicBlock *createBasicBlockAfter(SILBasicBlock *afterBB);
  SILBasicBlock *createBasicBlockBefore(SILBasicBlock *beforeBB);

  /// Removes and destroys \p BB;
  void eraseBlock(SILBasicBlock *BB) {
    assert(BB->getParent() == this);
    BlockList.erase(BB);
  }

  /// Transfer all blocks of \p F into this function, at the begin of the block
  /// list.
  void moveAllBlocksFromOtherFunction(SILFunction *F);
  
  /// Transfer \p blockInOtherFunction of another function into this function,
  /// before \p insertPointInThisFunction.
  void moveBlockFromOtherFunction(SILBasicBlock *blockInOtherFunction,
                                  iterator insertPointInThisFunction);

  /// Move block \p BB to immediately before the iterator \p IP.
  ///
  /// The block must be part of this function.
  void moveBlockBefore(SILBasicBlock *BB, SILFunction::iterator IP);

  /// Move block \p BB to immediately after block \p After.
  ///
  /// The block must be part of this function.
  void moveBlockAfter(SILBasicBlock *BB, SILBasicBlock *After) {
    moveBlockBefore(BB, std::next(After->getIterator()));
  }

  /// Return the unique basic block containing a return inst if it
  /// exists. Otherwise, returns end.
  iterator findReturnBB() {
    return std::find_if(begin(), end(),
      [](const SILBasicBlock &BB) -> bool {
        const TermInst *TI = BB.getTerminator();
        return isa<ReturnInst>(TI);
    });
  }

  /// Return the unique basic block containing a return inst if it
  /// exists. Otherwise, returns end.
  const_iterator findReturnBB() const {
    return std::find_if(begin(), end(),
      [](const SILBasicBlock &BB) -> bool {
        const TermInst *TI = BB.getTerminator();
        return isa<ReturnInst>(TI);
    });
  }

  /// Return the unique basic block containing a throw inst if it
  /// exists. Otherwise, returns end.
  iterator findThrowBB() {
    return std::find_if(begin(), end(),
                        [](const SILBasicBlock &BB) -> bool {
                          const TermInst *TI = BB.getTerminator();
                          return isa<ThrowInst>(TI);
                        });
  }
  
  /// Return the unique basic block containing a throw inst if it
  /// exists. Otherwise, returns end.
  const_iterator findThrowBB() const {
    return std::find_if(begin(), end(),
                        [](const SILBasicBlock &BB) -> bool {
                          const TermInst *TI = BB.getTerminator();
                          return isa<ThrowInst>(TI);
                        });
  }

  /// Loop over all blocks in this function and add all function exiting blocks
  /// to output.
  void findExitingBlocks(llvm::SmallVectorImpl<SILBasicBlock *> &output) const {
    for (auto &Block : const_cast<SILFunction &>(*this)) {
      if (Block.getTerminator()->isFunctionExiting()) {
        output.emplace_back(&Block);
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Argument Helper Methods
  //===--------------------------------------------------------------------===//

  SILArgument *getArgument(unsigned i) {
    assert(!empty() && "Cannot get argument of a function without a body");
    return begin()->getArgument(i);
  }

  const SILArgument *getArgument(unsigned i) const {
    assert(!empty() && "Cannot get argument of a function without a body");
    return begin()->getArgument(i);
  }

  ArrayRef<SILArgument *> getArguments() const {
    assert(!empty() && "Cannot get arguments of a function without a body");
    return begin()->getArguments();
  }

  ArrayRef<SILArgument *> getIndirectResults() const {
    assert(!empty() && "Cannot get arguments of a function without a body");
    return begin()->getArguments().slice(
        0, getConventions().getNumIndirectSILResults());
  }

  ArrayRef<SILArgument *> getArgumentsWithoutIndirectResults() const {
    assert(!empty() && "Cannot get arguments of a function without a body");
    return begin()->getArguments().slice(
        getConventions().getNumIndirectSILResults());
  }

  const SILArgument *getSelfArgument() const {
    assert(hasSelfParam() && "This method can only be called if the "
                             "SILFunction has a self parameter");
    return getArguments().back();
  }

  const SILArgument *getDynamicSelfMetadata() const {
    assert(hasDynamicSelfMetadata() && "This method can only be called if the "
           "SILFunction has a self-metadata parameter");
    return getArguments().back();
  }

  //===--------------------------------------------------------------------===//
  // Miscellaneous
  //===--------------------------------------------------------------------===//

  /// A value's lifetime, determined by looking at annotations on its decl and
  /// the default lifetime for the type.
  Lifetime getLifetime(VarDecl *decl, SILType ty) {
    return ty.getLifetime(*this).getLifetimeForAnnotatedValue(
        decl->getLifetimeAnnotation());
  }

  /// verify - Run the SIL verifier to make sure that the SILFunction follows
  /// invariants.
  void verify(SILPassManager *passManager = nullptr,
              bool SingleFunction = true,
              bool isCompleteOSSA = true,
              bool checkLinearLifetime = true) const;

  /// Run the SIL verifier without assuming OSSA lifetimes end at dead end
  /// blocks.
  void verifyIncompleteOSSA() const {
    verify(/*passManager*/nullptr, /*SingleFunction=*/true, /*completeOSSALifetimes=*/false);
  }

  /// Verifies the lifetime of memory locations in the function.
  void verifyMemoryLifetime(SILPassManager *passManager);

  /// Run the SIL ownership verifier to check that all values with ownership
  /// have a linear lifetime. Regular OSSA invariants are checked separately in
  /// normal SIL verification.
  ///
  /// \p deadEndBlocks is nullptr when OSSA lifetimes are complete.
  ///
  /// NOTE: The ownership verifier is run when performing normal IR
  /// verification, so this verification can be viewed as a subset of
  /// SILFunction::verify(checkLinearLifetimes=true).
  void verifyOwnership(DeadEndBlocks *deadEndBlocks) const;

  /// Verify that all non-cond-br critical edges have been split.
  ///
  /// This is a fast subset of the checks performed in the SILVerifier.
  void verifyCriticalEdges() const;

  /// Pretty-print the SILFunction.
  void dump(bool Verbose) const;
  void dump() const;
  
  /// Pretty-print the SILFunction.
  /// Useful for dumping the function when running in a debugger.
  /// Warning: no error handling is done. Fails with an assert if the file
  /// cannot be opened.
  void dump(const char *FileName) const;

  /// Pretty-print the SILFunction to the tream \p OS.
  ///
  /// \param Verbose Dump SIL location information in verbose mode.
  void print(raw_ostream &OS, bool Verbose = false) const {
    SILPrintContext PrintCtx(OS, Verbose);
    print(PrintCtx);
  }

  /// Pretty-print the SILFunction with the context \p PrintCtx.
  void print(SILPrintContext &PrintCtx) const;

  /// Pretty-print the SILFunction's name using SIL syntax,
  /// '@function_mangled_name'.
  void printName(raw_ostream &OS) const;

  /// Assigns consecutive numbers to all the SILNodes in the function.
  /// For instructions, both the instruction node and the value nodes of
  /// any results will be assigned numbers; the instruction node will
  /// be numbered the same as the first result, if there are any results.
  void numberValues(llvm::DenseMap<const SILNode*, unsigned> &nodeToNumberMap)
    const;

  ASTContext &getASTContext() const;

  /// This function is meant for use from the debugger.  You can just say 'call
  /// F->viewCFG()' and a ghostview window should pop up from the program,
  /// displaying the CFG of the current function with the code for each basic
  /// block inside.  This depends on there being a 'dot' and 'gv' program in
  /// your path.
  void viewCFG() const;
  /// Like ViewCFG, but the graph does not show the contents of basic blocks.
  void viewCFGOnly() const;

};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SILFunction &F) {
  F.print(OS);
  return OS;
}

} // end swift namespace

//===----------------------------------------------------------------------===//
// ilist_traits for SILFunction
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::swift::SILFunction> :
public ilist_node_traits<::swift::SILFunction> {
  using SILFunction = ::swift::SILFunction;

public:
  static void deleteNode(SILFunction *V) { V->~SILFunction(); }

private:
  void createNode(const SILFunction &);
};

} // end llvm namespace

//===----------------------------------------------------------------------===//
// Inline SIL implementations
//===----------------------------------------------------------------------===//

namespace swift {

inline bool SILBasicBlock::isEntry() const {
  return this == &*getParent()->begin();
}

inline SILModule &SILInstruction::getModule() const {
  return getFunction()->getModule();
}

} // end swift namespace

#endif

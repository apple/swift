//===--- IRGenSIL.cpp - Swift Per-Function IR Generation ------------------===//
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
//  This file implements basic setup and teardown for the class which
//  performs IR generation for function bodies.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/TerminatorUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SaveAndRestore.h"

#include "CallEmission.h"
#include "EntryPointArgumentEmission.h"
#include "Explosion.h"
#include "GenArchetype.h"
#include "GenBuiltin.h"
#include "GenCall.h"
#include "GenCast.h"
#include "GenClass.h"
#include "GenConstant.h"
#include "GenDecl.h"
#include "GenEnum.h"
#include "GenExistential.h"
#include "GenFunc.h"
#include "GenHeap.h"
#include "GenIntegerLiteral.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenOpaque.h"
#include "GenPack.h"
#include "GenPointerAuth.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenStruct.h"
#include "GenTuple.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenModule.h"
#include "MetadataLayout.h"
#include "MetadataRequest.h"
#include "NativeConventionSchema.h"
#include "ReferenceTypeInfo.h"

#define DEBUG_TYPE "irgensil"

using namespace swift;
using namespace irgen;

namespace {

class LoweredValue;

struct DynamicallyEnforcedAddress {
  Address Addr;
  llvm::Value *ScratchBuffer;
};

struct CoroutineState {
  Address Buffer;
  llvm::Value *Continuation;
  TemporarySet Temporaries;
};
  
/// Represents a SIL value lowered to IR, in one of these forms:
/// - an Address, corresponding to a SIL address value;
/// - an Explosion of (unmanaged) Values, corresponding to a SIL "register"; or
/// - a CallEmission for a partially-applied curried function or method.
class LoweredValue {
public:
  enum class Kind {
    /// The first two LoweredValue kinds correspond to a SIL address value.
    ///
    /// The LoweredValue of an existential alloc_stack keeps an owning container
    /// in addition to the address of the allocated buffer.
    /// Depending on the allocated type, the container may be equal to the
    /// buffer itself (for types with known sizes) or it may be the address
    /// of a fixed-size container which points to the heap-allocated buffer.
    /// In this case the address-part may be null, which means that the buffer
    /// is not allocated yet.
    ContainedAddress,

    /// The LoweredValue of a resilient, generic, or loadable typed alloc_stack
    /// keeps an optional stackrestore point in addition to the address of the
    /// allocated buffer. For all other address values the stackrestore point is
    /// just null.
    /// If the stackrestore point is set (currently, this might happen for
    /// opaque types: generic and resilient) the deallocation of the stack must
    /// reset the stack pointer to this point.
    StackAddress,

    /// A @box together with the address of the box value.
    OwnedAddress,

    /// The lowered value of a begin_access instruction using dynamic
    /// enforcement.
    DynamicallyEnforcedAddress,

    /// A normal value, represented as an exploded array of llvm Values.
    ExplosionVector,

    /// The special case of a single explosion.
    SingletonExplosion,

    /// A value that represents a function pointer.
    FunctionPointer,

    /// A value that represents an Objective-C method that must be called with
    /// a form of objc_msgSend.
    ObjCMethod,

    /// The special case of an empty explosion.
    EmptyExplosion,

    /// A coroutine state.
    CoroutineState,
  };
  
  Kind kind;
  
private:
  using ExplosionVector = SmallVector<llvm::Value *, 4>;
  using SingletonExplosion = llvm::Value*;

  using Members = ExternalUnionMembers<ContainedAddress,
                                       StackAddress,
                                       OwnedAddress,
                                       DynamicallyEnforcedAddress,
                                       ExplosionVector,
                                       SingletonExplosion,
                                       FunctionPointer,
                                       ObjCMethod,
                                       CoroutineState,
                                       void>;
  
  static Members::Index getMemberIndexForKind(Kind kind) {
    switch (kind) {
    case Kind::ContainedAddress: return Members::indexOf<ContainedAddress>();
    case Kind::StackAddress: return Members::indexOf<StackAddress>();
    case Kind::OwnedAddress: return Members::indexOf<OwnedAddress>();
    case Kind::DynamicallyEnforcedAddress: return Members::indexOf<DynamicallyEnforcedAddress>();
    case Kind::ExplosionVector: return Members::indexOf<ExplosionVector>();
    case Kind::SingletonExplosion: return Members::indexOf<SingletonExplosion>();
    case Kind::FunctionPointer: return Members::indexOf<FunctionPointer>();
    case Kind::ObjCMethod: return Members::indexOf<ObjCMethod>();
    case Kind::CoroutineState: return Members::indexOf<CoroutineState>();
    case Kind::EmptyExplosion: return Members::indexOf<void>();
    }
    llvm_unreachable("bad kind");
  }
  ExternalUnion<Kind, Members, getMemberIndexForKind> Storage;

  explicit LoweredValue(llvm::Value *singletonValue)
      : kind(Kind::SingletonExplosion) {
    Storage.emplace<SingletonExplosion>(kind, singletonValue);
  }

public:

  /// Create an address value without a stack restore point.
  LoweredValue(const Address &address)
      : kind(Kind::StackAddress) {
    Storage.emplace<StackAddress>(kind, address);
  }

  /// Create an address value with an optional stack restore point.
  LoweredValue(const StackAddress &address)
      : kind(Kind::StackAddress) {
    Storage.emplace<StackAddress>(kind, address);
  }

  /// Create an address value using dynamic enforcement.
  LoweredValue(const DynamicallyEnforcedAddress &address)
      : kind(Kind::DynamicallyEnforcedAddress) {
    Storage.emplace<DynamicallyEnforcedAddress>(kind, address);
  }
  
  enum ContainerForUnallocatedAddress_t { ContainerForUnallocatedAddress };

  /// Create an address value for an alloc_stack, consisting of a container and
  /// a not yet allocated buffer.
  LoweredValue(const Address &container, ContainerForUnallocatedAddress_t)
      : kind(Kind::ContainedAddress) {
    Storage.emplace<ContainedAddress>(kind, container, Address());
  }
  
  /// Create an address value for an alloc_stack, consisting of a container and
  /// the address of the allocated buffer.
  LoweredValue(const ContainedAddress &address)
      : kind(Kind::ContainedAddress) {
    Storage.emplace<ContainedAddress>(kind, address);
  }
  
  LoweredValue(const FunctionPointer &fn)
      : kind(Kind::FunctionPointer) {
    Storage.emplace<FunctionPointer>(kind, fn);
  }

  LoweredValue(ObjCMethod &&objcMethod)
      : kind(Kind::ObjCMethod) {
    Storage.emplace<ObjCMethod>(kind, std::move(objcMethod));
  }

  LoweredValue(Explosion &e) {
    auto elts = e.claimAll();
    if (elts.empty()) {
      kind = Kind::EmptyExplosion;
    } else if (elts.size() == 1) {
      kind = Kind::SingletonExplosion;
      Storage.emplace<SingletonExplosion>(kind, elts.front());
    } else {
      kind = Kind::ExplosionVector;
      auto &explosion = Storage.emplace<ExplosionVector>(kind);
      explosion.append(elts.begin(), elts.end());
    }
  }

  LoweredValue(const OwnedAddress &boxWithAddress)
      : kind(Kind::OwnedAddress) {
    Storage.emplace<OwnedAddress>(kind, boxWithAddress);
  }

  LoweredValue(CoroutineState &&state)
      : kind(Kind::CoroutineState) {
    Storage.emplace<CoroutineState>(kind, std::move(state));
  }

  LoweredValue(LoweredValue &&lv)
      : kind(lv.kind) {
    Storage.moveConstruct(kind, std::move(lv.Storage));
  }

  static LoweredValue forSingletonExplosion(llvm::Value *value) {
    return LoweredValue(value);
  }

  LoweredValue &operator=(LoweredValue &&lv) {
    Storage.moveAssign(kind, lv.kind, std::move(lv.Storage));
    kind = lv.kind;
    return *this;
  }

  ~LoweredValue() {
    Storage.destruct(kind);
  }
  
  bool isAddress() const {
    return (kind == Kind::StackAddress ||
            kind == Kind::DynamicallyEnforcedAddress);
  }
  bool isUnallocatedAddressInBuffer() const {
    return kind == Kind::ContainedAddress &&
           !Storage.get<ContainedAddress>(kind).getAddress().isValid();
  }
  bool isBoxWithAddress() const {
    return kind == Kind::OwnedAddress;
  }
  
  const StackAddress &getStackAddress() const {
    return Storage.get<StackAddress>(kind);
  }
  
  Address getContainerOfAddress() const {
    const auto &containedAddress = Storage.get<ContainedAddress>(kind);
    assert(containedAddress.getContainer().isValid() && "address has no container");
    return containedAddress.getContainer();
  }

  Address getAddressInContainer() const {
    const auto &containedAddress = Storage.get<ContainedAddress>(kind);
    assert(containedAddress.getContainer().isValid() &&
           "address has no container");
    return containedAddress.getAddress();
  }

  const DynamicallyEnforcedAddress &getDynamicallyEnforcedAddress() const {
    return Storage.get<DynamicallyEnforcedAddress>(kind);
  }

  Address getAnyAddress() const {
    if (kind == LoweredValue::Kind::StackAddress) {
      return Storage.get<StackAddress>(kind).getAddress();
    } else if (kind == LoweredValue::Kind::ContainedAddress) {
      return getAddressInContainer();
    } else {
      return getDynamicallyEnforcedAddress().Addr;
    }
  }
 
  Address getAddressOfBox() const {
    return Storage.get<OwnedAddress>(kind).getAddress();
  }

  ArrayRef<llvm::Value *> getKnownExplosionVector() const {
    return Storage.get<ExplosionVector>(kind);
  }

  llvm::Value *getKnownSingletonExplosion() const {
    return Storage.get<SingletonExplosion>(kind);
  }
  
  const FunctionPointer &getFunctionPointer() const {
    return Storage.get<FunctionPointer>(kind);
  }
  
  const ObjCMethod &getObjCMethod() const {
    return Storage.get<ObjCMethod>(kind);
  }

  const CoroutineState &getCoroutineState() const {
    return Storage.get<CoroutineState>(kind);
  }

  /// Produce an explosion for this lowered value.  Note that many
  /// different storage kinds can be turned into an explosion.
  Explosion getExplosion(IRGenFunction &IGF, SILType type) const {
    Explosion e;
    getExplosion(IGF, type, e);
    return e;
  }
  void getExplosion(IRGenFunction &IGF, SILType type, Explosion &ex) const;

  /// Produce an explosion which is known to be a single value.
  llvm::Value *getSingletonExplosion(IRGenFunction &IGF, SILType type) const;

  /// Produce a callee from this value.
  Callee getCallee(IRGenFunction &IGF, llvm::Value *selfValue,
                   CalleeInfo &&calleeInfo) const;
};

using PHINodeVector = llvm::TinyPtrVector<llvm::PHINode*>;
  
/// Represents a lowered SIL basic block. This keeps track
/// of SIL branch arguments so that they can be lowered to LLVM phi nodes.
struct LoweredBB {
  llvm::BasicBlock *bb;
  PHINodeVector phis;
  
  LoweredBB() = default;
  explicit LoweredBB(llvm::BasicBlock *bb, PHINodeVector &&phis)
    : bb(bb), phis(std::move(phis))
  {}
};

/// Visits a SIL Function and generates LLVM IR.
class IRGenSILFunction :
  public IRGenFunction, public SILInstructionVisitor<IRGenSILFunction>
{
public:
  llvm::DenseMap<SILValue, LoweredValue> LoweredValues;
  llvm::DenseMap<SILValue, StackAddress> LoweredPartialApplyAllocations;
  llvm::DenseMap<SILType, LoweredValue> LoweredUndefs;

  /// All alloc_ref instructions which allocate the object on the stack.
  llvm::SmallPtrSet<SILInstruction *, 8> StackAllocs;

  /// With closure captures it is actually possible to have two function
  /// arguments that both have the same name. Until this is fixed, we need to
  /// also hash the ArgNo here.
  using StackSlotKey =
      std::pair<unsigned, std::pair<const SILDebugScope *, StringRef>>;
  /// Keeps track of the mapping of source variables to -O0 shadow copy allocas.
  llvm::SmallDenseMap<StackSlotKey, Address, 8> ShadowStackSlots;
  llvm::SmallDenseMap<llvm::Value *, Address, 8> TaskAllocStackSlots;
  llvm::SmallDenseMap<Decl *, Identifier, 8> AnonymousVariables;
  llvm::SmallDenseSet<llvm::Value *, 4> PackShapeExpressions;
  /// To avoid inserting elements into ValueDomPoints twice.
  llvm::SmallDenseSet<llvm::Value *, 8> ValueVariables;
  /// Holds the DominancePoint of values that are storage for a source variable.
  SmallVector<std::pair<llvm::Value *, DominancePoint>, 8> ValueDomPoints;
  unsigned NumAnonVars = 0;

  /// Accumulative amount of allocated bytes on the stack. Used to limit the
  /// size for stack promoted objects.
  /// We calculate it on demand, so that we don't have to do it if the
  /// function does not have any stack promoted allocations.
  int EstimatedStackSize = -1;

  llvm::MapVector<SILBasicBlock *, LoweredBB> LoweredBBs;
  
  // Destination basic blocks for condfail traps.
  llvm::SmallVector<llvm::BasicBlock *, 8> FailBBs;

  SILFunction *CurSILFn;
  // If valid, the address by means of which a return--which is direct in
  // SIL--is passed indirectly in IR.  Such indirection is necessary when the
  // value which would be returned directly cannot fit into registers.
  Address IndirectReturn;

  // A cached dominance analysis.
  std::unique_ptr<DominanceInfo> Dominance;
  
  IRGenSILFunction(IRGenModule &IGM, SILFunction *f);
  ~IRGenSILFunction();
  
  /// Generate IR for the SIL Function.
  void emitSILFunction();

  /// Calculates EstimatedStackSize.
  void estimateStackSize();

  inline bool isAddress(SILValue v) const {
    SILType type = v->getType();
    return type.isAddress() || type.getASTType() == IGM.Context.TheRawPointerType;
  }

  void setLoweredValue(SILValue v, LoweredValue &&lv) {
    auto inserted = LoweredValues.insert({v, std::move(lv)});
    assert(inserted.second && "already had lowered value for sil value?!");
    (void)inserted;
  }
  
  /// Create a new Address corresponding to the given SIL address value.
  void setLoweredAddress(SILValue v, const Address &address) {
    assert(isAddress(v) && "address for non-address value?!");
    setLoweredValue(v, address);
  }

  void setLoweredStackAddress(SILValue v, const StackAddress &address) {
    assert(isAddress(v) && "address for non-address value?!");
    setLoweredValue(v, address);
  }

  void setLoweredDynamicallyEnforcedAddress(SILValue v,
                                            const Address &address,
                                            llvm::Value *scratch) {
    assert(isAddress(v) && "address for non-address value?!");
    setLoweredValue(v, DynamicallyEnforcedAddress{address, scratch});
  }
  
  void setContainerOfUnallocatedAddress(SILValue v,
                                        const Address &buffer) {
    assert(isAddress(v) && "address for non-address value?!");
    setLoweredValue(v,
      LoweredValue(buffer, LoweredValue::ContainerForUnallocatedAddress));
  }
  
  void overwriteAllocatedAddress(SILValue v, const Address &address) {
    assert(isAddress(v) && "address for non-address value?!");
    auto it = LoweredValues.find(v);
    assert(it != LoweredValues.end() && "no existing entry for overwrite?");
    assert(it->second.isUnallocatedAddressInBuffer() &&
           "not an unallocated address");
    it->second = ContainedAddress(it->second.getContainerOfAddress(), address);
  }

  void setAllocatedAddressForBuffer(SILValue v, const Address &allocedAddress);

  /// Create a new Explosion corresponding to the given SIL value.
  void setLoweredExplosion(SILValue v, Explosion &e) {
    assert(v->getType().isObject() && "explosion for address value?!");
    setLoweredValue(v, LoweredValue(e));
  }
  void setLoweredSingletonExplosion(SILValue v, llvm::Value *value) {
    assert(v->getType().isObject() && "explosion for address value?!");
    setLoweredValue(v, LoweredValue::forSingletonExplosion(value));
  }

  void setCorrespondingLoweredValues(SILInstructionResultArray results,
                                     Explosion &allValues) {
    for (SILValue result : results) {
      auto resultType = result->getType();
      auto &resultTI = getTypeInfo(resultType);

      // If the value is indirect, the next explosion value should just be
      // a pointer.
      if (resultType.isAddress()) {
        auto pointer = allValues.claimNext();
        setLoweredAddress(result, resultTI.getAddressForPointer(pointer));
        continue;
      }

      // Otherwise, claim out the right number of values.
      Explosion resultValue;
      cast<LoadableTypeInfo>(resultTI).reexplode(*this, allValues, resultValue);
      setLoweredExplosion(result, resultValue);
    }
  }

  void setLoweredBox(SILValue v, const OwnedAddress &box) {
    assert(v->getType().isObject() && "box for address value?!");
    setLoweredValue(v, LoweredValue(box));
  }

  /// Map the given SIL value to a FunctionPointer value.
  void setLoweredFunctionPointer(SILValue v, const FunctionPointer &fnPtr) {
    assert(v->getType().isObject() && "function for address value?!");
    assert(v->getType().is<SILFunctionType>() &&
           "function for non-function value?!");
    setLoweredValue(v, fnPtr);
  }

  /// Create a new Objective-C method corresponding to the given SIL value.
  void setLoweredObjCMethod(SILValue v, SILDeclRef method) {
    assert(v->getType().isObject() && "function for address value?!");
    assert(v->getType().is<SILFunctionType>() &&
           "function for non-function value?!");
    setLoweredValue(v, ObjCMethod{method, SILType(), false});
  }

  /// Create a new Objective-C method corresponding to the given SIL value that
  /// starts its search from the given search type.
  ///
  /// Unlike \c setLoweredObjCMethod, which finds the method in the actual
  /// runtime type of the object, this routine starts at the static type of the
  /// object and searches up the class hierarchy (toward superclasses).
  ///
  /// \param searchType The class from which the Objective-C runtime will start
  /// its search for a method.
  ///
  /// \param startAtSuper Whether we want to start at the superclass of the
  /// static type (vs. the static type itself).
  void setLoweredObjCMethodBounded(SILValue v, SILDeclRef method,
                                   SILType searchType, bool startAtSuper) {
    assert(v->getType().isObject() && "function for address value?!");
    assert(v->getType().is<SILFunctionType>() &&
           "function for non-function value?!");
    setLoweredValue(v, ObjCMethod{method, searchType, startAtSuper});
  }

  void setLoweredCoroutine(SILValue tokenResult, CoroutineState &&state) {
    setLoweredValue(tokenResult, std::move(state));
  }

  LoweredValue &getUndefLoweredValue(SILType t) {
    auto found = LoweredUndefs.find(t);
    if (found != LoweredUndefs.end())
      return found->second;
    
    auto &ti = getTypeInfo(t);
    switch (t.getCategory()) {
    case SILValueCategory::Address: {
      Address undefAddr = ti.getAddressForPointer(
                  llvm::UndefValue::get(ti.getStorageType()->getPointerTo()));
      LoweredUndefs.insert({t, LoweredValue(undefAddr)});
      break;
    }

    case SILValueCategory::Object: {
      auto schema = ti.getSchema();
      Explosion e;
      for (auto &elt : schema) {
        assert(!elt.isAggregate()
               && "non-scalar element in loadable type schema?!");
        e.add(llvm::UndefValue::get(elt.getScalarType()));
      }
      LoweredUndefs.insert({t, LoweredValue(e)});
      break;
    }
    }
    
    found = LoweredUndefs.find(t);
    assert(found != LoweredUndefs.end());
    return found->second;
  }
  
  /// Get the LoweredValue corresponding to the given SIL value, which must
  /// have been lowered.
  LoweredValue &getLoweredValue(SILValue v) {
    if (isa<SILUndef>(v))
      return getUndefLoweredValue(v->getType());
    
    auto foundValue = LoweredValues.find(v);
    assert(foundValue != LoweredValues.end() &&
           "no lowered explosion for sil value!");
    return foundValue->second;
  }
  
  /// Get the Address of a SIL value of address type, which must have been
  /// lowered.
  Address getLoweredAddress(SILValue v) {
    return getLoweredValue(v).getAnyAddress();
  }

  StackAddress getLoweredStackAddress(SILValue v) {
    return getLoweredValue(v).getStackAddress();
  }

  llvm::Value *getLoweredDynamicEnforcementScratchBuffer(BeginAccessInst *v) {
    return getLoweredValue(v).getDynamicallyEnforcedAddress().ScratchBuffer;
  }

  const CoroutineState &getLoweredCoroutine(SILValue v) {
    return getLoweredValue(v).getCoroutineState();
  }

  /// Add the unmanaged LLVM values lowered from a SIL value to an explosion.
  void getLoweredExplosion(SILValue v, Explosion &e) {
    getLoweredValue(v).getExplosion(*this, v->getType(), e);
  }
  /// Create an Explosion containing the unmanaged LLVM values lowered from a
  /// SIL value.
  Explosion getLoweredExplosion(SILValue v) {
    return getLoweredValue(v).getExplosion(*this, v->getType());
  }

  /// Return the single member of the lowered explosion for the
  /// given SIL value.
  llvm::Value *getLoweredSingletonExplosion(SILValue v) {
    return getLoweredValue(v).getSingletonExplosion(*this, v->getType());
  }
  
  LoweredBB &getLoweredBB(SILBasicBlock *bb) {
    auto foundBB = LoweredBBs.find(bb);
    assert(foundBB != LoweredBBs.end() && "no llvm bb for sil bb?!");
    return foundBB->second;
  }

  TypeExpansionContext getExpansionContext() {
    return TypeExpansionContext(*CurSILFn);
  }

  SILType getLoweredTypeInContext(SILType ty) {
    return CurSILFn->getModule()
        .Types.getLoweredType(ty.getASTType(), getExpansionContext())
        .getCategoryType(ty.getCategory());
  }

  StringRef getOrCreateAnonymousVarName(VarDecl *Decl) {
    Identifier &Name = AnonymousVariables[Decl];
    if (Name.empty()) {
      {
        llvm::SmallString<64> NameBuffer;
        llvm::raw_svector_ostream S(NameBuffer);
        S << "$_" << NumAnonVars++;
        Name = IGM.Context.getIdentifier(NameBuffer);
      }
      AnonymousVariables.insert({Decl, Name});
    }
    return Name.str();
  }

  template <class DebugVarCarryingInst>
  StringRef getVarName(DebugVarCarryingInst *i, bool &IsAnonymous) {
    auto VarInfo = i->getVarInfo();
    if (!VarInfo)
      return StringRef();

    StringRef Name = i->getVarInfo()->Name;
    // The $match variables generated by the type checker are not
    // guaranteed to be unique within their scope, but they have
    // unique VarDecls.
    if ((Name.empty() || Name == "$match") && i->getDecl()) {
      IsAnonymous = true;
      return getOrCreateAnonymousVarName(i->getDecl());
    }
    return Name;
  }

  /// To make it unambiguous whether a `var` binding has been initialized,
  /// zero-initialize the shadow copy alloca. LLDB uses the first pointer-sized
  /// field to recognize to detect uninitialized variables. This can be
  /// removed once swiftc switches to @llvm.dbg.addr() intrinsics.
  void zeroInit(llvm::AllocaInst *AI) {
    if (!AI)
      return;

    // Only do this at -Onone.
    auto optAllocationSize = AI->getAllocationSizeInBits(IGM.DataLayout);
    if (!optAllocationSize)
      return;
    uint64_t Size = *optAllocationSize / 8;
    if (IGM.IRGen.Opts.shouldOptimize() || !Size)
      return;

    llvm::IRBuilder<> ZeroInitBuilder(AI->getNextNode());
    ZeroInitBuilder.SetInsertPoint(getEarliestInsertionPoint()->getParent(),
                                   getEarliestInsertionPoint()->getIterator());
    // No debug location is how LLVM marks prologue instructions.
    ZeroInitBuilder.SetCurrentDebugLocation(nullptr);
    ZeroInitBuilder.CreateMemSet(
        AI, llvm::ConstantInt::get(IGM.Int8Ty, 0),
        Size, llvm::MaybeAlign(AI->getAlign()));
  }

  /// Try to emit an inline assembly gadget which extends the lifetime of
  /// \p Var. Returns whether or not this was successful.
  bool emitLifetimeExtendingUse(llvm::Value *Var) {
    llvm::Type *ArgTys;
    auto *Ty = Var->getType();
    // Vectors, Pointers and Floats are expected to fit into a register.
    if (Ty->isPointerTy() || Ty->isFloatingPointTy() || Ty->isVectorTy())
      ArgTys = {Ty};
    else {
      // If this is not a scalar or vector type, we can't handle it.
      if (isa<llvm::StructType>(Ty))
        return false;
      // The storage is guaranteed to be no larger than the register width.
      // Extend the storage so it would fit into a register.
      llvm::Type *IntTy;
      switch (IGM.getClangASTContext().getTargetInfo().getRegisterWidth()) {
      case 64:
        IntTy = IGM.Int64Ty;
        break;
      case 32:
        IntTy = IGM.Int32Ty;
        break;
      default:
        llvm_unreachable("unsupported register width");
      }
      ArgTys = {IntTy};
      Var = Var->getType()->getIntegerBitWidth() < IntTy->getIntegerBitWidth()
                ? Builder.CreateZExtOrBitCast(Var, IntTy)
                : Builder.CreateTruncOrBitCast(Var, IntTy);
    }
    // Emit an empty inline assembler expression depending on the register.
    auto *AsmFnTy = llvm::FunctionType::get(IGM.VoidTy, ArgTys, false);
    auto *InlineAsm = llvm::InlineAsm::get(AsmFnTy, "", "r", true);
    Builder.CreateAsmCall(InlineAsm, Var);
    return true;
  }

  /// At -Onone, forcibly keep all LLVM values that are tracked by
  /// debug variables alive by inserting an empty inline assembler
  /// expression depending on the value in the blocks dominated by the
  /// value.
  ///
  /// This is used only in async functions.
  void emitDebugVariableRangeExtension(const SILBasicBlock *CurBB) {
    if (IGM.IRGen.Opts.shouldOptimize())
      return;
    for (auto &Variable : ValueDomPoints) {
      llvm::Value *Var = Variable.first;
      DominancePoint VarDominancePoint = Variable.second;
      if (getActiveDominancePoint() == VarDominancePoint ||
          isActiveDominancePointDominatedBy(VarDominancePoint)) {
        bool ExtendedLifetime = emitLifetimeExtendingUse(Var);
        if (!ExtendedLifetime)
          continue;

        // Propagate dbg.values for Var into the current basic block. Note
        // that this shouldn't be necessary. LiveDebugValues should be doing
        // this but can't in general because it currently only tracks register
        // locations.
        llvm::BasicBlock *BB =
            isa<llvm::Instruction>(Var)
                ? cast<llvm::Instruction>(Var)->getParent()
                : &cast<llvm::Argument>(Var)->getParent()->getEntryBlock();
        llvm::BasicBlock *CurBB = Builder.GetInsertBlock();
        if (BB == CurBB)
          // The current basic block must be a successor of the dbg.value().
          continue;

        llvm::SmallVector<llvm::DbgValueInst *, 4> DbgValues;
        llvm::findDbgValues(DbgValues, Var);
        for (auto *DVI : DbgValues)
          if (DVI->getParent() == BB)
            IGM.DebugInfo->getBuilder().insertDbgValueIntrinsic(
                DVI->getValue(), DVI->getVariable(), DVI->getExpression(),
                DVI->getDebugLoc(), &*CurBB->getFirstInsertionPt());
      }
    }
  }

  
  /// Account for bugs in LLVM.
  ///
  /// - When a variable is spilled into a stack slot, LiveDebugValues fails to
  ///   recognize a restore of that slot for a different variable.
  ///
  /// - The LLVM type legalizer currently doesn't update debug
  ///   intrinsics when a large value is split up into smaller
  ///   pieces. Note that this heuristic as a bit too conservative
  ///   on 32-bit targets as it will also fire for doubles.
  ///
  /// - CodeGen Prepare may drop dbg.values pointing to PHI instruction.
  bool needsShadowCopy(llvm::Value *Storage) {
    // If we have a constant data vector, we always need a shadow copy due to
    // bugs in LLVM.
    if (isa<llvm::ConstantDataVector>(Storage))
      return true;
    return !isa<llvm::Constant>(Storage);
  }

#ifndef NDEBUG
  /// Check if \p Val can be stored into \p Alloca, and emit some diagnostic
  /// info if it can't.
  bool canAllocaStoreValue(Address Alloca, llvm::Value *Val,
                           SILDebugVariable VarInfo,
                           const SILDebugScope *Scope) {
    bool canStore = Alloca.getElementType() == Val->getType();
    if (canStore)
      return true;
    llvm::errs() << "Invalid shadow copy:\n"
                 << "  Value : " << *Val << "\n"
                 << "  Alloca: " << *Alloca.getAddress() << "\n"
                 << "---\n"
                 << "Previous shadow copy into " << VarInfo.Name
                 << " in the same scope!\n"
                 << "Scope:\n";
    Scope->print(getSILModule());
    return false;
  }
#endif

  static bool isCallToSwiftTaskAlloc(llvm::Value *val) {
    auto *call = dyn_cast<llvm::CallInst>(val);
    if (!call)
      return false;
    auto *callee = call->getCalledFunction();
    if (!callee)
      return false;
    auto isTaskAlloc = callee->getName().equals("swift_task_alloc");
    return isTaskAlloc;
  }

  static bool isTaskAlloc(llvm::Value *Storage) {
    while (Storage) {
      if (auto *LdInst = dyn_cast<llvm::LoadInst>(Storage))
        Storage = LdInst->getOperand(0);
      else if (auto *GEPInst = dyn_cast<llvm::GetElementPtrInst>(Storage))
        Storage = GEPInst->getOperand(0);
      else if (auto *BCInst = dyn_cast<llvm::BitCastInst>(Storage))
        Storage = BCInst->getOperand(0);
      else if (auto *CallInst = dyn_cast<llvm::CallInst>(Storage))
        return isCallToSwiftTaskAlloc(CallInst);
      else
        break;
    }
    return false;
  }

  llvm::Value *emitTaskAllocShadowCopy(llvm::Value *Storage,
                                       const SILDebugScope *Scope,
                                       bool Init) {
    auto getRec = [&](llvm::Instruction *Orig) {
      llvm::Value *Inner =
        emitTaskAllocShadowCopy(Orig->getOperand(0), Scope, Init);
      if (!Init)
        return Inner;

      llvm::Instruction *Cloned = Orig->clone();
      Cloned->setOperand(0, Inner);
      Cloned->insertBefore(Orig);
      return static_cast<llvm::Value *>(Cloned);
    };
    if (auto *LdInst = dyn_cast<llvm::LoadInst>(Storage))
      return getRec(LdInst);
    if (auto *GEPInst = dyn_cast<llvm::GetElementPtrInst>(Storage))
      return getRec(GEPInst);
    if (auto *BCInst = dyn_cast<llvm::BitCastInst>(Storage))
      return getRec(BCInst);
    if (auto *CallInst = dyn_cast<llvm::CallInst>(Storage)) {
      assert(isTaskAlloc(CallInst));
      auto Align = IGM.getPointerAlignment();
      auto &Alloca = TaskAllocStackSlots[CallInst];
      if (!Alloca.isValid())
        Alloca = createAlloca(Storage->getType(), Align, "taskalloc.debug");
      if (Init) {
        zeroInit(cast<llvm::AllocaInst>(Alloca.getAddress()));
        ArtificialLocation AutoRestore(Scope, IGM.DebugInfo.get(), Builder);
        auto *Store =
          Builder.CreateStore(Storage, Alloca.getAddress(), Align);
        Store->moveAfter(CallInst);
      }
      return Alloca.getAddress();
    }
    return Storage;
  }

  /// Unconditionally emit a stack shadow copy of an \c llvm::Value.
  Address emitShadowCopy(llvm::Value *Storage, const SILDebugScope *Scope,
                         SILDebugVariable VarInfo,
                         llvm::Optional<Alignment> _Align, bool Init,
                         bool WasMoved) {
    auto Align = _Align.value_or(IGM.getPointerAlignment());
    unsigned ArgNo = VarInfo.ArgNo;
    auto &Alloca = ShadowStackSlots[{ArgNo, {Scope, VarInfo.Name}}];

    if (!Alloca.isValid())
      Alloca = createAlloca(Storage->getType(), Align, VarInfo.Name + ".debug");

    // If our value was ever moved, we may be reinitializing the shadow
    // copy. Insert the bit cast so that the types line up and we do not get the
    // duplicate shadow copy error (which triggers based off of type
    // differences).
    auto Address = Alloca;
    if (WasMoved) {
      auto nonPtrAllocaType = Alloca.getElementType();
      if (nonPtrAllocaType != Storage->getType())
        Address = Builder.CreateElementBitCast(Address, Storage->getType());
    }

    // This might happen because of non-loadable types.
    if (Storage->stripPointerCasts()->getType() == Alloca.getElementType())
      Storage = Storage->stripPointerCasts();

    assert(canAllocaStoreValue(Address, Storage, VarInfo, Scope) &&
           "bad scope?");

    if (Init) {
      // Zero init our bare allocation.
      zeroInit(cast<llvm::AllocaInst>(Alloca.getAddress()));
      ArtificialLocation AutoRestore(Scope, IGM.DebugInfo.get(), Builder);
      // But store into the address with maybe bitcast.
      Builder.CreateStore(Storage, Address.getAddress(), Align);
    }

    // If this allocation was moved at some point, we might be reinitializing a
    // shadow copy. In such a case, lets insert an identity bit cast so that our
    // callers will use this address with respect to the place where we
    // reinit. Otherwise, callers may use the alloca's insert point. The
    // optimizer will eliminate these later without issue.
    return Alloca;
  }

  bool shouldShadowVariable(SILDebugVariable varInfo, bool isAnonymous) {
    return !IGM.IRGen.Opts.DisableDebuggerShadowCopies
      && !IGM.IRGen.Opts.shouldOptimize()
      && !isAnonymous;
  }

  bool shouldShadowStorage(llvm::Value *Storage) {
    return !isa<llvm::AllocaInst>(Storage)
      && !isa<llvm::UndefValue>(Storage)
      && needsShadowCopy(Storage);
  }

  /// At -Onone, emit a shadow copy of an Address in an alloca, so the
  /// register allocator doesn't elide the dbg.value intrinsic when
  /// register pressure is high.  There is a trade-off to this: With
  /// shadow copies, we lose the precise lifetime.
  llvm::Value *emitShadowCopyIfNeeded(llvm::Value *Storage,
                                      const SILDebugScope *Scope,
                                      SILDebugVariable VarInfo,
                                      bool IsAnonymous, bool WasMoved,
                                      llvm::Optional<Alignment> Align = None) {
    // Never emit shadow copies when optimizing, or if already on the stack.  No
    // debug info is emitted for refcounts either

    // Mark variables in async functions that don't generate a shadow copy for
    // lifetime extension, so they get spilled into the async context.
    if (!IGM.IRGen.Opts.shouldOptimize() && CurSILFn->isAsync())
      if (isa<llvm::AllocaInst>(Storage)) {
        if (emitLifetimeExtendingUse(Storage))
          if (ValueVariables.insert(Storage).second)
            ValueDomPoints.push_back({Storage, getActiveDominancePoint()});
      }

    // This condition must be consistent with emitPoisonDebugValueInst to avoid
    // generating extra shadow copies for debug_value [poison].
    if (!shouldShadowVariable(VarInfo, IsAnonymous)
        || !shouldShadowStorage(Storage)) {
      return Storage;
    }

    // Emit a shadow copy.
    auto shadow = emitShadowCopy(Storage, Scope, VarInfo, Align, true, WasMoved)
                      .getAddress();

    // Mark variables in async functions for lifetime extension, so they get
    // spilled into the async context.
    if (!IGM.IRGen.Opts.shouldOptimize() && CurSILFn->isAsync()) {
      if (emitLifetimeExtendingUse(shadow)) {
        if (ValueVariables.insert(shadow).second)
          ValueDomPoints.push_back({shadow, getActiveDominancePoint()});
      }
    }

    return shadow;
  }

  /// Like \c emitShadowCopyIfNeeded() but takes an \c Address instead of an
  /// \c llvm::Value.
  llvm::Value *emitShadowCopyIfNeeded(Address Storage,
                                      const SILDebugScope *Scope,
                                      SILDebugVariable VarInfo,
                                      bool IsAnonymous, bool WasMoved) {
    return emitShadowCopyIfNeeded(Storage.getAddress(), Scope, VarInfo,
                                  IsAnonymous, WasMoved,
                                  Storage.getAlignment());
  }

  /// Like \c emitShadowCopyIfNeeded() but takes an exploded value.
  void emitShadowCopyIfNeeded(SILValue &SILVal, const SILDebugScope *Scope,
                              SILDebugVariable VarInfo, bool IsAnonymous,
                              bool WasMoved,
                              llvm::SmallVectorImpl<llvm::Value *> &copy) {
    Explosion e = getLoweredExplosion(SILVal);

    // Only do this at -O0.
    if (!shouldShadowVariable(VarInfo, IsAnonymous)) {
      auto vals = e.claimAll();
      copy.append(vals.begin(), vals.end());

      // Mark variables in async functions for lifetime extension, so they get
      // spilled into the async context.
      if (!IGM.IRGen.Opts.shouldOptimize() && CurSILFn->isAsync())
        if (vals.begin() != vals.end()) {
          auto Value = vals.front();
          if (isa<llvm::Instruction>(Value) || isa<llvm::Argument>(Value))
            if (emitLifetimeExtendingUse(Value))
              if (ValueVariables.insert(Value).second)
                ValueDomPoints.push_back({Value, getActiveDominancePoint()});
        }
      return;
    }

    // Single or empty values.
    if (e.empty())
      return;
    
    if (e.size() == 1) {
      copy.push_back(emitShadowCopyIfNeeded(e.claimNext(), Scope, VarInfo,
                                            IsAnonymous, WasMoved));
      return;
    }

    unsigned ArgNo = VarInfo.ArgNo;
    auto &Alloca = ShadowStackSlots[{ArgNo, {Scope, VarInfo.Name}}];
    if (Alloca.isValid()) {
      (void)e.claimAll();
      // Async functions use the value of the artificial address.
      if (CurSILFn->isAsync()) {
        auto shadow = Alloca.getAddress();
        auto inst = cast<llvm::Instruction>(shadow);
        llvm::IRBuilder<> builder(inst->getNextNode());
        shadow =
            builder.CreateLoad(Alloca.getElementType(), Alloca.getAddress());
        copy.push_back(shadow);
        return;
      }
    } else {
      SILType Type = SILVal->getType();
      auto &LTI = cast<LoadableTypeInfo>(IGM.getTypeInfo(Type));
      Alloca =
        LTI.allocateStack(*this, Type, VarInfo.Name + ".debug").getAddress();
      zeroInit(cast<llvm::AllocaInst>(Alloca.getAddress()));
      ArtificialLocation AutoRestore(Scope, IGM.DebugInfo.get(), Builder);
      LTI.initialize(*this, e, Alloca, false /* isOutlined */);
      auto shadow = Alloca.getAddress();
      // Async functions use the value of the artificial address.
      if (CurSILFn->isAsync() && emitLifetimeExtendingUse(shadow))
        if (ValueVariables.insert(shadow).second)
          ValueDomPoints.push_back({shadow, getActiveDominancePoint()});
    }
    copy.push_back(Alloca.getAddress());
  }

  void emitPackCountDebugVariable(llvm::Value *Shape) {
    if (!PackShapeExpressions.insert(Shape).second)
      return;
    llvm::SmallString<8> Buf;
    unsigned Position = PackShapeExpressions.size() - 1;
    llvm::raw_svector_ostream(Buf) << "$pack_count_" << Position;
    auto Name = IGM.Context.getIdentifier(Buf.str());
    SILDebugVariable Var(Name.str(), true, 0);
    Shape = emitShadowCopyIfNeeded(Shape, getDebugScope(), Var, false,
                                   false /*was move*/);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitPackCountParameter(*this, Shape, Var);
  }

  /// Force all archetypes referenced by the type to be bound by this point.
  /// TODO: just make sure that we have a path to them that the debug info
  ///       can follow.
  void bindArchetypes(swift::Type Ty) {
    auto runtimeTy = IGM.getRuntimeReifiedType(Ty->getCanonicalType());
    if (!IGM.IRGen.Opts.shouldOptimize() && runtimeTy->hasArchetype())
      runtimeTy.visit([&](CanType t) {
        if (auto archetype = dyn_cast<ArchetypeType>(t))
          emitTypeMetadataRef(archetype);
        else if (auto packArchetype = dyn_cast<PackArchetypeType>(t))
          emitTypeMetadataRef(packArchetype);
        else if (auto packtype = dyn_cast<SILPackType>(t)) {
          llvm::Value *Shape = emitPackShapeExpression(t);
          emitPackCountDebugVariable(Shape);
        } else if (auto packtype = dyn_cast<PackType>(t)) {
          llvm::Value *Shape = emitPackShapeExpression(t);
          emitPackCountDebugVariable(Shape);
        }
      });
  }
  
  /// Emit debug info for a function argument or a local variable.
  template <typename StorageType>
  void emitDebugVariableDeclaration(
      StorageType Storage, DebugTypeInfo Ty, SILType SILTy,
      const SILDebugScope *DS, SILLocation VarLoc, SILDebugVariable VarInfo,
      IndirectionKind Indirection,
      AddrDbgInstrKind DbgInstrKind = AddrDbgInstrKind::DbgDeclare) {
    if (swift::TypeBase *ty = SILTy.getASTType().getPointer()) {
      if (MetatypeType *metaTy = dyn_cast<MetatypeType>(ty))
        ty = metaTy->getRootClass().getPointer();
    }

    assert(IGM.DebugInfo && "debug info not enabled");

    if (VarInfo.ArgNo) {
      PrologueLocation AutoRestore(IGM.DebugInfo.get(), Builder);
      IGM.DebugInfo->emitVariableDeclaration(
          Builder, Storage, Ty, DS, VarLoc, VarInfo, Indirection,
          ArtificialKind::RealValue, DbgInstrKind);
      return;
    }

    IGM.DebugInfo->emitVariableDeclaration(
        Builder, Storage, Ty, DS, VarLoc, VarInfo, Indirection,
        ArtificialKind::RealValue, DbgInstrKind);
  }

  void emitFailBB() {
    if (!FailBBs.empty()) {
      // Move the trap basic blocks to the end of the function.
      for (auto *FailBB : FailBBs) {
        auto &BlockList = CurFn->getBasicBlockList();
        BlockList.splice(BlockList.end(), BlockList, FailBB);
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // SIL instruction lowering
  //===--------------------------------------------------------------------===//

  void visitSILBasicBlock(SILBasicBlock *BB);

  void emitErrorResultVar(CanSILFunctionType FnTy,
                          SILResultInfo ErrorInfo,
                          DebugValueInst *DbgValue);
  void emitPoisonDebugValueInst(DebugValueInst *i);
  void emitDebugInfoForAllocStack(AllocStackInst *i, const TypeInfo &type,
                                  llvm::Value *addr);
  void visitAllocStackInst(AllocStackInst *i);
  void visitAllocPackInst(AllocPackInst *i);
  void visitAllocRefInst(AllocRefInst *i);
  void visitAllocRefDynamicInst(AllocRefDynamicInst *i);
  void visitAllocBoxInst(AllocBoxInst *i);

  void visitProjectBoxInst(ProjectBoxInst *i);

  void visitApplyInst(ApplyInst *i);
  void visitTryApplyInst(TryApplyInst *i);
  void visitFullApplySite(FullApplySite i);
  void visitPartialApplyInst(PartialApplyInst *i);
  void visitBuiltinInst(BuiltinInst *i);

  void visitFunctionRefBaseInst(FunctionRefBaseInst *i);
  void visitFunctionRefInst(FunctionRefInst *i);
  void visitDynamicFunctionRefInst(DynamicFunctionRefInst *i);
  void visitPreviousDynamicFunctionRefInst(PreviousDynamicFunctionRefInst *i);
  void visitAllocGlobalInst(AllocGlobalInst *i);
  void visitGlobalAddrInst(GlobalAddrInst *i);
  void visitGlobalValueInst(GlobalValueInst *i);
  void visitBaseAddrForOffsetInst(BaseAddrForOffsetInst *i);

  void visitIntegerLiteralInst(IntegerLiteralInst *i);
  void visitFloatLiteralInst(FloatLiteralInst *i);
  void visitStringLiteralInst(StringLiteralInst *i);

  void visitLoadInst(LoadInst *i);
  void visitStoreInst(StoreInst *i);
  void visitAssignInst(AssignInst *i) {
    llvm_unreachable("assign is not valid in canonical SIL");
  }
  void visitAssignByWrapperInst(AssignByWrapperInst *i) {
    llvm_unreachable("assign_by_wrapper is not valid in canonical SIL");
  }
  void visitMarkUninitializedInst(MarkUninitializedInst *i) {
    llvm_unreachable("mark_uninitialized is not valid in canonical SIL");
  }
  void visitMarkFunctionEscapeInst(MarkFunctionEscapeInst *i) {
    llvm_unreachable("mark_function_escape is not valid in canonical SIL");
  }
  void visitLoadBorrowInst(LoadBorrowInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitDebugValueInst(DebugValueInst *i);
  void visitDebugStepInst(DebugStepInst *i);
  void visitRetainValueInst(RetainValueInst *i);
  void visitRetainValueAddrInst(RetainValueAddrInst *i);
  void visitCopyValueInst(CopyValueInst *i);
  void visitExplicitCopyValueInst(ExplicitCopyValueInst *i) {
    llvm_unreachable("Valid only when ownership is enabled");
  }
  void visitMoveValueInst(MoveValueInst *i) {
    auto e = getLoweredExplosion(i->getOperand());
    setLoweredExplosion(i, e);
  }
  void visitDropDeinitInst(DropDeinitInst *i) {
    auto e = getLoweredExplosion(i->getOperand());
    setLoweredExplosion(i, e);
  }
  void visitMarkMustCheckInst(MarkMustCheckInst *i) {
    llvm_unreachable("Invalid in Lowered SIL");
  }
  void visitMarkUnresolvedReferenceBindingInst(
      MarkUnresolvedReferenceBindingInst *i) {
    llvm_unreachable("Invalid in Lowered SIL");
  }
  void visitCopyableToMoveOnlyWrapperValueInst(
      CopyableToMoveOnlyWrapperValueInst *i) {
    auto e = getLoweredExplosion(i->getOperand());
    setLoweredExplosion(i, e);
  }
  void visitMoveOnlyWrapperToCopyableValueInst(
      MoveOnlyWrapperToCopyableValueInst *i) {
    auto e = getLoweredExplosion(i->getOperand());
    setLoweredExplosion(i, e);
  }
  void visitReleaseValueInst(ReleaseValueInst *i);
  void visitReleaseValueAddrInst(ReleaseValueAddrInst *i);
  void visitDestroyValueInst(DestroyValueInst *i);
  void visitAutoreleaseValueInst(AutoreleaseValueInst *i);
  void visitSetDeallocatingInst(SetDeallocatingInst *i);
  void visitObjectInst(ObjectInst *i)  {
    llvm_unreachable("object instruction cannot appear in a function");
  }
  void visitStructInst(StructInst *i);
  void visitTupleInst(TupleInst *i);
  void visitEnumInst(EnumInst *i);
  void visitInitEnumDataAddrInst(InitEnumDataAddrInst *i);
  void visitSelectEnumInst(SelectEnumInst *i);
  void visitSelectEnumAddrInst(SelectEnumAddrInst *i);
  void visitSelectValueInst(SelectValueInst *i);
  void visitUncheckedEnumDataInst(UncheckedEnumDataInst *i);
  void visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *i);
  void visitInjectEnumAddrInst(InjectEnumAddrInst *i);
  void visitObjCProtocolInst(ObjCProtocolInst *i);
  void visitMetatypeInst(MetatypeInst *i);
  void visitValueMetatypeInst(ValueMetatypeInst *i);
  void visitExistentialMetatypeInst(ExistentialMetatypeInst *i);
  void visitTupleExtractInst(TupleExtractInst *i);
  void visitDestructureTupleInst(DestructureTupleInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitDestructureStructInst(DestructureStructInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitTupleElementAddrInst(TupleElementAddrInst *i);
  void visitStructExtractInst(StructExtractInst *i);
  void visitStructElementAddrInst(StructElementAddrInst *i);
  void visitRefElementAddrInst(RefElementAddrInst *i);
  void visitRefTailAddrInst(RefTailAddrInst *i);

  void visitClassMethodInst(ClassMethodInst *i);
  void visitSuperMethodInst(SuperMethodInst *i);
  void visitObjCMethodInst(ObjCMethodInst *i);
  void visitObjCSuperMethodInst(ObjCSuperMethodInst *i);
  void visitWitnessMethodInst(WitnessMethodInst *i);

  void visitOpenExistentialAddrInst(OpenExistentialAddrInst *i);
  void visitOpenExistentialMetatypeInst(OpenExistentialMetatypeInst *i);
  void visitOpenExistentialRefInst(OpenExistentialRefInst *i);
  void visitOpenExistentialValueInst(OpenExistentialValueInst *i);
  void visitInitExistentialAddrInst(InitExistentialAddrInst *i);
  void visitInitExistentialValueInst(InitExistentialValueInst *i);
  void visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *i);
  void visitInitExistentialRefInst(InitExistentialRefInst *i);
  void visitDeinitExistentialAddrInst(DeinitExistentialAddrInst *i);
  void visitDeinitExistentialValueInst(DeinitExistentialValueInst *i);

  void visitAllocExistentialBoxInst(AllocExistentialBoxInst *i);
  void visitOpenExistentialBoxInst(OpenExistentialBoxInst *i);
  void visitOpenExistentialBoxValueInst(OpenExistentialBoxValueInst *i);
  void visitProjectExistentialBoxInst(ProjectExistentialBoxInst *i);
  void visitDeallocExistentialBoxInst(DeallocExistentialBoxInst *i);
  
  void visitPackLengthInst(PackLengthInst *i);
  void visitOpenPackElementInst(OpenPackElementInst *i);
  void visitDynamicPackIndexInst(DynamicPackIndexInst *i);
  void visitPackPackIndexInst(PackPackIndexInst *i);
  void visitScalarPackIndexInst(ScalarPackIndexInst *i);
  void visitPackElementGetInst(PackElementGetInst *i);
  void visitPackElementSetInst(PackElementSetInst *i);
  void visitTuplePackElementAddrInst(TuplePackElementAddrInst *i);

  void visitProjectBlockStorageInst(ProjectBlockStorageInst *i);
  void visitInitBlockStorageHeaderInst(InitBlockStorageHeaderInst *i);
  
  void visitFixLifetimeInst(FixLifetimeInst *i);
  void visitEndLifetimeInst(EndLifetimeInst *i) {
    llvm_unreachable("unimplemented");
  }
  void
  visitUncheckedOwnershipConversionInst(UncheckedOwnershipConversionInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitBeginBorrowInst(BeginBorrowInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitEndBorrowInst(EndBorrowInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitStoreBorrowInst(StoreBorrowInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitBeginAccessInst(BeginAccessInst *i);
  void visitEndAccessInst(EndAccessInst *i);
  void visitBeginUnpairedAccessInst(BeginUnpairedAccessInst *i);
  void visitEndUnpairedAccessInst(EndUnpairedAccessInst *i);
  void visitUnmanagedRetainValueInst(UnmanagedRetainValueInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitUnmanagedReleaseValueInst(UnmanagedReleaseValueInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitUnmanagedAutoreleaseValueInst(UnmanagedAutoreleaseValueInst *i) {
    llvm_unreachable("unimplemented");
  }
  void visitMarkDependenceInst(MarkDependenceInst *i);
  void visitCopyBlockInst(CopyBlockInst *i);
  void visitCopyBlockWithoutEscapingInst(CopyBlockWithoutEscapingInst *i) {
    llvm_unreachable("not valid in canonical SIL");
  }
  void visitStrongRetainInst(StrongRetainInst *i);
  void visitStrongReleaseInst(StrongReleaseInst *i);
  void visitIsUniqueInst(IsUniqueInst *i);
  void visitBeginCOWMutationInst(BeginCOWMutationInst *i);
  void visitEndCOWMutationInst(EndCOWMutationInst *i);
  void visitIsEscapingClosureInst(IsEscapingClosureInst *i);
  void visitDeallocStackInst(DeallocStackInst *i);
  void visitDeallocStackRefInst(DeallocStackRefInst *i);
  void visitDeallocPackInst(DeallocPackInst *i);
  void visitDeallocBoxInst(DeallocBoxInst *i);
  void visitDeallocRefInst(DeallocRefInst *i);
  void visitDeallocPartialRefInst(DeallocPartialRefInst *i);

  void visitCopyAddrInst(CopyAddrInst *i);
  void visitExplicitCopyAddrInst(ExplicitCopyAddrInst *i);
  void visitMarkUnresolvedMoveAddrInst(MarkUnresolvedMoveAddrInst *mai) {
    llvm_unreachable("Valid only when ownership is enabled");
  }
  void visitDestroyAddrInst(DestroyAddrInst *i);

  void visitBindMemoryInst(BindMemoryInst *i);
  void visitRebindMemoryInst(RebindMemoryInst *i);

  void visitCondFailInst(CondFailInst *i);

  void visitIncrementProfilerCounterInst(IncrementProfilerCounterInst *I);

  void visitConvertFunctionInst(ConvertFunctionInst *i);
  void visitConvertEscapeToNoEscapeInst(ConvertEscapeToNoEscapeInst *i);
  void visitUpcastInst(UpcastInst *i);
  void visitAddressToPointerInst(AddressToPointerInst *i);
  void visitPointerToAddressInst(PointerToAddressInst *i);
  void visitUncheckedRefCastInst(UncheckedRefCastInst *i);
  void visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *i);
  void visitUncheckedAddrCastInst(UncheckedAddrCastInst *i);
  void visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *i);
  void visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *i);
  void visitUncheckedValueCastInst(UncheckedValueCastInst *i) {
    llvm_unreachable("Should never be seen in Lowered SIL");
  }
  void visitRefToRawPointerInst(RefToRawPointerInst *i);
  void visitRawPointerToRefInst(RawPointerToRefInst *i);
  void visitThinToThickFunctionInst(ThinToThickFunctionInst *i);
  void visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *i);
  void visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *i);
  void visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *i);
  void visitUnconditionalCheckedCastAddrInst(UnconditionalCheckedCastAddrInst *i);
  void visitObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *i);
  void visitObjCExistentialMetatypeToObjectInst(
                                        ObjCExistentialMetatypeToObjectInst *i);
  void visitRefToBridgeObjectInst(RefToBridgeObjectInst *i);
  void visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *i);
  void visitBridgeObjectToRefInst(BridgeObjectToRefInst *i);
  void visitBridgeObjectToWordInst(BridgeObjectToWordInst *i);
  void visitValueToBridgeObjectInst(ValueToBridgeObjectInst *i);

  void visitIndexAddrInst(IndexAddrInst *i);
  void visitTailAddrInst(TailAddrInst *i);
  void visitIndexRawPointerInst(IndexRawPointerInst *i);

  void visitBeginApplyInst(BeginApplyInst *i);
  void visitEndApplyInst(EndApplyInst *i);
  void visitAbortApplyInst(AbortApplyInst *i);
  void visitEndApply(BeginApplyInst *i, bool isAbort);
  
  void visitUnreachableInst(UnreachableInst *i);
  void visitBranchInst(BranchInst *i);
  void visitCondBranchInst(CondBranchInst *i);
  void visitReturnInst(ReturnInst *i);
  void visitThrowInst(ThrowInst *i);
  void visitUnwindInst(UnwindInst *i);
  void visitYieldInst(YieldInst *i);
  void visitSwitchValueInst(SwitchValueInst *i);
  void visitSwitchEnumInst(SwitchEnumInst *i);
  void visitSwitchEnumAddrInst(SwitchEnumAddrInst *i);
  void visitDynamicMethodBranchInst(DynamicMethodBranchInst *i);
  void visitCheckedCastBranchInst(CheckedCastBranchInst *i);
  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *i);
  
  void visitGetAsyncContinuationInst(GetAsyncContinuationInst *i);
  void visitGetAsyncContinuationAddrInst(GetAsyncContinuationAddrInst *i);
  void visitAwaitAsyncContinuationInst(AwaitAsyncContinuationInst *i);

  void visitHopToExecutorInst(HopToExecutorInst *i);
  void visitExtractExecutorInst(ExtractExecutorInst *i) {
    llvm_unreachable("extract_executor should never be seen in Lowered SIL");
  }

  void visitKeyPathInst(KeyPathInst *I);

  void visitDifferentiableFunctionInst(DifferentiableFunctionInst *i);
  void visitLinearFunctionInst(LinearFunctionInst *i);
  void
  visitDifferentiableFunctionExtractInst(DifferentiableFunctionExtractInst *i);
  void visitLinearFunctionExtractInst(LinearFunctionExtractInst *i);
  void visitDifferentiabilityWitnessFunctionInst(
      DifferentiabilityWitnessFunctionInst *i);
  void visitTestSpecificationInst(TestSpecificationInst *i) {
    llvm_unreachable("test-only instruction in Lowered SIL?!");
  }

  void visitHasSymbolInst(HasSymbolInst *i);

#define LOADABLE_REF_STORAGE_HELPER(Name)                                      \
  void visitRefTo##Name##Inst(RefTo##Name##Inst *i);                           \
  void visit##Name##ToRefInst(Name##ToRefInst *i);                             \
  void visitStrongCopy##Name##ValueInst(StrongCopy##Name##ValueInst *i);
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  void visitLoad##Name##Inst(Load##Name##Inst *i); \
  void visitStore##Name##Inst(Store##Name##Inst *i);
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  LOADABLE_REF_STORAGE_HELPER(Name)                                            \
  void visitStrongRetain##Name##Inst(StrongRetain##Name##Inst *i);             \
  void visit##Name##RetainInst(Name##RetainInst *i);                           \
  void visit##Name##ReleaseInst(Name##ReleaseInst *i);
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...) \
  LOADABLE_REF_STORAGE_HELPER(Name)
#include "swift/AST/ReferenceStorage.def"
#undef LOADABLE_REF_STORAGE_HELPER
  
};

} // end anonymous namespace

static AsyncContextLayout getAsyncContextLayout(IRGenSILFunction &IGF) {
  return getAsyncContextLayout(IGF.IGM, IGF.CurSILFn);
}

namespace {
class SyncEntryPointArgumentEmission
    : public virtual EntryPointArgumentEmission {
protected:
  IRGenSILFunction &IGF;
  SILBasicBlock &entry;
  Explosion &allParamValues;
  SyncEntryPointArgumentEmission(IRGenSILFunction &IGF, SILBasicBlock &entry,
                                 Explosion &allParamValues)
      : IGF(IGF), entry(entry), allParamValues(allParamValues){};

public:
  bool requiresIndirectResult(SILType retType) override {
    auto &schema =
        IGF.IGM.getTypeInfo(retType).nativeReturnValueSchema(IGF.IGM);
    return schema.requiresIndirect();
  }
  llvm::Value *getIndirectResultForFormallyDirectResult() override {
    return allParamValues.claimNext();
  }
  llvm::Value *getIndirectResult(unsigned index) override {
    return allParamValues.claimNext();
  };
  llvm::Value *
  getNextPolymorphicParameter(GenericRequirement &requirement) override {
    return allParamValues.claimNext();
  }
  llvm::Value *getNextPolymorphicParameterAsMetadata() override {
    return allParamValues.claimNext();
  }
};
class AsyncEntryPointArgumentEmission
    : public virtual EntryPointArgumentEmission {
protected:
  IRGenSILFunction &IGF;
  SILBasicBlock &entry;
  Explosion &allParamValues;
  AsyncEntryPointArgumentEmission(IRGenSILFunction &IGF, SILBasicBlock &entry,
                                  Explosion &allParamValues)
      : IGF(IGF), entry(entry), allParamValues(allParamValues){};
};

class COrObjCEntryPointArgumentEmission
    : public virtual EntryPointArgumentEmission {};

class SyncCOrObjCEntryPointArgumentEmission
    : public SyncEntryPointArgumentEmission,
      public COrObjCEntryPointArgumentEmission {
public:
  SyncCOrObjCEntryPointArgumentEmission(IRGenSILFunction &_IGF,
                                        SILBasicBlock &_entry,
                                        Explosion &_allParamValues)
      : SyncEntryPointArgumentEmission(_IGF, _entry, _allParamValues){};
};

class SyncNativeCCEntryPointArgumentEmission final
    : public NativeCCEntryPointArgumentEmission,
      public SyncEntryPointArgumentEmission {
public:
  SyncNativeCCEntryPointArgumentEmission(IRGenSILFunction &_IGF,
                                         SILBasicBlock &_entry,
                                         Explosion &_allParamValues)
      : SyncEntryPointArgumentEmission(_IGF, _entry, _allParamValues){};

  llvm::Value *getCallerErrorResultArgument() override {
    return allParamValues.takeLast();
  }
  void mapAsyncParameters() override{/* nothing to map*/};
  llvm::Value *getContext() override { return allParamValues.takeLast(); }
  Explosion getArgumentExplosion(unsigned index, unsigned size) override {
    assert(size > 0);
    Explosion result;
    allParamValues.transferInto(result, size);
    return result;
  }
  llvm::Value *getSelfWitnessTable() override {
    return allParamValues.takeLast();
  }
  llvm::Value *getSelfMetadata() override { return allParamValues.takeLast(); }
  llvm::Value *getCoroutineBuffer() override {
    return allParamValues.claimNext();
  }
  Explosion
  explosionForObject(IRGenFunction &IGF, unsigned index, SILArgument *param,
                     SILType paramTy, const LoadableTypeInfo &loadableParamTI,
                     const LoadableTypeInfo &loadableArgTI,
                     std::function<Explosion(unsigned index, unsigned size)>
                         explosionForArgument) override {
    Explosion paramValues;
    // If the explosion must be passed indirectly, load the value from the
    // indirect address.
    auto &nativeSchema = loadableArgTI.nativeParameterValueSchema(IGF.IGM);
    if (nativeSchema.requiresIndirect()) {
      Explosion paramExplosion = explosionForArgument(index, 1);
      Address paramAddr =
          loadableParamTI.getAddressForPointer(paramExplosion.claimNext());
      if (loadableParamTI.getStorageType() != loadableArgTI.getStorageType())
        paramAddr =
            loadableArgTI.getAddressForPointer(IGF.Builder.CreateBitCast(
                paramAddr.getAddress(),
                loadableArgTI.getStorageType()->getPointerTo()));
      loadableArgTI.loadAsTake(IGF, paramAddr, paramValues);
    } else {
      if (!nativeSchema.empty()) {
        // Otherwise, we map from the native convention to the type's explosion
        // schema.
        Explosion nativeParam;
        unsigned size = nativeSchema.size();
        Explosion paramExplosion = explosionForArgument(index, size);
        paramExplosion.transferInto(nativeParam, size);
        paramValues = nativeSchema.mapFromNative(IGF.IGM, IGF, nativeParam,
                                                 param->getType());
      } else {
        assert(loadableParamTI.getSchema().empty());
      }
    }
    return paramValues;
  };

public:
  using SyncEntryPointArgumentEmission::requiresIndirectResult;
  using SyncEntryPointArgumentEmission::getIndirectResultForFormallyDirectResult;
  using SyncEntryPointArgumentEmission::getIndirectResult;
  using SyncEntryPointArgumentEmission::getNextPolymorphicParameterAsMetadata;
  using SyncEntryPointArgumentEmission::getNextPolymorphicParameter;
};

class AsyncNativeCCEntryPointArgumentEmission final
    : public NativeCCEntryPointArgumentEmission,
      public AsyncEntryPointArgumentEmission {
  llvm::Value *context = nullptr;
  /*const*/ AsyncContextLayout layout;
  Address dataAddr;

  Explosion loadExplosion(ElementLayout layout) {
    Address addr = layout.project(IGF, dataAddr, /*offsets*/ llvm::None);
    auto &ti = cast<LoadableTypeInfo>(layout.getType());
    Explosion explosion;
    ti.loadAsTake(IGF, addr, explosion);
    return explosion;
  }
  llvm::Value *loadValue(ElementLayout layout) {
    auto explosion = loadExplosion(layout);
    return explosion.claimNext();
  }

public:
  AsyncNativeCCEntryPointArgumentEmission(IRGenSILFunction &IGF,
                                          SILBasicBlock &entry,
                                          Explosion &allParamValues)
      : AsyncEntryPointArgumentEmission(IGF, entry, allParamValues),
        layout(getAsyncContextLayout(IGF)){};

  void mapAsyncParameters() override {
    context = allParamValues.claimNext();
    dataAddr = layout.emitCastTo(IGF, context);
  };

  llvm::Value *getCallerErrorResultArgument() override {
    llvm_unreachable("should not be used");
  }
  llvm::Value *getContext() override { return allParamValues.takeLast(); }
  Explosion getArgumentExplosion(unsigned index, unsigned size) override {
    assert(size > 0);
    Explosion result;
    allParamValues.transferInto(result, size);
    return result;
  }
  bool requiresIndirectResult(SILType retType) override {
    auto &schema =
        IGF.IGM.getTypeInfo(retType).nativeReturnValueSchema(IGF.IGM);
    return schema.requiresIndirect();
  }
  llvm::Value *getIndirectResultForFormallyDirectResult() override {
    return allParamValues.claimNext();
  }
  llvm::Value *
  getNextPolymorphicParameter(GenericRequirement &requirement) override {
    return allParamValues.claimNext();
  }
  llvm::Value *getNextPolymorphicParameterAsMetadata() override {
    return allParamValues.claimNext();
  }
  llvm::Value *getIndirectResult(unsigned index) override {
    return allParamValues.claimNext();
  };
  llvm::Value *getSelfWitnessTable() override {
    return allParamValues.takeLast();
  }
  llvm::Value *getSelfMetadata() override { return allParamValues.takeLast(); }
  llvm::Value *getCoroutineBuffer() override {
    llvm_unreachable(
        "async functions do not use a fixed size coroutine buffer");
  }
  Explosion
  explosionForObject(IRGenFunction &IGF, unsigned index, SILArgument *param,
                     SILType paramTy, const LoadableTypeInfo &loadableParamTI,
                     const LoadableTypeInfo &loadableArgTI,
                     std::function<Explosion(unsigned index, unsigned size)>
                         explosionForArgument) override {
    Explosion paramValues;
    // If the explosion must be passed indirectly, load the value from the
    // indirect address.
    auto &nativeSchema = loadableArgTI.nativeParameterValueSchema(IGF.IGM);
    if (nativeSchema.requiresIndirect()) {
      Explosion paramExplosion = explosionForArgument(index, 1);
      Address paramAddr =
          loadableParamTI.getAddressForPointer(paramExplosion.claimNext());
      if (loadableParamTI.getStorageType() != loadableArgTI.getStorageType())
        paramAddr =
            loadableArgTI.getAddressForPointer(IGF.Builder.CreateBitCast(
                paramAddr.getAddress(),
                loadableArgTI.getStorageType()->getPointerTo()));
      loadableArgTI.loadAsTake(IGF, paramAddr, paramValues);
    } else {
      if (!nativeSchema.empty()) {
        // Otherwise, we map from the native convention to the type's explosion
        // schema.
        Explosion nativeParam;
        unsigned size = nativeSchema.size();
        Explosion paramExplosion = explosionForArgument(index, size);
        paramExplosion.transferInto(nativeParam, size);
        paramValues = nativeSchema.mapFromNative(IGF.IGM, IGF, nativeParam,
                                                 param->getType());
      } else {
        assert(loadableParamTI.getSchema().empty());
      }
    }
    return paramValues;
  };
};

std::unique_ptr<NativeCCEntryPointArgumentEmission>
getNativeCCEntryPointArgumentEmission(IRGenSILFunction &IGF,
                                      SILBasicBlock &entry,
                                      Explosion &allParamValues) {
  if (IGF.CurSILFn->isAsync()) {
    return std::make_unique<AsyncNativeCCEntryPointArgumentEmission>(
        IGF, entry, allParamValues);
  } else {
    return std::make_unique<SyncNativeCCEntryPointArgumentEmission>(
        IGF, entry, allParamValues);
  }
}
std::unique_ptr<COrObjCEntryPointArgumentEmission>
getCOrObjCEntryPointArgumentEmission(IRGenSILFunction &IGF,
                                     SILBasicBlock &entry,
                                     Explosion &allParamValues) {
  if (IGF.CurSILFn->isAsync()) {
    llvm_unreachable("unsupported");
  } else {
    return std::make_unique<SyncCOrObjCEntryPointArgumentEmission>(
        IGF, entry, allParamValues);
  }
}
} // end anonymous namespace

void LoweredValue::getExplosion(IRGenFunction &IGF, SILType type,
                                Explosion &ex) const {
  switch (kind) {
  case Kind::StackAddress:
    ex.add(Storage.get<StackAddress>(kind).getAddressPointer());
    return;

  case Kind::ContainedAddress:
  case Kind::DynamicallyEnforcedAddress:
  case Kind::CoroutineState:
    llvm_unreachable("not a value");

  case Kind::ExplosionVector:
    ex.add(Storage.get<ExplosionVector>(kind));
    return;

  case Kind::SingletonExplosion:
    ex.add(Storage.get<SingletonExplosion>(kind));
    return;

  case Kind::EmptyExplosion:
    return;

  case Kind::OwnedAddress:
    ex.add(Storage.get<OwnedAddress>(kind).getOwner());
    return;

  case Kind::FunctionPointer:
    ex.add(Storage.get<FunctionPointer>(kind)
                  .getExplosionValue(IGF, type.castTo<SILFunctionType>()));
    return;

  case Kind::ObjCMethod:
    ex.add(Storage.get<ObjCMethod>(kind).getExplosionValue(IGF));
    return;
  }
  llvm_unreachable("bad kind");
}

llvm::Value *LoweredValue::getSingletonExplosion(IRGenFunction &IGF,
                                                 SILType type) const {
  switch (kind) {
  case Kind::StackAddress:
  case Kind::ContainedAddress:
  case Kind::DynamicallyEnforcedAddress:
  case Kind::CoroutineState:
    llvm_unreachable("not a value");

  case Kind::EmptyExplosion:
  case Kind::ExplosionVector:
    llvm_unreachable("not a singleton explosion");

  case Kind::SingletonExplosion:
    return Storage.get<SingletonExplosion>(kind);

  case Kind::OwnedAddress:
    return Storage.get<OwnedAddress>(kind).getOwner();
      
  case Kind::FunctionPointer:
    return Storage.get<FunctionPointer>(kind)
                  .getExplosionValue(IGF, type.castTo<SILFunctionType>());

  case Kind::ObjCMethod:
    return Storage.get<ObjCMethod>(kind).getExplosionValue(IGF);
  }
  llvm_unreachable("bad kind");
}

IRGenSILFunction::IRGenSILFunction(IRGenModule &IGM, SILFunction *f)
    : IRGenFunction(IGM,
                    IGM.getAddrOfSILFunction(f, ForDefinition,
                                             f->isDynamicallyReplaceable()),
                    f->getOptimizationMode(), f->getDebugScope(),
                    f->getLocation()),
      CurSILFn(f) {
  // Apply sanitizer attributes to the function.
  // TODO: Check if the function is supposed to be excluded from ASan either by
  // being in the external file or via annotations.
  if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Address)
    CurFn->addFnAttr(llvm::Attribute::SanitizeAddress);
  if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread) {
    auto declContext = f->getDeclContext();
    if (isa_and_nonnull<DestructorDecl>(declContext)) {
      // Do not report races in deinit and anything called from it
      // because TSan does not observe synchronization between retain
      // count dropping to '0' and the object deinitialization.
      CurFn->addFnAttr("sanitize_thread_no_checking_at_run_time");
    } else {
      CurFn->addFnAttr(llvm::Attribute::SanitizeThread);
    }
  }

  // Disable inlining of coroutine functions until we split.
  if (f->getLoweredFunctionType()->isCoroutine()) {
    CurFn->addFnAttr(llvm::Attribute::NoInline);
  }
  // Emit the thunk that calls the previous implementation if this is a dynamic
  // replacement.
  if (f->getDynamicallyReplacedFunction()) {
    IGM.emitDynamicReplacementOriginalFunctionThunk(f);
  }

  if (f->isDynamicallyReplaceable() && !f->isAsync()) {
    IGM.createReplaceableProlog(*this, f);
  }
}

IRGenSILFunction::~IRGenSILFunction() {
  assert(Builder.hasPostTerminatorIP() && "did not terminate BB?!");
  // Emit the fail BB if we have one.
  if (!FailBBs.empty())
    emitFailBB();
  LLVM_DEBUG(CurFn->print(llvm::dbgs()));
}

template<typename ValueVector>
static void emitPHINodesForType(IRGenSILFunction &IGF, SILType type,
                                const TypeInfo &ti, unsigned predecessors,
                                ValueVector &phis) {
  if (type.isAddress()) {
    phis.push_back(IGF.Builder.CreatePHI(ti.getStorageType()->getPointerTo(),
                                         predecessors));
  } else {
    // PHIs are always emitted with maximal explosion.
    ExplosionSchema schema = ti.getSchema();
    for (auto &elt : schema) {
      if (elt.isScalar())
        phis.push_back(
                   IGF.Builder.CreatePHI(elt.getScalarType(), predecessors));
      else
        phis.push_back(
                   IGF.Builder.CreatePHI(elt.getAggregateType()->getPointerTo(),
                   predecessors));
    }
  }
}

static PHINodeVector
emitPHINodesForBBArgs(IRGenSILFunction &IGF,
                      SILBasicBlock *silBB,
                      llvm::BasicBlock *llBB) {
  PHINodeVector phis;
  unsigned predecessors = std::distance(silBB->pred_begin(), silBB->pred_end());
  
  IGF.Builder.SetInsertPoint(llBB);
  if (IGF.IGM.DebugInfo) {
    // Use the location of the first instruction in the basic block
    // for the φ-nodes.
    if (!silBB->empty()) {
      SILInstruction &I = *silBB->begin();
      auto DS = I.getDebugScope();
      assert(DS);
      IGF.IGM.DebugInfo->setCurrentLoc(IGF.Builder, DS, I.getLoc());
    }
  }

  for (SILArgument *arg : make_range(silBB->args_begin(), silBB->args_end())) {
    size_t first = phis.size();
    
    const TypeInfo &ti = IGF.getTypeInfo(arg->getType());
    
    emitPHINodesForType(IGF, arg->getType(), ti, predecessors, phis);
    if (arg->getType().isAddress()) {
      IGF.setLoweredAddress(arg,
                            ti.getAddressForPointer(phis.back()));
    } else {
      Explosion argValue;
      for (llvm::PHINode *phi :
               swift::make_range(phis.begin()+first, phis.end()))
        argValue.add(phi);
      IGF.setLoweredExplosion(arg, argValue);
    }
  }

  // Since we return to the entry of the function, reset the location.
  if (IGF.IGM.DebugInfo)
    IGF.IGM.DebugInfo->clearLoc(IGF.Builder);

  return phis;
}

static void addIncomingExplosionToPHINodes(IRGenSILFunction &IGF,
                                           LoweredBB &lbb,
                                           unsigned &phiIndex,
                                           Explosion &argValue);

// TODO: Handle this during SIL AddressLowering.
static ArrayRef<SILArgument *> emitEntryPointIndirectReturn(
    EntryPointArgumentEmission &emission, IRGenSILFunction &IGF,
    SILBasicBlock *entry, CanSILFunctionType funcTy,
    llvm::function_ref<bool(SILType)> requiresIndirectResult) {
  // Map an indirect return for a type SIL considers loadable but still
  // requires an indirect return at the IR level.
  SILFunctionConventions fnConv(funcTy, IGF.getSILModule());
  SILType directResultType = IGF.CurSILFn->mapTypeIntoContext(
      fnConv.getSILResultType(IGF.IGM.getMaximalTypeExpansionContext()));
  if (requiresIndirectResult(directResultType)) {
    auto &paramTI = IGF.IGM.getTypeInfo(directResultType);
    auto &retTI =
        IGF.IGM.getTypeInfo(IGF.getLoweredTypeInContext(directResultType));
    auto ptr = emission.getIndirectResultForFormallyDirectResult();
    if (paramTI.getStorageType() != retTI.getStorageType()) {
      assert(directResultType.getASTType()->hasOpaqueArchetype());
      ptr = IGF.Builder.CreateBitCast(ptr,
                                      retTI.getStorageType()->getPointerTo());
    }
    IGF.IndirectReturn = retTI.getAddressForPointer(ptr);
  }

  auto bbargs = entry->getArguments();

  // Map the indirect returns if present.
  unsigned numIndirectResults = fnConv.getNumIndirectSILResults();
  unsigned idx = 0;
  for (auto indirectResultType : fnConv.getIndirectSILResultTypes(
           IGF.IGM.getMaximalTypeExpansionContext())) {
    SILArgument *ret = bbargs[idx];
    auto inContextResultType =
        IGF.CurSILFn->mapTypeIntoContext(indirectResultType);
    auto &retTI = IGF.IGM.getTypeInfo(ret->getType());
    auto &paramTI = IGF.IGM.getTypeInfo(inContextResultType);

    // The parameter's type might be different due to looking through opaque
    // archetypes or for non-fixed types (llvm likes to do type based analysis
    // for sret arguments and so we use opaque storage types for them).
    llvm::Value *ptr = emission.getIndirectResult(idx);
    bool isFixedSize = isa<FixedTypeInfo>(paramTI);
    if (paramTI.getStorageType() != retTI.getStorageType() || !isFixedSize) {
      assert(!isFixedSize ||
             inContextResultType.getASTType()->hasOpaqueArchetype());
      ptr = IGF.Builder.CreateBitCast(ptr,
                                      retTI.getStorageType()->getPointerTo());
    }
    auto addr = retTI.getAddressForPointer(ptr);
    IGF.setLoweredAddress(ret, addr);
    ++idx;
  }
  assert(numIndirectResults == idx);

  return bbargs.slice(numIndirectResults);
}

template <typename ExplosionForArgument>
static void bindParameter(IRGenSILFunction &IGF,
                          NativeCCEntryPointArgumentEmission &emission,
                          unsigned index, SILArgument *param, SILType paramTy,
                          ExplosionForArgument explosionForArgument) {
  // Pull out the parameter value and its formal type.
  auto &paramTI = IGF.getTypeInfo(IGF.CurSILFn->mapTypeIntoContext(paramTy));
  auto &argTI = IGF.getTypeInfo(param->getType());

  // If the SIL parameter isn't passed indirectly, we need to map it
  // to an explosion.
  if (param->getType().isObject()) {
    auto &loadableParamTI = cast<LoadableTypeInfo>(paramTI);
    auto &loadableArgTI = cast<LoadableTypeInfo>(argTI);
    auto paramValues =
        emission.explosionForObject(IGF, index, param, paramTy, loadableParamTI,
                                    loadableArgTI, explosionForArgument);
    IGF.setLoweredExplosion(param, paramValues);
    return;
  }

  // Okay, the type is passed indirectly in SIL, so we need to map
  // it to an address.
  // FIXME: that doesn't mean we should physically pass it
  // indirectly at this resilience expansion. An @in or @in_guaranteed parameter
  // could be passed by value in the right resilience domain.
  Explosion paramExplosion = explosionForArgument(index, 1);
  auto ptr = paramExplosion.claimNext();
  if (paramTI.getStorageType() != argTI.getStorageType()) {
    ptr =
        IGF.Builder.CreateBitCast(ptr, argTI.getStorageType()->getPointerTo());
  }
  Address paramAddr = argTI.getAddressForPointer(ptr);
  IGF.setLoweredAddress(param, paramAddr);
}

/// Emit entry point arguments for a SILFunction with the Swift calling
/// convention.
static void emitEntryPointArgumentsNativeCC(IRGenSILFunction &IGF,
                                            SILBasicBlock *entry,
                                            Explosion &allParamValues) {
  auto emission =
      getNativeCCEntryPointArgumentEmission(IGF, *entry, allParamValues);
  auto funcTy = IGF.CurSILFn->getLoweredFunctionType();

  // Map the indirect return if present.
  ArrayRef<SILArgument *> params = emitEntryPointIndirectReturn(
      *emission, IGF, entry, funcTy, [&](SILType retType) -> bool {
        return emission->requiresIndirectResult(retType);
      });

  // Map the async context parameters if present.
  emission->mapAsyncParameters();

  // The witness method CC passes Self as a final argument.
  WitnessMetadata witnessMetadata;
  if (funcTy->getRepresentation() == SILFunctionTypeRepresentation::WitnessMethod) {
    collectTrailingWitnessMetadata(IGF, *IGF.CurSILFn, *emission,
                                   witnessMetadata);
  }

  // The coroutine context should be the first parameter.
  switch (funcTy->getCoroutineKind()) {
  case SILCoroutineKind::None:
    break;
  case SILCoroutineKind::YieldOnce:
    emitYieldOnceCoroutineEntry(IGF, funcTy, *emission);
    break;
  case SILCoroutineKind::YieldMany:
    emitYieldManyCoroutineEntry(IGF, funcTy, *emission);
    break;
  }

  if (funcTy->isAsync()) {
    emitAsyncFunctionEntry(IGF, getAsyncContextLayout(IGF.IGM, IGF.CurSILFn),
                           LinkEntity::forSILFunction(IGF.CurSILFn),
                           Signature::forAsyncEntry(
                               IGF.IGM, funcTy,
                               FunctionPointerKind::defaultAsync())
                               .getAsyncContextIndex());
    if (IGF.CurSILFn->isDynamicallyReplaceable()) {
      IGF.IGM.createReplaceableProlog(IGF, IGF.CurSILFn);
      // Remap the entry block.
      IGF.LoweredBBs[&*IGF.CurSILFn->begin()] = LoweredBB(IGF.Builder.GetInsertBlock(), {});
    }
  }

  // Bind the error result by popping it off the parameter list.
  if (funcTy->hasErrorResult() && !funcTy->isAsync()) {
    SILFunctionConventions fnConv(funcTy, IGF.getSILModule());
    auto errorType =
        fnConv.getSILErrorType(IGF.IGM.getMaximalTypeExpansionContext());
    auto &errorTI = cast<FixedTypeInfo>(IGF.getTypeInfo(errorType));
    IGF.setCallerErrorResultSlot(
        Address(emission->getCallerErrorResultArgument(),
                errorTI.getStorageType(), IGF.IGM.getPointerAlignment()));
  }

  SILFunctionConventions conv(funcTy, IGF.getSILModule());

  // The 'self' argument might be in the context position, which is
  // now the end of the parameter list.  Bind it now.
  if (hasSelfContextParameter(funcTy)) {
    SILArgument *selfParam = params.back();
    params = params.drop_back();

    bindParameter(
        IGF, *emission, 0, selfParam,
        conv.getSILArgumentType(conv.getNumSILArguments() - 1,
                                IGF.IGM.getMaximalTypeExpansionContext()),
        [&](unsigned startIndex, unsigned size) {
          assert(size == 1);
          Explosion selfTemp;
          selfTemp.add(emission->getContext());
          return selfTemp;
        });

    // Even if we don't have a 'self', if we have an error result, we
    // should have a placeholder argument here.
    //
    // For async functions, there will be a thick context within the async
    // context whenever there is no self context.
  } else if ((!funcTy->isAsync() && funcTy->hasErrorResult()) ||
             funcTy->getRepresentation() ==
                 SILFunctionTypeRepresentation::Thick) {
    llvm::Value *contextPtr = emission->getContext();
    (void)contextPtr;
    assert(contextPtr->getType() == IGF.IGM.RefCountedPtrTy);
  }

  // Map the remaining SIL parameters to LLVM parameters.
  unsigned i = 0;
  for (SILArgument *param : params) {
    auto argIdx = conv.getSILArgIndexOfFirstParam() + i;
    bindParameter(IGF, *emission, i, param,
                  conv.getSILArgumentType(
                      argIdx, IGF.IGM.getMaximalTypeExpansionContext()),
                  [&](unsigned index, unsigned size) {
                    return emission->getArgumentExplosion(index, size);
                  });
    ++i;
  }

  // Bind polymorphic arguments.  This can only be done after binding
  // all the value parameters.
  if (hasPolymorphicParameters(funcTy)) {
    emitPolymorphicParameters(
        IGF, *IGF.CurSILFn, *emission, &witnessMetadata,
        [&](unsigned paramIndex) -> llvm::Value * {
          SILValue parameter =
              IGF.CurSILFn->getArgumentsWithoutIndirectResults()[paramIndex];
          return IGF.getLoweredSingletonExplosion(parameter);
        });
  }

  assert(allParamValues.empty() && "didn't claim all parameters!");
}

/// Emit entry point arguments for the parameters of a C function, or the
/// method parameters of an ObjC method.
static void emitEntryPointArgumentsCOrObjC(IRGenSILFunction &IGF,
                                           SILBasicBlock *entry,
                                           Explosion &params,
                                           CanSILFunctionType funcTy) {
  auto emission = getCOrObjCEntryPointArgumentEmission(IGF, *entry, params);
  // First, lower the method type.
  ForeignFunctionInfo foreignInfo = IGF.IGM.getForeignFunctionInfo(funcTy);
  assert(foreignInfo.ClangInfo);
  auto &FI = *foreignInfo.ClangInfo;

  // Okay, start processing the parameters explosion.

  // First, claim all the indirect results.
  ArrayRef<SILArgument *> args = emitEntryPointIndirectReturn(
      *emission, IGF, entry, funcTy, [&](SILType directResultType) -> bool {
        return FI.getReturnInfo().isIndirect();
      });

  unsigned nextArgTyIdx = 0;

  // Handle the arguments of an ObjC method.
  if (IGF.CurSILFn->getRepresentation() ==
        SILFunctionTypeRepresentation::ObjCMethod) {
    // Claim the self argument from the end of the formal arguments.
    SILArgument *selfArg = args.back();
    args = args.slice(0, args.size() - 1);

    // Set the lowered explosion for the self argument.
    auto &selfTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(selfArg->getType()));
    auto selfSchema = selfTI.getSchema();
    assert(selfSchema.size() == 1 && "Expected self to be a single element!");

    auto *selfValue = params.claimNext();
    auto *bodyType = selfSchema.begin()->getScalarType();
    if (selfValue->getType() != bodyType)
      selfValue = IGF.coerceValue(selfValue, bodyType, IGF.IGM.DataLayout);

    Explosion self;
    self.add(selfValue);
    IGF.setLoweredExplosion(selfArg, self);

    // Discard the implicit _cmd argument.
    params.claimNext();

    // We've handled the self and _cmd arguments, so when we deal with
    // generating explosions for the remaining arguments we can skip
    // these.
    nextArgTyIdx = 2;
  }

  assert(args.size() == (FI.arg_size() - nextArgTyIdx) &&
         "Number of arguments not equal to number of argument types!");

  // Generate lowered explosions for each explicit argument.
  for (auto i : indices(args)) {
    SILArgument *arg = args[i];
    auto argTyIdx = i + nextArgTyIdx;
    auto &argTI = IGF.getTypeInfo(arg->getType());
    
    // Bitcast indirect argument pointers to the right storage type.
    if (arg->getType().isAddress()) {
      llvm::Value *ptr = params.claimNext();
      ptr = IGF.Builder.CreateBitCast(ptr,
                                      argTI.getStorageType()->getPointerTo());
      IGF.setLoweredAddress(arg, Address(ptr, argTI.getStorageType(),
                                         argTI.getBestKnownAlignment()));
      continue;
    }
    
    auto &loadableArgTI = cast<LoadableTypeInfo>(argTI);

    Explosion argExplosion;
    emitForeignParameter(IGF, params, foreignInfo, argTyIdx, arg->getType(),
                         loadableArgTI, argExplosion, false);
    IGF.setLoweredExplosion(arg, argExplosion);
  }

  assert(params.empty() && "didn't claim all parameters!");

  // emitPolymorphicParameters() may create function calls, so we need
  // to initialize the debug location here.
  ArtificialLocation Loc(IGF.getDebugScope(), IGF.IGM.DebugInfo.get(),
                         IGF.Builder);
  
  // Bind polymorphic arguments. This can only be done after binding
  // all the value parameters, and must be done even for non-polymorphic
  // functions because of imported Objective-C generics.
  emitPolymorphicParameters(
      IGF, *IGF.CurSILFn, *emission, nullptr,
      [&](unsigned paramIndex) -> llvm::Value * {
        SILValue parameter = entry->getArguments()[paramIndex];
        return IGF.getLoweredSingletonExplosion(parameter);
      });
}

/// Get metadata for the dynamic Self type if we have it.
static void emitDynamicSelfMetadata(IRGenSILFunction &IGF) {
  if (!IGF.CurSILFn->hasDynamicSelfMetadata())
    return;
  
  const SILArgument *selfArg = IGF.CurSILFn->getDynamicSelfMetadata();
  auto selfTy = selfArg->getType().getASTType();
  CanMetatypeType metaTy =
    dyn_cast<MetatypeType>(selfTy);
  IRGenFunction::DynamicSelfKind selfKind;
  if (!metaTy)
    selfKind = IRGenFunction::ObjectReference;
  else {
    selfTy = metaTy.getInstanceType();
    switch (metaTy->getRepresentation()) {
    case MetatypeRepresentation::Thin:
      assert(selfTy.isForeignReferenceType() &&
             "Only foreign reference metatypes are allowed to be thin");
      selfKind = IRGenFunction::ObjectReference;
      break;
    case MetatypeRepresentation::Thick:
      selfKind = IRGenFunction::SwiftMetatype;
      break;
    case MetatypeRepresentation::ObjC:
      selfKind = IRGenFunction::ObjCMetatype;
      break;
    }
  }
  llvm::Value *value = IGF.getLoweredExplosion(selfArg).claimNext();
  if (auto dynSelfTy = dyn_cast<DynamicSelfType>(selfTy))
    selfTy = dynSelfTy.getSelfType();

  // Specify the exact Self type if we know it, either because the class
  // is final, or because the function we're emitting is a method with the
  // [exact_self_class] attribute set on it during the SIL pipeline.
  bool isExact = selfTy->getClassOrBoundGenericClass()->isFinal()
    || IGF.CurSILFn->isExactSelfClass();

  IGF.setDynamicSelfMetadata(selfTy, isExact, value, selfKind);
}

/// Emit the definition for the given SIL constant.
void IRGenModule::emitSILFunction(SILFunction *f) {
  if (f->isExternalDeclaration())
    return;

  // Do not emit bodies of public_external functions.
  if (hasPublicVisibility(f->getLinkage()) && f->isAvailableExternally())
    return;

  PrettyStackTraceSILFunction stackTrace("emitting IR", f);
  IRGenSILFunction(*this, f).emitSILFunction();
}

void IRGenSILFunction::emitSILFunction() {
  LLVM_DEBUG(llvm::dbgs() << "emitting SIL function: ";
             CurSILFn->printName(llvm::dbgs());
             llvm::dbgs() << '\n';
             CurSILFn->print(llvm::dbgs()));
  
  assert(!CurSILFn->empty() && "function has no basic blocks?!");

  if (CurSILFn->getDynamicallyReplacedFunction())
    IGM.IRGen.addDynamicReplacement(CurSILFn);

  if (CurSILFn->getLinkage() == SILLinkage::Shared) {
    if (CurSILFn->markedAsAlwaysEmitIntoClient() &&
        CurSILFn->hasOpaqueResultTypeWithAvailabilityConditions()) {
      auto *V = CurSILFn->getLocation().castToASTNode<ValueDecl>();
      auto *opaqueResult = V->getOpaqueResultTypeDecl();
      // `@_alwaysEmitIntoClient` declaration with opaque result
      // has to emit opaque type descriptor into client module
      // when it has availability conditions because the underlying
      // type in such cases is unknown until runtime.
      IGM.maybeEmitOpaqueTypeDecl(opaqueResult);
    }
  }

  auto funcTy = CurSILFn->getLoweredFunctionType();
  bool isAsyncFn = funcTy->isAsync();
  if (isAsyncFn && funcTy->getLanguage() == SILFunctionLanguage::Swift) {
    emitAsyncFunctionPointer(IGM,
                             CurFn,
                             LinkEntity::forSILFunction(CurSILFn),
                             getAsyncContextLayout(*this).getSize());
  }
  if (isAsyncFn) {
    IGM.noteSwiftAsyncFunctionDef();
  }

  if (CurSILFn->isRuntimeAccessible())
    IGM.addAccessibleFunction(CurSILFn);

  // Emit distributed accessor, and mark the thunk as accessible
  // by name at runtime through it.
  if (CurSILFn->isDistributed() && CurSILFn->isThunk() == IsThunk) {
    IGM.emitDistributedTargetAccessor(CurSILFn);
    IGM.addAccessibleFunction(CurSILFn);
  }

  // Configure the dominance resolver.
  // TODO: consider re-using a dom analysis from the PassManager
  // TODO: consider using a cheaper analysis at -O0
  setDominanceResolver([](IRGenFunction &IGF_,
                          DominancePoint activePoint,
                          DominancePoint dominatingPoint) -> bool {
    IRGenSILFunction &IGF = static_cast<IRGenSILFunction&>(IGF_);
    if (!IGF.Dominance) {
      IGF.Dominance.reset(new DominanceInfo(IGF.CurSILFn));
    }
    return IGF.Dominance->dominates(dominatingPoint.as<SILBasicBlock>(),
                                    activePoint.as<SILBasicBlock>());
  });
  
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitFunction(*CurSILFn, CurFn);

  // Map the entry bb.
  LoweredBBs[&*CurSILFn->begin()] = LoweredBB(&CurFn->back(), {});
  // Create LLVM basic blocks for the other bbs.
  for (auto bi = std::next(CurSILFn->begin()), be = CurSILFn->end(); bi != be;
       ++bi) {
    // FIXME: Use the SIL basic block's name.
    llvm::BasicBlock *llBB = llvm::BasicBlock::Create(IGM.getLLVMContext());
    auto phis = emitPHINodesForBBArgs(*this, &*bi, llBB);
    CurFn->getBasicBlockList().push_back(llBB);
    LoweredBBs[&*bi] = LoweredBB(llBB, std::move(phis));
  }

  auto entry = LoweredBBs.begin();
  Builder.SetInsertPoint(entry->second.bb);

  // Map the LLVM arguments to arguments on the entry point BB.
  Explosion params = collectParameters();

  switch (funcTy->getLanguage()) {
  case SILFunctionLanguage::Swift:
    emitEntryPointArgumentsNativeCC(*this, entry->first, params);
    break;
  case SILFunctionLanguage::C:
    emitEntryPointArgumentsCOrObjC(*this, entry->first, params, funcTy);
    break;
  }
  emitDynamicSelfMetadata(*this);

  assert(params.empty() && "did not map all llvm params to SIL params?!");

  // It's really nice to be able to assume that we've already emitted
  // all the values from dominating blocks --- it makes simple
  // peepholing more powerful and allows us to avoid the need for
  // nasty "forward-declared" values.  We can do this by emitting
  // blocks using a simple walk through the successor graph.
  //
  // We do want to preserve the original source order, but that's done
  // by having previously added all the primary blocks to the LLVM
  // function in their original order.  As long as any secondary
  // blocks are inserted after the current IP instead of at the end
  // of the function, we're fine.

  // Invariant: for every block in the work queue, we have visited all
  // of its dominators.
  // Start with the entry block, for which the invariant trivially holds.
  BasicBlockWorklist workQueue(&*CurSILFn->getEntryBlock());

  while (SILBasicBlock *bb = workQueue.pop()) {
    // Emit the block.
    visitSILBasicBlock(bb);

#ifndef NDEBUG
    // Assert that the current IR IP (if valid) is immediately prior
    // to the initial IR block for the next primary SIL block.
    // It's not semantically necessary to preserve SIL block order,
    // but we really should.
    if (auto curBB = Builder.GetInsertBlock()) {
      auto next = std::next(SILFunction::iterator(bb));
      if (next != CurSILFn->end()) {
        auto nextBB = LoweredBBs[&*next].bb;
        assert(&*std::next(curBB->getIterator()) == nextBB &&
               "lost source SIL order?");
      }
    }
#endif

    // The immediate dominator of a successor of this block needn't be
    // this block, but it has to be something which dominates this
    // block.  In either case, we've visited it.
    //
    // Therefore the invariant holds of all the successors, and we can
    // queue them up if we haven't already visited them.
    for (auto *succBB : bb->getSuccessorBlocks()) {
      workQueue.pushIfNotVisited(succBB);
    }
  }

  // If there are dead blocks in the SIL function, we might have left
  // invalid blocks in the IR.  Do another pass and kill them off.
  for (SILBasicBlock &bb : *CurSILFn)
    if (!workQueue.isVisited(&bb))
      LoweredBBs[&bb].bb->eraseFromParent();

}

void IRGenSILFunction::estimateStackSize() {
  if (EstimatedStackSize >= 0)
    return;

  // TODO: as soon as we generate alloca instructions with accurate lifetimes
  // we should also do a better stack size calculation here. Currently we
  // add all stack sizes even if life ranges do not overlap.
  for (SILBasicBlock &BB : *CurSILFn) {
    for (SILInstruction &I : BB) {
      if (auto *ASI = dyn_cast<AllocStackInst>(&I)) {
        const TypeInfo &type = getTypeInfo(ASI->getElementType());
        if (llvm::Constant *SizeConst = type.getStaticSize(IGM)) {
          auto *SizeInt = cast<llvm::ConstantInt>(SizeConst);
          EstimatedStackSize += (int)SizeInt->getSExtValue();
        }
      }
    }
  }
}

void IRGenSILFunction::visitSILBasicBlock(SILBasicBlock *BB) {
  // Insert into the lowered basic block.
  llvm::BasicBlock *llBB = getLoweredBB(BB).bb;
  Builder.SetInsertPoint(llBB);

  bool InEntryBlock = BB->pred_empty();

  // Set this block as the dominance point.  This implicitly communicates
  // with the dominance resolver configured in emitSILFunction.
  DominanceScope dominance(*this, InEntryBlock ? DominancePoint::universal()
                                               : DominancePoint(BB));

  // Generate the body.
  bool InCleanupBlock = false;
  bool KeepCurrentLocation = false;

  for (auto &I : *BB) {
    if (IGM.DebugInfo) {
      // Set the debug info location for I, if applicable.
      auto DS = I.getDebugScope();
      SILLocation ILoc = I.getLoc();
      // Handle cleanup locations.
      if (ILoc.is<CleanupLocation>()) {
        // Cleanup locations point to the decl of the value that is
        // being destroyed (for diagnostic generation). As far as
        // the linetable is concerned, cleanups at the end of a
        // lexical scope should point to the cleanup location, which
        // is the location of the last instruction in the basic block.
        if (!InCleanupBlock) {
          InCleanupBlock = true;
          // Scan ahead to see if this is the final cleanup block in
          // this basic block.
          auto It = I.getIterator();
          do ++It; while (It != BB->end() &&
                          It->getLoc().is<CleanupLocation>());
          // We are still in the middle of a basic block?
          if (It != BB->end() && !isa<TermInst>(It))
            KeepCurrentLocation = true;
        }

        // Assign the cleanup location to this instruction.
        if (!KeepCurrentLocation) {
          assert(BB->getTerminator());
          ILoc = BB->getTerminator()->getLoc();
          DS = BB->getTerminator()->getDebugScope();
        }
      } else if (InCleanupBlock) {
        KeepCurrentLocation = false;
        InCleanupBlock = false;
      }

      // Until SILDebugScopes are properly serialized, bare functions
      // are allowed to not have a scope.
      if (!DS) {
        if (CurSILFn->isBare())
          DS = CurSILFn->getDebugScope();
        assert(maybeScopeless(I) && "instruction has location, but no scope");
      }

      // Set the builder's debug location.
      if (DS && !KeepCurrentLocation)
        IGM.DebugInfo->setCurrentLoc(Builder, DS, ILoc);
      else {
        // Reuse the last scope for an easier-to-read line table.
        auto Prev = --I.getIterator();
        if (Prev != BB->end())
          DS = Prev->getDebugScope();

        // Use an artificial (line 0) location, to indicate we'd like to
        // reuse the last debug loc.
        IGM.DebugInfo->setCurrentLoc(
            Builder, DS, RegularLocation::getAutoGeneratedLocation());
      }
      if (isa<TermInst>(&I))
        emitDebugVariableRangeExtension(BB);
    }
    
#ifdef CHECK_RUNTIME_EFFECT_ANALYSIS
    IGM.effectOfRuntimeFuncs = RuntimeEffect::NoEffect;
    IGM.emittedRuntimeFuncs.clear();
#endif

    visit(&I);

#ifdef CHECK_RUNTIME_EFFECT_ANALYSIS
    if (!isa<DebugValueInst>(&I)) {
      SILType impactType;
      RuntimeEffect silImpact = getRuntimeEffect(&I, impactType);
      if ((unsigned)IGM.effectOfRuntimeFuncs & ~(unsigned)silImpact) {
        llvm::errs() << "Missing runtime impact " << (unsigned)silImpact
                     << ", expected: " << (unsigned)IGM.effectOfRuntimeFuncs
                     << "\n in " << I
                     << "emitted runtime functions:\n";
        for (const char *funcName : IGM.emittedRuntimeFuncs) {
          llvm::errs() << "  " << funcName << "()\n";
        }
        llvm_unreachable("wrong runtime impact definition");
      }
    }
#endif
  }

  assert(Builder.hasPostTerminatorIP() && "SIL bb did not terminate block?!");
}

void IRGenSILFunction::visitDifferentiableFunctionInst(
    DifferentiableFunctionInst *i) {
  auto origFnExp = getLoweredExplosion(i->getOriginalFunction());
  Explosion e;
  e.add(origFnExp.claimAll());
  // TODO(TF-1211): Uncomment assertions after upstreaming differentiation
  // transform.
  // The mandatory differentiation transform canonicalizes
  // `differentiable_function` instructions and ensures that derivative operands
  // are populated.
  /*
  assert(i->hasDerivativeFunctions());
  for (auto &derivFnOperand : i->getDerivativeFunctionArray())
    e.add(getLoweredExplosion(derivFnOperand.get()).claimAll());
  setLoweredExplosion(i, e);
  */
  // Note: code below is a temporary measure until TF-1211. Derivative function
  // operands should always exist after the differentiation transform.
  auto getDerivativeExplosion = [&](AutoDiffDerivativeFunctionKind kind) {
    // If the derivative value exists, get its explosion.
    if (i->hasDerivativeFunctions())
      return getLoweredExplosion(i->getDerivativeFunction(kind));
    // Otherwise, create an undef explosion.
    auto origFnType =
        i->getOriginalFunction()->getType().castTo<SILFunctionType>();
    auto derivativeFnType = origFnType->getAutoDiffDerivativeFunctionType(
        i->getParameterIndices(), i->getResultIndices(), kind,
        i->getModule().Types,
        LookUpConformanceInModule(i->getModule().getSwiftModule()));
    auto *undef = SILUndef::get(
        SILType::getPrimitiveObjectType(derivativeFnType), *i->getFunction());
    return getLoweredExplosion(undef);
  };
  auto jvpExp = getDerivativeExplosion(AutoDiffDerivativeFunctionKind::JVP);
  e.add(jvpExp.claimAll());
  auto vjpExp = getDerivativeExplosion(AutoDiffDerivativeFunctionKind::VJP);
  e.add(vjpExp.claimAll());
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::
visitLinearFunctionInst(LinearFunctionInst *i) {
  auto origExp = getLoweredExplosion(i->getOriginalFunction());
  Explosion e;
  e.add(origExp.claimAll());
  assert(i->hasTransposeFunction());
  e.add(getLoweredExplosion(i->getTransposeFunction()).claimAll());
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitDifferentiableFunctionExtractInst(
    DifferentiableFunctionExtractInst *i) {
  unsigned structFieldOffset = i->getExtractee().rawValue;
  unsigned fieldSize = 1;
  auto fnRepr = i->getOperand()->getType().getFunctionRepresentation();
  if (fnRepr == SILFunctionTypeRepresentation::Thick) {
    structFieldOffset *= 2;
    fieldSize = 2;
  }
  auto diffFnExp = getLoweredExplosion(i->getOperand());
  assert(diffFnExp.size() == fieldSize * 3);
  Explosion e;
  e.add(diffFnExp.getRange(structFieldOffset, structFieldOffset + fieldSize));
  (void)diffFnExp.claimAll();
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::
visitLinearFunctionExtractInst(LinearFunctionExtractInst *i) {
  unsigned structFieldOffset = i->getExtractee().rawValue;
  unsigned fieldSize = 1;
  auto fnRepr = i->getOperand()->getType().getFunctionRepresentation();
  if (fnRepr == SILFunctionTypeRepresentation::Thick) {
    structFieldOffset *= 2;
    fieldSize = 2;
  }
  auto diffFnExp = getLoweredExplosion(i->getOperand());
  assert(diffFnExp.size() == fieldSize * 2);
  Explosion e;
  e.add(diffFnExp.getRange(structFieldOffset, structFieldOffset + fieldSize));
  (void)diffFnExp.claimAll();
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitDifferentiabilityWitnessFunctionInst(
    DifferentiabilityWitnessFunctionInst *i) {
  llvm::Value *diffWitness =
      IGM.getAddrOfDifferentiabilityWitness(i->getWitness());
  unsigned offset = 0;
  switch (i->getWitnessKind()) {
  case DifferentiabilityWitnessFunctionKind::JVP:
    offset = 0;
    break;
  case DifferentiabilityWitnessFunctionKind::VJP:
    offset = 1;
    break;
  case DifferentiabilityWitnessFunctionKind::Transpose:
    llvm_unreachable("Not yet implemented");
  }

  diffWitness = Builder.CreateStructGEP(IGM.DifferentiabilityWitnessTy,
                                        diffWitness, offset);
  diffWitness = Builder.CreateLoad(
      Address(diffWitness, IGM.Int8PtrTy, IGM.getPointerAlignment()));

  auto fnType = cast<SILFunctionType>(i->getType().getASTType());
  Signature signature = IGM.getSignature(fnType);
  diffWitness =
      Builder.CreateBitCast(diffWitness, signature.getType()->getPointerTo());

  setLoweredFunctionPointer(
      i, FunctionPointer::createUnsigned(fnType, diffWitness, signature, true));
}

void IRGenSILFunction::visitHasSymbolInst(HasSymbolInst *i) {
  auto fn = IGM.emitHasSymbolFunction(i->getDecl());
  llvm::CallInst *call = Builder.CreateCall(fn->getFunctionType(), fn, {});

  Explosion e;
  e.add(call);
  setLoweredValue(i, e);
}

FunctionPointer::Kind irgen::classifyFunctionPointerKind(SILFunction *fn) {
  using SpecialKind = FunctionPointer::SpecialKind;

  // Check for some special cases, which are currently all async:
  if (fn->isAsync()) {
    auto name = fn->getName();
    if (name.equals("swift_task_future_wait"))
      return SpecialKind::TaskFutureWait;
    if (name.equals("swift_task_future_wait_throwing"))
      return SpecialKind::TaskFutureWaitThrowing;

    if (name.equals("swift_asyncLet_wait"))
      return SpecialKind::AsyncLetWait;
    if (name.equals("swift_asyncLet_wait_throwing"))
      return SpecialKind::AsyncLetWaitThrowing;

    if (name.equals("swift_asyncLet_get"))
      return SpecialKind::AsyncLetGet;
    if (name.equals("swift_asyncLet_get_throwing"))
      return SpecialKind::AsyncLetGetThrowing;

    if (name.equals("swift_asyncLet_finish"))
      return SpecialKind::AsyncLetFinish;
    
    if (name.equals("swift_taskGroup_wait_next_throwing"))
      return SpecialKind::TaskGroupWaitNext;

    if (name.equals("swift_taskGroup_waitAll"))
      return SpecialKind::TaskGroupWaitAll;

    if (name.equals("swift_distributed_execute_target"))
      return SpecialKind::DistributedExecuteTarget;
  }

  return fn->getLoweredFunctionType();
}
// Async functions that end up with weak_odr or linkonce_odr linkage may not be
// directly called because we need to preserve the connection between the
// function's implementation and the function's context size in the async
// function pointer data structure.
static bool mayDirectlyCallAsync(SILFunction *fn) {
  if (fn->getLinkage() == SILLinkage::Shared ||
      fn->getLinkage() == SILLinkage::PublicNonABI) {
    return false;
  }

  return true;
}

void IRGenSILFunction::visitFunctionRefBaseInst(FunctionRefBaseInst *i) {
  auto fn = i->getInitiallyReferencedFunction();
  auto fnType = fn->getLoweredFunctionType();

  auto fpKind = irgen::classifyFunctionPointerKind(fn);

  auto sig = IGM.getSignature(fnType, fpKind, true /*forStaticCall*/);

  // Note that the pointer value returned by getAddrOfSILFunction doesn't
  // necessarily have element type sig.getType(), e.g. if it's imported.
  // FIXME: should we also be using this for dynamic async functions?
  // We seem to completely ignore this in the standard async FP path below.
  auto *fnPtr = IGM.getAddrOfSILFunction(
      fn, NotForDefinition, false /*isDynamicallyReplaceableImplementation*/,
      isa<PreviousDynamicFunctionRefInst>(i));

  // For ordinary async functions, produce both the async FP and the
  // direct address of the function.  In the common case where we
  // directly call the function, we'll want to call the latter rather
  // than indirecting through the async FP.
  llvm::Constant *value;
  llvm::Constant *secondaryValue;
  bool useSignature = false;
  if (fpKind.isAsyncFunctionPointer()) {
    value = IGM.getAddrOfAsyncFunctionPointer(fn);
    value = llvm::ConstantExpr::getBitCast(value, fnPtr->getType());
    secondaryValue = mayDirectlyCallAsync(fn) ?
      IGM.getAddrOfSILFunction(fn, NotForDefinition) : nullptr;
    if (!secondaryValue)
      useSignature = true;

  // For ordinary sync functions and special async functions, produce
  // only the direct address of the function.  The runtime does not
  // define async FP symbols for the special async functions it defines.
  } else {
    value = fnPtr;
    secondaryValue = nullptr;
  }
  FunctionPointer fp =
      FunctionPointer::forDirect(fpKind, value, secondaryValue, sig, useSignature);
  // Update the foreign no-throw information if needed.
  if (const auto *cd = fn->getClangDecl()) {
    if (auto *cfd = dyn_cast<clang::FunctionDecl>(cd)) {
      if (auto *cft = cfd->getType()->getAs<clang::FunctionProtoType>()) {
        if (cft->isNothrow())
          fp.setForeignNoThrow();
      }
    }
    if (IGM.emittedForeignFunctionThunksWithExceptionTraps.count(fnPtr))
      fp.setForeignCallCatchesExceptionInThunk();
  }
  // Store the function as a FunctionPointer so we can avoid bitcasting
  // or thunking if we don't need to.
  setLoweredFunctionPointer(i, fp);
}

void IRGenSILFunction::visitFunctionRefInst(FunctionRefInst *i) {
  visitFunctionRefBaseInst(i);
}

void IRGenSILFunction::visitDynamicFunctionRefInst(DynamicFunctionRefInst *i) {
  visitFunctionRefBaseInst(i);
}

void IRGenSILFunction::visitPreviousDynamicFunctionRefInst(
    PreviousDynamicFunctionRefInst *i) {
  if (UseBasicDynamicReplacement) {
    IGM.unimplemented(i->getLoc().getSourceLoc(),
      ": calling the original implementation of a dynamic function is not "
      "supported with -Xllvm -basic-dynamic-replacement");
  }
  visitFunctionRefBaseInst(i);
}

void IRGenSILFunction::visitAllocGlobalInst(AllocGlobalInst *i) {
  SILGlobalVariable *var = i->getReferencedGlobal();
  SILType loweredTy = var->getLoweredType();
  auto &ti = getTypeInfo(loweredTy);

  auto expansion = IGM.getResilienceExpansionForLayout(var);

  // If the global is fixed-size in all resilience domains that can see it,
  // we allocated storage for it statically, and there's nothing to do.
  if (ti.isFixedSize(expansion))
    return;

  // Otherwise, the static storage for the global consists of a fixed-size
  // buffer.
  Address addr = IGM.getAddrOfSILGlobalVariable(var, ti,
                                                NotForDefinition);
  emitAllocateValueInBuffer(*this, loweredTy, addr);
}

void IRGenSILFunction::visitGlobalAddrInst(GlobalAddrInst *i) {
  SILGlobalVariable *var = i->getReferencedGlobal();
  SILType loweredTy = var->getLoweredTypeInContext(getExpansionContext());
  assert(loweredTy == i->getType().getObjectType());
  auto &ti = getTypeInfo(loweredTy);
  
  auto expansion = IGM.getResilienceExpansionForLayout(var);

  // If the variable is empty in all resilience domains that can see it,
  // don't actually emit a symbol for the global at all, just return undef.
  if (ti.isKnownEmpty(expansion)) {
    setLoweredAddress(i, ti.getUndefAddress());
    return;
  }

  Address addr = IGM.getAddrOfSILGlobalVariable(var, ti,
                                                NotForDefinition);

  // If the global is fixed-size in all resilience domains that can see it,
  // we allocated storage for it statically, and there's nothing to do.
  if (ti.isFixedSize(expansion)) {
    setLoweredAddress(i, addr);
    return;
  }

  // Otherwise, the static storage for the global consists of a fixed-size
  // buffer; project it.
  addr = emitProjectValueInBuffer(*this, loweredTy, addr);
  
  setLoweredAddress(i, addr);
}

/// Returns true if \p val has no other uses than ref_element_addr or
/// ref_tail_addr.
static bool hasOnlyProjections(SILValue val) {
  for (Operand *use : val->getUses()) {
    SILInstruction *user = use->getUser();
    if (auto *upCast = dyn_cast<UpcastInst>(user)) {
      if (!hasOnlyProjections(upCast))
        return false;
      continue;
    }
    if (isa<RefElementAddrInst>(user) || isa<RefTailAddrInst>(user))
      continue;
    return false;
  }
  return true;
}

void IRGenSILFunction::visitGlobalValueInst(GlobalValueInst *i) {
  SILGlobalVariable *var = i->getReferencedGlobal();
  assert(var->isInitializedObject() &&
         "global_value only supported for statically initialized objects");
  SILType loweredTy = var->getLoweredType();
  assert(loweredTy == i->getType());
  auto &ti = getTypeInfo(loweredTy);
  assert(ti.isFixedSize(IGM.getResilienceExpansionForLayout(var)));

  llvm::Value *Ref = IGM.getAddrOfSILGlobalVariable(var, ti,
                                                NotForDefinition).getAddress();
  // We don't need to initialize the global object if it's never used for
  // something which can access the object header.
  if (!hasOnlyProjections(i) && !IGM.canMakeStaticObjectsReadOnly()) {
    auto ClassType = loweredTy.getASTType();
    llvm::Value *Metadata =
      emitClassHeapMetadataRef(*this, ClassType, MetadataValueType::TypeMetadata,
                               MetadataState::Complete);
    llvm::Value *CastAddr = Builder.CreateBitCast(Ref, IGM.RefCountedPtrTy);
    llvm::Value *InitRef = emitInitStaticObjectCall(Metadata, CastAddr, "staticref");
    Ref = Builder.CreateBitCast(InitRef, Ref->getType());
  }
  Explosion e;
  e.add(Ref);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitBaseAddrForOffsetInst(BaseAddrForOffsetInst *i) {
  auto storagePtrTy = IGM.getStoragePointerType(i->getType());
  auto storageTy = IGM.getStorageType(i->getType());
  llvm::Value *addr = llvm::ConstantPointerNull::get(storagePtrTy);
  setLoweredAddress(i, Address(addr, storageTy, Alignment()));
}

void IRGenSILFunction::visitMetatypeInst(swift::MetatypeInst *i) {
  auto metaTy = i->getType().castTo<MetatypeType>();
  Explosion e;
  emitMetatypeRef(*this, metaTy, e);
  setLoweredExplosion(i, e);
}

static llvm::Value *getClassBaseValue(IRGenSILFunction &IGF,
                                      SILValue v) {
  if (v->getType().isAddress()) {
    auto addr = IGF.getLoweredAddress(v);
    return IGF.Builder.CreateLoad(addr);
  }
  
  Explosion e = IGF.getLoweredExplosion(v);
  return e.claimNext();
}

void IRGenSILFunction::visitValueMetatypeInst(swift::ValueMetatypeInst *i) {
  SILType instanceTy = i->getOperand()->getType();
  auto metaTy = i->getType().castTo<MetatypeType>();
  
  if (metaTy->getRepresentation() == MetatypeRepresentation::Thin) {
    Explosion empty;
    setLoweredExplosion(i, empty);
    return;
  }
  
  Explosion e;
  
  if (instanceTy.getClassOrBoundGenericClass()) {
    e.add(emitDynamicTypeOfHeapObject(*this,
                           getClassBaseValue(*this, i->getOperand()),
                           metaTy->getRepresentation(), instanceTy,
                           CurSILFn->getGenericSignature()));
  } else if (auto arch = instanceTy.getAs<ArchetypeType>()) {
    if (arch->requiresClass()) {
      e.add(emitDynamicTypeOfHeapObject(*this,
                             getClassBaseValue(*this, i->getOperand()),
                             metaTy->getRepresentation(), instanceTy,
                             CurSILFn->getGenericSignature()));
    } else {
      Address base = getLoweredAddress(i->getOperand());
      e.add(emitDynamicTypeOfOpaqueArchetype(*this, base,
                                             i->getOperand()->getType()));
      // FIXME: We need to convert this back to an ObjC class for an
      // ObjC metatype representation.
      if (metaTy->getRepresentation() == MetatypeRepresentation::ObjC)
        unimplemented(i->getLoc().getSourceLoc(),
                      "objc metatype of non-class-bounded archetype");
    }
  } else {
    emitMetatypeRef(*this, metaTy, e);
  }
  
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitExistentialMetatypeInst(
                                            swift::ExistentialMetatypeInst *i) {
  Explosion result;
  SILValue op = i->getOperand();
  SILType opType = op->getType();

  switch (opType.getPreferredExistentialRepresentation()) {
  case ExistentialRepresentation::Metatype: {
    Explosion existential = getLoweredExplosion(op);
    emitMetatypeOfMetatype(*this, existential, opType, result);
    break;
  }
  case ExistentialRepresentation::Class: {
    Explosion existential = getLoweredExplosion(op);
    emitMetatypeOfClassExistential(*this, existential, i->getType(),
                                   opType, CurSILFn->getGenericSignature(),
                                   result);
    break;
  }
  case ExistentialRepresentation::Boxed: {
    Explosion existential = getLoweredExplosion(op);
    emitMetatypeOfBoxedExistential(*this, existential, opType, result);
    break;
  }
  case ExistentialRepresentation::Opaque: {
    Address existential = getLoweredAddress(op);
    emitMetatypeOfOpaqueExistential(*this, existential, opType, result);
    break;
  }
  case ExistentialRepresentation::None:
    llvm_unreachable("Bad existential representation");
  }

  setLoweredExplosion(i, result);
}

static void emitApplyArgument(IRGenSILFunction &IGF,
                              SILValue arg,
                              SILType paramType,
                              Explosion &out) {
  bool isSubstituted = (arg->getType() != paramType);

  // For indirect arguments, we just need to pass a pointer.
  if (paramType.isAddress()) {
    // This address is of the substituted type.
    auto addr = IGF.getLoweredAddress(arg);

    // If a substitution is in play, just bitcast the address.
    if (isSubstituted) {
      auto origType = IGF.IGM.getStorageType(paramType);
      addr = IGF.Builder.CreateElementBitCast(addr, origType);
    }
      
    out.add(addr.getAddress());
    return;
  }

  // Otherwise, it's an explosion, which we may need to translate,
  // both in terms of explosion level and substitution levels.
  assert(arg->getType().isObject());

  // Fast path: avoid an unnecessary temporary explosion.
  if (!isSubstituted) {
    IGF.getLoweredExplosion(arg, out);
    return;
  }

  Explosion temp = IGF.getLoweredExplosion(arg);
  reemitAsUnsubstituted(IGF, paramType, arg->getType(),
                        temp, out);
}

static llvm::Value *getObjCClassForValue(IRGenFunction &IGF,
                                         llvm::Value *selfValue,
                                         CanAnyMetatypeType selfType) {
  // If we have a Swift metatype, map it to the heap metadata, which
  // will be the Class for an ObjC type.
  switch (selfType->getRepresentation()) {
  case swift::MetatypeRepresentation::ObjC:
    return selfValue;

  case swift::MetatypeRepresentation::Thick:
    // Convert thick metatype to Objective-C metatype.
    return emitClassHeapMetadataRefForMetatype(IGF, selfValue,
                                               selfType.getInstanceType());

  case swift::MetatypeRepresentation::Thin:
    llvm_unreachable("Cannot convert Thin metatype to ObjC metatype");
  }
  llvm_unreachable("bad metatype representation");
}

static llvm::Value *
emitWitnessTableForLoweredCallee(IRGenSILFunction &IGF,
                                 CanSILFunctionType substCalleeType) {
  // This use of getSelfInstanceType() assumes that the instance type is
  // always a meaningful formal type.
  auto substSelfType = substCalleeType->getSelfInstanceType(
      IGF.IGM.getSILModule(), IGF.IGM.getMaximalTypeExpansionContext());
  auto substConformance =
      substCalleeType->getWitnessMethodConformanceOrInvalid();

  llvm::Value *argMetadata = IGF.emitTypeMetadataRef(substSelfType);
  llvm::Value *wtable =
    emitWitnessTableRef(IGF, substSelfType, &argMetadata, substConformance);

  return wtable;
}

Callee LoweredValue::getCallee(IRGenFunction &IGF,
                               llvm::Value *selfValue,
                               CalleeInfo &&calleeInfo) const {
  switch (kind) {
  case Kind::FunctionPointer: {
    auto &fn = getFunctionPointer();
    if (calleeInfo.OrigFnType->getRepresentation() ==
        SILFunctionTypeRepresentation::ObjCMethod) {
      return getObjCDirectMethodCallee(std::move(calleeInfo), fn, selfValue);
    }
    return Callee(std::move(calleeInfo), fn, selfValue);
  }

  case Kind::ObjCMethod: {
    const auto &objcMethod = getObjCMethod();
    assert(selfValue);

    // Convert a metatype 'self' argument to the ObjC class pointer.
    // FIXME: why on earth is this not correctly represented in SIL?
    if (auto metatype = dyn_cast<AnyMetatypeType>(
            calleeInfo.OrigFnType->getSelfParameter().getArgumentType(
                IGF.IGM.getSILModule(), calleeInfo.OrigFnType,
                IGF.IGM.getMaximalTypeExpansionContext()))) {
      selfValue = getObjCClassForValue(IGF, selfValue, metatype);
    }

    return getObjCMethodCallee(IGF, objcMethod, selfValue,
                               std::move(calleeInfo));
  }

  case Kind::SingletonExplosion: {
    auto functionValue = getKnownSingletonExplosion();

    switch (calleeInfo.OrigFnType->getRepresentation()) {
    case SILFunctionType::Representation::Block:
      assert(!selfValue && "block function with self?");
      return getBlockPointerCallee(IGF, functionValue, std::move(calleeInfo));

    case SILFunctionType::Representation::ObjCMethod:
    case SILFunctionType::Representation::CXXMethod:
    case SILFunctionType::Representation::Thick:
      llvm_unreachable("unexpected function with singleton representation");

    case SILFunctionType::Representation::WitnessMethod:
    case SILFunctionType::Representation::Thin:
    case SILFunctionType::Representation::Method:
      return getSwiftFunctionPointerCallee(IGF, functionValue, selfValue,
                                           std::move(calleeInfo), false, false);
    case SILFunctionType::Representation::Closure:
      return getSwiftFunctionPointerCallee(IGF, functionValue, selfValue,
                                           std::move(calleeInfo), false, true);

    case SILFunctionType::Representation::CFunctionPointer:
      assert(!selfValue && "C function pointer has self?");
      return getCFunctionPointerCallee(IGF, functionValue,
                                       std::move(calleeInfo));
    }
    llvm_unreachable("bad kind");
  }

  case Kind::ExplosionVector: {
    auto vector = getKnownExplosionVector();
    assert(calleeInfo.OrigFnType->getRepresentation()
             == SILFunctionType::Representation::Thick);

    assert(!selfValue && "thick function pointer with self?");
    assert(vector.size() == 2 && "thick function pointer with size != 2");
    llvm::Value *functionValue = vector[0];
    llvm::Value *contextValue = vector[1];
    bool castToRefcountedContext = calleeInfo.OrigFnType->isNoEscape();
    return getSwiftFunctionPointerCallee(IGF, functionValue, contextValue,
                                         std::move(calleeInfo),
                                         castToRefcountedContext, true);
  }

  case LoweredValue::Kind::EmptyExplosion:
  case LoweredValue::Kind::OwnedAddress:
  case LoweredValue::Kind::ContainedAddress:
  case LoweredValue::Kind::StackAddress:
  case LoweredValue::Kind::DynamicallyEnforcedAddress:
  case LoweredValue::Kind::CoroutineState:
    llvm_unreachable("not a valid callee");
  }
  llvm_unreachable("bad kind");
}

static std::unique_ptr<CallEmission> getCallEmissionForLoweredValue(
    IRGenSILFunction &IGF, CanSILFunctionType origCalleeType,
    CanSILFunctionType substCalleeType, const LoweredValue &lv,
    llvm::Value *selfValue, SubstitutionMap substitutions,
    WitnessMetadata *witnessMetadata) {
  Callee callee = lv.getCallee(IGF, selfValue,
                               CalleeInfo(origCalleeType, substCalleeType,
                                          substitutions));

  switch (origCalleeType->getRepresentation()) {
  case SILFunctionType::Representation::WitnessMethod: {
    auto wtable = emitWitnessTableForLoweredCallee(IGF, substCalleeType);
    witnessMetadata->SelfWitnessTable = wtable;
    break;
  }

  case SILFunctionType::Representation::ObjCMethod:
  case SILFunctionType::Representation::CXXMethod:
  case SILFunctionType::Representation::Thick:
  case SILFunctionType::Representation::Block:
  case SILFunctionType::Representation::Thin:
  case SILFunctionType::Representation::CFunctionPointer:
  case SILFunctionType::Representation::Method:
  case SILFunctionType::Representation::Closure:
    break;
  }

  auto callEmission = getCallEmission(IGF, selfValue, std::move(callee));
  if (IGF.CurSILFn->isThunk())
    callEmission->addFnAttribute(llvm::Attribute::NoInline);

  return callEmission;
}

/// Get the size passed to stackAlloc().
static llvm::Value *getStackAllocationSize(IRGenSILFunction &IGF,
                                           SILValue vCapacity,
                                           SILValue vStride,
                                           SourceLoc loc) {
  auto &Diags = IGF.IGM.Context.Diags;

  // Check for a negative capacity, which is invalid.
  auto capacity = IGF.getLoweredSingletonExplosion(vCapacity);
  Optional<int64_t> capacityValue;
  if (auto capacityConst = dyn_cast<llvm::ConstantInt>(capacity)) {
    capacityValue = capacityConst->getSExtValue();
    if (*capacityValue < 0) {
      Diags.diagnose(loc, diag::temporary_allocation_size_negative);
    }
  }

  // Check for a negative stride, which should never occur because the caller
  // should always be using MemoryLayout<T>.stride to produce this value.
  auto stride = IGF.getLoweredSingletonExplosion(vStride);
  Optional<int64_t> strideValue;
  if (auto strideConst = dyn_cast<llvm::ConstantInt>(stride)) {
    strideValue = strideConst->getSExtValue();
    if (*strideValue < 0) {
      llvm_unreachable("Builtin.stackAlloc() caller passed an invalid stride");
    }
  }

  // Get the byte count (the product of capacity and stride.)
  llvm::Value *result = nullptr;
  if (capacityValue && strideValue) {
    int64_t byteCount = 0;
    auto overflow = llvm::MulOverflow(*capacityValue, *strideValue, byteCount);
    if (overflow) {
      Diags.diagnose(loc, diag::temporary_allocation_size_overflow);
    } else {
      // For architectures narrower than 64 bits, check if the byte count fits
      // in a (signed) size value.
      auto maxByteCount = llvm::APInt::getSignedMaxValue(
        IGF.IGM.SizeTy->getBitWidth()).getSExtValue();
      if (byteCount > maxByteCount) {
        Diags.diagnose(loc, diag::temporary_allocation_size_overflow);
      }
    }
    result = llvm::ConstantInt::get(IGF.IGM.SizeTy, byteCount);

  } else {
    // If either value is not known at compile-time, preconditions must be
    // tested at runtime by Builtin.stackAlloc()'s caller. See
    // _byteCountForTemporaryAllocation(of:capacity:).
    result = IGF.Builder.CreateMul(capacity, stride);
  }

  // If the caller requests a zero-byte allocation, allocate one byte instead
  // to ensure that the resulting pointer is valid and unique on the stack.
  return IGF.Builder.CreateIntrinsicCall(llvm::Intrinsic::umax,
    {IGF.IGM.SizeTy}, {llvm::ConstantInt::get(IGF.IGM.SizeTy, 1), result});
}

/// Get the alignment passed to stackAlloc() as a compile-time constant.
///
/// If the specified alignment is not known at compile time or is not valid,
/// the default maximum alignment is substituted.
static Alignment getStackAllocationAlignment(IRGenSILFunction &IGF,
                                             SILValue v,
                                             SourceLoc loc) {
  auto &Diags = IGF.IGM.Context.Diags;

  // Check for a non-positive alignment, which is invalid.
  auto align = IGF.getLoweredSingletonExplosion(v);
  if (auto alignConst = dyn_cast<llvm::ConstantInt>(align)) {
    auto alignValue = alignConst->getSExtValue();
    if (alignValue <= 0) {
      Diags.diagnose(loc, diag::temporary_allocation_alignment_not_positive);
    } else if (!llvm::isPowerOf2_64(alignValue)) {
      Diags.diagnose(loc, diag::temporary_allocation_alignment_not_power_of_2);
    } else {
      return Alignment(alignValue);
    }
  }

  // If the alignment is not known at compile-time, preconditions must be tested
  // at runtime by Builtin.stackAlloc()'s caller. See
  // _isStackAllocationSafe(byteCount:alignment:).
  return Alignment(MaximumAlignment);
}

/// Emit a call to a stack allocation builtin (stackAlloc() or stackDealloc().)
///
/// Returns whether or not `i` was such a builtin (true if so, false if it was
/// some other builtin.)
static bool emitStackAllocBuiltinCall(IRGenSILFunction &IGF,
                                      swift::BuiltinInst *i) {
  if (i->getBuiltinKind() == BuiltinValueKind::StackAlloc ||
      i->getBuiltinKind() == BuiltinValueKind::UnprotectedStackAlloc) {
    // Stack-allocate a buffer with the specified size/alignment.
    auto loc = i->getLoc().getSourceLoc();
    auto size = getStackAllocationSize(
      IGF, i->getOperand(0), i->getOperand(1), loc);
    auto align = getStackAllocationAlignment(IGF, i->getOperand(2), loc);

    auto stackAddress = IGF.emitDynamicAlloca(IGF.IGM.Int8Ty, size, align,
                                              false, "temp_alloc");
    IGF.setLoweredStackAddress(i, stackAddress);

    return true;

  } else if (i->getBuiltinKind() == BuiltinValueKind::StackDealloc) {
    // Deallocate a stack address previously allocated with the StackAlloc
    // builtin above.
    auto address = i->getOperand(0);
    auto stackAddress = IGF.getLoweredStackAddress(address);

    if (stackAddress.getAddress().isValid()) {
      IGF.emitDeallocateDynamicAlloca(stackAddress, false);
    }

    return true;
  }

  return false;
}

void IRGenSILFunction::visitBuiltinInst(swift::BuiltinInst *i) {
  const BuiltinInfo &builtin = getSILModule().getBuiltinInfo(i->getName());

  if (emitStackAllocBuiltinCall(*this, i)) {
    return;
  }

  auto argValues = i->getArguments();
  Explosion args;
  SmallVector<SILType, 4> argTypes;

  for (auto idx : indices(argValues)) {
    auto argValue = argValues[idx];

    // Builtin arguments should never be substituted, so use the value's type
    // as the parameter type.
    emitApplyArgument(*this, argValue, argValue->getType(), args);

    argTypes.push_back(argValue->getType());
  }
  
  Explosion result;
  emitBuiltinCall(*this, builtin, i, argTypes, args, result);
  
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitApplyInst(swift::ApplyInst *i) {
  visitFullApplySite(i);
}

void IRGenSILFunction::visitTryApplyInst(swift::TryApplyInst *i) {
  visitFullApplySite(i);
}

void IRGenSILFunction::visitFullApplySite(FullApplySite site) {
  auto origCalleeType = site.getOrigCalleeType();
  auto substCalleeType = site.getSubstCalleeType();
  if (site.getOrigCalleeType()->isDifferentiable()) {
    origCalleeType = origCalleeType->getWithoutDifferentiability();
    substCalleeType = substCalleeType->getWithoutDifferentiability();
  }

  // If the callee is a differentiable function, we extract the original
  // function because we want to call the original function.
  Optional<LoweredValue> diffCalleeOrigFnLV;
  if (site.getOrigCalleeType()->isDifferentiable()) {
    auto diffFnExplosion = getLoweredExplosion(site.getCallee());
    Explosion origFnExplosion;
    unsigned fieldSize = 1;
    if (origCalleeType->getRepresentation() ==
        SILFunctionTypeRepresentation::Thick) {
      fieldSize = 2;
    }
    origFnExplosion.add(diffFnExplosion.getRange(0, 0 + fieldSize));
    (void)diffFnExplosion.claimAll();
    diffCalleeOrigFnLV = LoweredValue(origFnExplosion);
  }

  const LoweredValue &calleeLV =
      diffCalleeOrigFnLV ? *diffCalleeOrigFnLV :
                            getLoweredValue(site.getCallee());

  auto args = site.getArguments();
  SILFunctionConventions origConv(origCalleeType, getSILModule());
  assert(origConv.getNumSILArguments() == args.size());

  // Extract 'self' if it needs to be passed as the context parameter.
  llvm::Value *selfValue = nullptr;
  if (hasSelfContextParameter(origCalleeType)) {
    SILValue selfArg = args.back();
    args = args.drop_back();

    if (selfArg->getType().isObject()) {
      selfValue = getLoweredSingletonExplosion(selfArg);
    } else {
      selfValue = getLoweredAddress(selfArg).getAddress();
    }
  }

  Explosion llArgs;
  WitnessMetadata witnessMetadata;
  auto emission = getCallEmissionForLoweredValue(
      *this, origCalleeType, substCalleeType, calleeLV, selfValue,
      site.getSubstitutionMap(), &witnessMetadata);

  emission->begin();

  // Lower the arguments and return value in the callee's generic context.
  GenericContextScope scope(IGM, origCalleeType->getInvocationGenericSignature());

  // Allocate space for the coroutine buffer.
  Optional<Address> coroutineBuffer;
  switch (origCalleeType->getCoroutineKind()) {
  case SILCoroutineKind::None:
    break;

  case SILCoroutineKind::YieldOnce:
    coroutineBuffer = emitAllocYieldOnceCoroutineBuffer(*this);
    break;

  case SILCoroutineKind::YieldMany:
    coroutineBuffer = emitAllocYieldManyCoroutineBuffer(*this);
    break;
  }
  if (coroutineBuffer) {
    llArgs.add(coroutineBuffer->getAddress());
  }

  // Lower the SIL arguments to IR arguments.
  
  // Turn the formal SIL parameters into IR-gen things.
  for (auto index : indices(args)) {
    emitApplyArgument(*this, args[index], emission->getParameterType(index),
                      llArgs);
  }

  auto &calleeFP = emission->getCallee().getFunctionPointer();

  // Pass the generic arguments.
  if (hasPolymorphicParameters(origCalleeType) &&
      !calleeFP.shouldSuppressPolymorphicArguments()) {
    SubstitutionMap subMap = site.getSubstitutionMap();
    emitPolymorphicArguments(*this, origCalleeType,
                             subMap, &witnessMetadata, llArgs);
  }

  if (calleeFP.shouldPassContinuationDirectly()) {
    llArgs.add(emission->getResumeFunctionPointer());
    llArgs.add(emission->getAsyncContext());
  }

  // Add all those arguments.
  emission->setArgs(llArgs, false, &witnessMetadata);

  SILInstruction *i = site.getInstruction();
  
  Explosion result;
  emission->emitToExplosion(result, false);

  // For a simple apply, just bind the apply result to the result of the call.
  if (auto apply = dyn_cast<ApplyInst>(i)) {
    setLoweredExplosion(apply, result);
    emission->end();

  // For begin_apply, we have to destructure the call.
  } else if (auto beginApply = dyn_cast<BeginApplyInst>(i)) {
    // Grab the continuation pointer.  This will still be an i8*.
    auto continuation = result.claimNext();

    setLoweredCoroutine(
        beginApply->getTokenResult(),
        {*coroutineBuffer, continuation, emission->claimTemporaries()});

    setCorrespondingLoweredValues(beginApply->getYieldedValues(), result);

    emission->end();
  } else {
    auto tryApplyInst = cast<TryApplyInst>(i);

    // Load the error value.
    SILFunctionConventions substConv(substCalleeType, getSILModule());
    SILType errorType =
        substConv.getSILErrorType(IGM.getMaximalTypeExpansionContext());
    Address calleeErrorSlot = emission->getCalleeErrorSlot(
        errorType, /*isCalleeAsync=*/site.getOrigCalleeType()->isAsync());
    auto errorValue = Builder.CreateLoad(calleeErrorSlot);
    emission->end();

    auto &normalDest = getLoweredBB(tryApplyInst->getNormalBB());
    auto &errorDest = getLoweredBB(tryApplyInst->getErrorBB());

    // Zero the error slot to maintain the invariant that it always
    // contains null.  This will frequently become a dead store.
    auto nullError = llvm::Constant::getNullValue(errorValue->getType());
    if (!tryApplyInst->getErrorBB()->getSinglePredecessorBlock()) {
      // Only do that here if we can't move the store to the error block.
      // See below.
      Builder.CreateStore(nullError, calleeErrorSlot);
    }

    // If the error value is non-null, branch to the error destination.
    auto hasError = Builder.CreateICmpNE(errorValue, nullError);
    Builder.CreateCondBr(hasError, errorDest.bb, normalDest.bb);

    // Set up the PHI nodes on the normal edge.
    unsigned firstIndex = 0;
    addIncomingExplosionToPHINodes(*this, normalDest, firstIndex, result);
    assert(firstIndex == normalDest.phis.size());

    // Set up the PHI nodes on the error edge.
    assert(errorDest.phis.size() == 1);
    errorDest.phis[0]->addIncoming(errorValue, Builder.GetInsertBlock());

    if (tryApplyInst->getErrorBB()->getSinglePredecessorBlock()) {
      // Zeroing out the error slot only in the error block increases the chance
      // that it will become a dead store.
      auto origBB = Builder.GetInsertBlock();
      Builder.SetInsertPoint(errorDest.bb);
      Builder.CreateStore(nullError, calleeErrorSlot);
      Builder.SetInsertPoint(origBB);
    }
  }
}

/// If the value is a @convention(witness_method) function, the context
/// is the witness table that must be passed to the call.
///
/// \param v A value of possibly-polymorphic SILFunctionType.
/// \param subs This is the set of substitutions that we are going to be
/// applying to 'v'.
static std::tuple<FunctionPointer, llvm::Value*, CanSILFunctionType>
getPartialApplicationFunction(IRGenSILFunction &IGF, SILValue v,
                              SubstitutionMap subs,
                              CanSILFunctionType substFnType) {
  LoweredValue &lv = IGF.getLoweredValue(v);
  auto fnType = v->getType().castTo<SILFunctionType>();

  switch (lv.kind) {
  case LoweredValue::Kind::ContainedAddress:
  case LoweredValue::Kind::StackAddress:
  case LoweredValue::Kind::DynamicallyEnforcedAddress:
  case LoweredValue::Kind::OwnedAddress:
  case LoweredValue::Kind::EmptyExplosion:
  case LoweredValue::Kind::CoroutineState:
    llvm_unreachable("not a valid function");

  case LoweredValue::Kind::ObjCMethod:
    llvm_unreachable("objc method partial application shouldn't get here");

  case LoweredValue::Kind::FunctionPointer: {
    llvm::Value *context = nullptr;
    switch (fnType->getRepresentation()) {
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::Block:
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::CXXMethod:
      llvm_unreachable("partial_apply of foreign functions not implemented");
        
    case SILFunctionTypeRepresentation::WitnessMethod:
      context = emitWitnessTableForLoweredCallee(IGF, substFnType);
      break;
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::Closure:
      break;
    }

    auto fn = lv.getFunctionPointer();
    return std::make_tuple(fn, context, fnType);
  }
  case LoweredValue::Kind::SingletonExplosion: {
    llvm::Value *fnPtr = lv.getKnownSingletonExplosion();
    auto fn = FunctionPointer::forExplosionValue(IGF, fnPtr, fnType);
    llvm::Value *context = nullptr;
    auto repr = fnType->getRepresentation();
    assert(repr != SILFunctionType::Representation::Block &&
           "partial apply of block not implemented");
    if (repr == SILFunctionType::Representation::WitnessMethod) {
      context = emitWitnessTableForLoweredCallee(IGF, substFnType);
    }
    return std::make_tuple(fn, context, fnType);
  }
  case LoweredValue::Kind::ExplosionVector: {
    assert(fnType->getRepresentation()
             == SILFunctionType::Representation::Thick);
    Explosion ex = lv.getExplosion(IGF, v->getType());
    llvm::Value *fnPtr = ex.claimNext();
    auto fn = FunctionPointer::forExplosionValue(IGF, fnPtr, fnType);
    llvm::Value *context = ex.claimNext();
    return std::make_tuple(fn, context, fnType);
  }
  }
  llvm_unreachable("bad kind");
}

// A "simple" partial_apply is one where the argument can be directly
// adopted as the context of the result closure.
static bool isSimplePartialApply(IRGenFunction &IGF, PartialApplyInst *i) {
  // The callee type must use the `method` convention.
  auto calleeTy = i->getCallee()->getType().castTo<SILFunctionType>();
  auto resultTy = i->getFunctionType();
  
  if (calleeTy->getRepresentation() != SILFunctionTypeRepresentation::Method)
    return false;

  // Partially applying a polymorphic function entails capturing its generic 
  // arguments (it is not legal to leave any polymorphic arguments unbound)
  // which means that both self and those generic arguments would need to be
  // captured.
  if (calleeTy->isPolymorphic())
    return false;
  
  // There should be one applied argument.
  // (This is a bit stricter than necessary, because empty arguments could be
  // ignored, and for noescape closures, any amount of data less than a pointer
  // in size can be blobbed into a single context word, but those will be
  // handled by a simplification pass in SIL.)
  if (i->getNumArguments() != 1)
    return false;
  // The closure application is going to expect to pass the context in swiftself
  // only methods where the call to `hasSelfContextParameter` returns true will
  // use swiftself for the self parameter.
  if (!hasSelfContextParameter(calleeTy))
    return false;

  auto appliedParam = calleeTy->getParameters().back();
  if (resultTy->isNoEscape()) {
    // A trivial closure accepts an unowned or guaranteed argument, possibly
    // direct or indirect.
    switch (appliedParam.getConvention()) {
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_InoutAliasable:
      // Indirect arguments are trivially word sized.
      return true;
        
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned: {
      // Is the direct argument a single word-sized value?
      auto argSchema = IGF.IGM.getTypeInfo(i->getArgument(0)->getType())
                          .getSchema();
      if (argSchema.size() != 1)
        return false;
        
      if (argSchema[0].getScalarType()->getPrimitiveSizeInBits()
            != IGF.IGM.getPointerSize().getValueInBits())
        return false;
      
      return true;
    }
    default:
      return false;
    }
  } else {
    // An escaping closure argument's convention should match the callee
    // convention of the result.
    if (resultTy->getCalleeConvention() != appliedParam.getConvention()) {
      return false;
    }
    assert(!isIndirectFormalParameter(resultTy->getCalleeConvention()));
    
    auto &argInfo = IGF.IGM.getTypeInfo(i->getArgument(0)->getType());
    
    if (!argInfo.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal))
      return false;
    
    return true;
  }
}

void IRGenSILFunction::visitPartialApplyInst(swift::PartialApplyInst *i) {
  SILValue v(i);

  if (isSimplePartialApply(*this, i)) {
    Explosion function;
    
    auto &ti = IGM.getTypeInfo(v->getType());
    auto schema = ti.getSchema();
    assert(schema.size() == 2);
    auto calleeTy = schema[0].getScalarType();
    auto contextTy = schema[1].getScalarType();
    auto callee = getLoweredExplosion(i->getCallee());
    auto calleeValue = callee.claimNext();
    assert(callee.empty());
    calleeValue = Builder.CreateBitOrPointerCast(calleeValue, calleeTy);
    
    // Re-sign the implementation pointer as a closure entry point.
    auto calleeFn = FunctionPointer::forExplosionValue(*this, calleeValue,
                                                       i->getOrigCalleeType());
    function.add(calleeFn.getExplosionValue(*this, i->getFunctionType()));

    Explosion context;
    for (auto arg : i->getArguments()) {
      auto &value = getLoweredValue(arg);
      
      if (value.isAddress()) {
        context.add(value.getAnyAddress().getAddress());
      } else {
        getLoweredExplosion(arg, context);
      }
    }
    auto contextValue = context.claimNext();
    assert(context.empty());
    contextValue = Builder.CreateBitOrPointerCast(contextValue, contextTy);
    function.add(contextValue);
    
    setLoweredExplosion(v, function);
    return;
  }
  

  // NB: We collect the arguments under the substituted type.
  auto args = i->getArguments();
  auto calleeTy = i->getSubstCalleeType();
  auto params = calleeTy->getParameters();
  params = params.slice(params.size() - args.size(), args.size());
  
  Explosion llArgs;

  auto &lv = getLoweredValue(i->getCallee());

  // Lower the parameters in the callee's generic context.
  {
    GenericContextScope scope(IGM,
                            i->getOrigCalleeType()->getSubstGenericSignature());
    for (auto index : indices(args)) {
      auto paramTy = IGM.silConv.getSILType(
          params[index], calleeTy, IGM.getMaximalTypeExpansionContext());
      assert(args[index]->getType() == paramTy);
      emitApplyArgument(*this, args[index], paramTy, llArgs);
    }
  }
  
  if (lv.kind == LoweredValue::Kind::ObjCMethod) {
    // Objective-C partial applications require a different path. There's no
    // actual function pointer to capture, and we semantically can't cache
    // dispatch, so we need to perform the message send in the partial
    // application thunk.
    auto &objcMethod = lv.getObjCMethod();
    assert(i->getArguments().size() == 1 &&
           "only partial application of objc method to self implemented");
    assert(llArgs.size() == 1 &&
           "objc partial_apply argument is not a single retainable pointer?!");
    llvm::Value *selfVal = llArgs.claimNext();
    
    Explosion function;
    emitObjCPartialApplication(*this,
                               objcMethod,
                               i->getOrigCalleeType(),
                               i->getType().castTo<SILFunctionType>(),
                               selfVal,
                               i->getArguments()[0]->getType(),
                               function);
    setLoweredExplosion(i, function);
    return;
  }
  
  // Get the function value.
  auto result = getPartialApplicationFunction(*this, i->getCallee(),
                                              i->getSubstitutionMap(),
                                              i->getSubstCalleeType());
  FunctionPointer calleeFn = std::get<0>(result);
  llvm::Value *innerContext = std::get<1>(result);
  CanSILFunctionType origCalleeTy = std::get<2>(result);

  // Create the thunk and function value.
  Explosion function;
  auto closureStackAddr = emitFunctionPartialApplication(
      *this, *CurSILFn, calleeFn, innerContext, llArgs, params,
      i->getSubstitutionMap(), origCalleeTy, i->getSubstCalleeType(),
      i->getType().castTo<SILFunctionType>(), function, false);
  setLoweredExplosion(v, function);

  if (closureStackAddr) {
    assert(i->isOnStack());
    LoweredPartialApplyAllocations[v] = *closureStackAddr;
  }
}

void IRGenSILFunction::visitIntegerLiteralInst(swift::IntegerLiteralInst *i) {
  Explosion e;
  if (i->getType().is<BuiltinIntegerLiteralType>()) {
    auto pair = emitConstantIntegerLiteral(IGM, i);
    e.add(pair.Data);
    e.add(pair.Flags);
  } else {
    llvm::Value *constant = emitConstantInt(IGM, i);
    e.add(constant);
  }
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitFloatLiteralInst(swift::FloatLiteralInst *i) {
  llvm::Value *constant = emitConstantFP(IGM, i);
  Explosion e;
  e.add(constant);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitStringLiteralInst(swift::StringLiteralInst *i) {
  llvm::Value *addr;

  // Emit a load of a selector.
  if (i->getEncoding() == swift::StringLiteralInst::Encoding::ObjCSelector)
    addr = emitObjCSelectorRefLoad(i->getValue());
  else
    addr = emitAddrOfConstantString(IGM, i);

  Explosion e;
  e.add(addr);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitUnreachableInst(swift::UnreachableInst *i) {
  if (isAsync()) {
    emitCoroutineOrAsyncExit();
    return;
  }
  Builder.CreateUnreachable();
}

void IRGenFunction::emitCoroutineOrAsyncExit() {
  // The LLVM coroutine representation demands that there be a
  // unique call to llvm.coro.end.

  // If the coroutine exit block already exists, just branch to it.
  if (auto coroEndBB = CoroutineExitBlock) {
    Builder.CreateBr(coroEndBB);
    return;
  }

  // Otherwise, create it and branch to it.
  auto coroEndBB = createBasicBlock("coro.end");
  CoroutineExitBlock = coroEndBB;
  Builder.CreateBr(coroEndBB);

  // Emit the block.
  Builder.emitBlock(coroEndBB);
  auto handle = getCoroutineHandle();
  if (isAsync())
    Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_end_async,
                                {handle,
                                 /*is unwind*/ Builder.getFalse()});
  else
    Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_end,
                                {handle,
                                 /*is unwind*/ Builder.getFalse()});
  Builder.CreateUnreachable();
}

static void emitReturnInst(IRGenSILFunction &IGF,
                           SILType resultTy,
                           Explosion &result,
                           CanSILFunctionType fnType) {
  // If we're generating a coroutine, just call coro.end.
  if (IGF.isCoroutine() && !IGF.isAsync()) {
    assert(result.empty() &&
           "coroutines do not currently support non-void returns");
    IGF.emitCoroutineOrAsyncExit();
    return;
  }
  SILFunctionConventions conv(IGF.CurSILFn->getLoweredFunctionType(),
                              IGF.getSILModule());

  // The invariant on the out-parameter is that it's always zeroed, so
  // there's nothing to do here.

  // Even if SIL has a direct return, the IR-level calling convention may
  // require an indirect return.
  if (IGF.IndirectReturn.isValid()) {
    auto &retTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(resultTy));
    retTI.initialize(IGF, result, IGF.IndirectReturn, false);
    auto asyncLayout = getAsyncContextLayout(IGF);
    if (!IGF.isAsync()) {
      IGF.Builder.CreateRetVoid();
      return;
    } else {
      if (fnType->hasErrorResult()) {
        SmallVector<llvm::Value *, 16> nativeResultsStorage;
        auto errorResultType = IGF.CurSILFn->mapTypeIntoContext(
            conv.getSILErrorType(IGF.IGM.getMaximalTypeExpansionContext()));
        auto errorType =
            cast<llvm::PointerType>(IGF.IGM.getStorageType(errorResultType));
        nativeResultsStorage.push_back(llvm::ConstantPointerNull::get(errorType));
        return emitAsyncReturn(
            IGF, asyncLayout, fnType,
            Optional<llvm::ArrayRef<llvm::Value *>>(nativeResultsStorage));
      }

      return emitAsyncReturn(IGF, asyncLayout, fnType, llvm::None);
    }
  }

  auto funcResultType = IGF.CurSILFn->mapTypeIntoContext(
      conv.getSILResultType(IGF.IGM.getMaximalTypeExpansionContext()));

  if (IGF.isAsync()) {
    // If we're generating an async function, store the result into the buffer.
    auto asyncLayout = getAsyncContextLayout(IGF);
    Explosion error;
    if (fnType->hasErrorResult()) {
      SmallVector<llvm::Value *, 16> nativeResultsStorage;
      auto errorResultType = IGF.CurSILFn->mapTypeIntoContext(
          conv.getSILErrorType(IGF.IGM.getMaximalTypeExpansionContext()));
      auto errorType =
          cast<llvm::PointerType>(IGF.IGM.getStorageType(errorResultType));
      error.add(llvm::ConstantPointerNull::get(errorType));
    }
    emitAsyncReturn(IGF, asyncLayout, funcResultType, fnType, result, error);
  } else {
    auto funcLang = IGF.CurSILFn->getLoweredFunctionType()->getLanguage();
    auto swiftCCReturn = funcLang == SILFunctionLanguage::Swift;
    assert(swiftCCReturn ||
           funcLang == SILFunctionLanguage::C && "Need to handle all cases");
    IGF.emitScalarReturn(resultTy, funcResultType, result, swiftCCReturn,
                         false);
  }
}

void IRGenSILFunction::visitReturnInst(swift::ReturnInst *i) {
  Explosion result = getLoweredExplosion(i->getOperand());

  // Implicitly autorelease the return value if the function's result
  // convention is autoreleased.
  auto fnConv = CurSILFn->getConventions();
  if (fnConv.getNumDirectSILResults() == 1
      && (fnConv.getDirectSILResults().begin()->getConvention()
          == ResultConvention::Autoreleased)) {
    Explosion temp;
    temp.add(emitObjCAutoreleaseReturnValue(*this, result.claimNext()));
    result = std::move(temp);
  }

  emitReturnInst(*this, i->getOperand()->getType(), result,
                 i->getFunction()->getLoweredFunctionType());
}

void IRGenSILFunction::visitThrowInst(swift::ThrowInst *i) {
  // Store the exception to the error slot.
  llvm::Value *exn = getLoweredSingletonExplosion(i->getOperand());

  if (!isAsync()) {
    Builder.CreateStore(exn, getCallerErrorResultSlot());
    // Async functions just return to the continuation.
  } else if (isAsync()) {
    auto layout = getAsyncContextLayout(*this);
    SILFunctionConventions conv(CurSILFn->getLoweredFunctionType(),
                                getSILModule());
    auto funcResultType = CurSILFn->mapTypeIntoContext(
        conv.getSILResultType(IGM.getMaximalTypeExpansionContext()));

    Explosion empty;
    Explosion error;
    error.add(exn);
    emitAsyncReturn(*this, layout, funcResultType,
                    i->getFunction()->getLoweredFunctionType(), empty, error);
    return;
  }

  // Create a normal return, but leaving the return value undefined.
  auto fnTy = CurFn->getFunctionType();
  auto retTy = fnTy->getReturnType();
  if (retTy->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(llvm::UndefValue::get(retTy));
  }
}

void IRGenSILFunction::visitUnwindInst(swift::UnwindInst *i) {
  // Just call coro.end; there's no need to distinguish 'unwind'
  // and 'return' at the LLVM level.
  emitCoroutineOrAsyncExit();
}

void IRGenSILFunction::visitYieldInst(swift::YieldInst *i) {
  auto coroutineType = CurSILFn->getLoweredFunctionType();
  SILFunctionConventions coroConv(coroutineType, getSILModule());

  GenericContextScope scope(IGM, coroutineType->getInvocationGenericSignature());

  // Collect all the yielded values.
  Explosion values;
  auto yieldedValues = i->getYieldedValues();
  auto yields = coroutineType->getYields();
  assert(yieldedValues.size() == yields.size());
  for (auto idx : indices(yieldedValues)) {
    SILValue value = yieldedValues[idx];
    SILParameterInfo yield = yields[idx];
    emitApplyArgument(
        *this, value,
        coroConv.getSILType(yield, IGM.getMaximalTypeExpansionContext()),
        values);
  }

  // Emit the yield intrinsic.
  auto isUnwind = emitYield(*this, coroutineType, values);

  // Branch to the appropriate destination.
  auto unwindBB = getLoweredBB(i->getUnwindBB()).bb;
  auto resumeBB = getLoweredBB(i->getResumeBB()).bb;
  Builder.CreateCondBr(isUnwind, unwindBB, resumeBB);
}

void IRGenSILFunction::visitBeginApplyInst(BeginApplyInst *i) {
  visitFullApplySite(i);
}

void IRGenSILFunction::visitEndApplyInst(EndApplyInst *i) {
  visitEndApply(i->getBeginApply(), false);
}

void IRGenSILFunction::visitAbortApplyInst(AbortApplyInst *i) {
  visitEndApply(i->getBeginApply(), true);
}

void IRGenSILFunction::visitEndApply(BeginApplyInst *i, bool isAbort) {
  const auto &coroutine = getLoweredCoroutine(i->getTokenResult());

  auto sig = Signature::forCoroutineContinuation(IGM, i->getOrigCalleeType());

  // Cast the continuation pointer to the right function pointer type.
  auto continuation = coroutine.Continuation;
  continuation = Builder.CreateBitCast(continuation,
                                       sig.getType()->getPointerTo());


  auto schemaAndEntity =
    getCoroutineResumeFunctionPointerAuth(IGM, i->getOrigCalleeType());
  auto pointerAuth = PointerAuthInfo::emit(*this, schemaAndEntity.first,
                                           coroutine.Buffer.getAddress(),
                                           schemaAndEntity.second);
  auto callee = FunctionPointer::createSigned(i->getOrigCalleeType(),
                                              continuation, pointerAuth, sig);

  Builder.CreateCall(callee, {
    coroutine.Buffer.getAddress(),
    llvm::ConstantInt::get(IGM.Int1Ty, isAbort)
  });

  coroutine.Temporaries.destroyAll(*this);

  emitDeallocYieldOnceCoroutineBuffer(*this, coroutine.Buffer);
}

static llvm::BasicBlock *emitBBMapForSwitchValue(
        IRGenSILFunction &IGF,
        SmallVectorImpl<std::pair<SILValue, llvm::BasicBlock*>> &dests,
        SwitchValueInst *inst) {
  for (unsigned i = 0, e = inst->getNumCases(); i < e; ++i) {
    auto casePair = inst->getCase(i);
    dests.push_back({casePair.first, IGF.getLoweredBB(casePair.second).bb});
  }

  llvm::BasicBlock *defaultDest = nullptr;
  if (inst->hasDefault())
    defaultDest = IGF.getLoweredBB(inst->getDefaultBB()).bb;
  return defaultDest;
}

static llvm::ConstantInt *
getSwitchCaseValue(IRGenFunction &IGF, SILValue val) {
  auto *IL = cast<IntegerLiteralInst>(val);
  return cast<llvm::ConstantInt>(emitConstantInt(IGF.IGM, IL));
}

static void
emitSwitchValueDispatch(IRGenSILFunction &IGF,
                        SILType ty,
                        Explosion &value,
                        ArrayRef<std::pair<SILValue, llvm::BasicBlock*>> dests,
                        llvm::BasicBlock *defaultDest) {
  // Create an unreachable block for the default if the original SIL
  // instruction had none.
  bool unreachableDefault = false;
  if (!defaultDest) {
    unreachableDefault = true;
    defaultDest = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
  }

  if (ty.is<BuiltinIntegerType>()) {
    auto *discriminator = value.claimNext();
    auto *i = IGF.Builder.CreateSwitch(discriminator, defaultDest,
                                     dests.size());
    for (auto &dest : dests)
      i->addCase(getSwitchCaseValue(IGF, dest.first), dest.second);
  } else {
    // Get the value we're testing, which is a function.
    llvm::Value *val;
    llvm::BasicBlock *nextTest = nullptr;
    if (ty.is<SILFunctionType>()) {
      val = value.claimNext();   // Function pointer.
      //values.claimNext();         // Ignore the data pointer.
    } else {
      llvm_unreachable("switch_value operand has an unknown type");
    }

    for (int i = 0, e = dests.size(); i < e; ++i) {
      auto casePair = dests[i];
      llvm::Value *caseval;
      auto casevalue = IGF.getLoweredExplosion(casePair.first);
      if (casePair.first->getType().is<SILFunctionType>()) {
        caseval = casevalue.claimNext();   // Function pointer.
        //values.claimNext();         // Ignore the data pointer.
      } else {
        llvm_unreachable("switch_value operand has an unknown type");
      }

      // Compare operand with a case tag value.
      llvm::Value *cond = IGF.Builder.CreateICmp(llvm::CmpInst::ICMP_EQ,
                                                 val, caseval);

      if (i == e -1 && !unreachableDefault) {
        nextTest = nullptr;
        IGF.Builder.CreateCondBr(cond, casePair.second, defaultDest);
      } else {
        nextTest = IGF.createBasicBlock("next-test");
        IGF.Builder.CreateCondBr(cond, casePair.second, nextTest);
        IGF.Builder.emitBlock(nextTest);
        IGF.Builder.SetInsertPoint(nextTest);
      }
    }

    if (nextTest) {
      IGF.Builder.CreateBr(defaultDest);
    }
  }

  if (unreachableDefault) {
    IGF.Builder.emitBlock(defaultDest);
    IGF.Builder.CreateUnreachable();
  }
}

void IRGenSILFunction::visitSwitchValueInst(SwitchValueInst *inst) {
  Explosion value = getLoweredExplosion(inst->getOperand());

  // Map the SIL dest bbs to their LLVM bbs.
  SmallVector<std::pair<SILValue, llvm::BasicBlock*>, 4> dests;
  auto *defaultDest = emitBBMapForSwitchValue(*this, dests, inst);

  emitSwitchValueDispatch(*this, inst->getOperand()->getType(),
                                  value, dests, defaultDest);
}

// Bind an incoming explosion value to an explosion of LLVM phi node(s).
static void addIncomingExplosionToPHINodes(IRGenSILFunction &IGF,
                                           ArrayRef<llvm::Value*> phis,
                                           Explosion &argValue) {
  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  unsigned phiIndex = 0;
  while (!argValue.empty())
    cast<llvm::PHINode>(phis[phiIndex++])
      ->addIncoming(argValue.claimNext(), curBB);
  assert(phiIndex == phis.size() && "explosion doesn't match number of phis");
}

// Bind an incoming explosion value to a SILArgument's LLVM phi node(s).
static void addIncomingExplosionToPHINodes(IRGenSILFunction &IGF,
                                           LoweredBB &lbb,
                                           unsigned &phiIndex,
                                           Explosion &argValue) {
  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  while (!argValue.empty())
    lbb.phis[phiIndex++]->addIncoming(argValue.claimNext(), curBB);
}

// Bind an incoming address value to a SILArgument's LLVM phi node(s).
static void addIncomingAddressToPHINodes(IRGenSILFunction &IGF,
                                         ArrayRef<llvm::Value*> phis,
                                         Address argValue) {
  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  assert(phis.size() == 1 && "more than one phi for address?!");
  cast<llvm::PHINode>(phis[0])->addIncoming(argValue.getAddress(), curBB);
}

// Bind an incoming address value to a SILArgument's LLVM phi node(s).
static void addIncomingAddressToPHINodes(IRGenSILFunction &IGF,
                                         LoweredBB &lbb,
                                         unsigned &phiIndex,
                                         Address argValue) {
  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  lbb.phis[phiIndex++]->addIncoming(argValue.getAddress(), curBB);
}

// Add branch arguments to destination phi nodes.
static void addIncomingSILArgumentsToPHINodes(IRGenSILFunction &IGF,
                                              LoweredBB &lbb,
                                              OperandValueArrayRef args) {
  unsigned phiIndex = 0;
  for (SILValue arg : args) {
    if (arg->getType().isAddress()) {
      addIncomingAddressToPHINodes(IGF, lbb, phiIndex,
                                   IGF.getLoweredAddress(arg));
      continue;
    }
    
    Explosion argValue = IGF.getLoweredExplosion(arg);
    addIncomingExplosionToPHINodes(IGF, lbb, phiIndex, argValue);
  }
}

static llvm::BasicBlock *emitBBMapForSwitchEnum(
    IRGenSILFunction &IGF,
    SmallVectorImpl<std::pair<EnumElementDecl *, llvm::BasicBlock *>> &dests,
    SwitchEnumTermInst inst) {
  for (unsigned i = 0, e = inst.getNumCases(); i < e; ++i) {
    auto casePair = inst.getCase(i);

    // If the destination BB accepts the case argument, set up a waypoint BB so
    // we can feed the values into the argument's PHI node(s).
    //
    // FIXME: This is cheesy when the destination BB has only the switch
    // as a predecessor.
    if (!casePair.second->args_empty())
      dests.push_back({casePair.first,
        llvm::BasicBlock::Create(IGF.IGM.getLLVMContext())});
    else
      dests.push_back({casePair.first, IGF.getLoweredBB(casePair.second).bb});
  }

  llvm::BasicBlock *defaultDest = nullptr;
  if (inst.hasDefault())
    defaultDest = IGF.getLoweredBB(inst.getDefaultBB()).bb;
  return defaultDest;
}

void IRGenSILFunction::visitSwitchEnumInst(SwitchEnumInst *inst) {
  Explosion value = getLoweredExplosion(inst->getOperand());
  
  // Map the SIL dest bbs to their LLVM bbs.
  SmallVector<std::pair<EnumElementDecl*, llvm::BasicBlock*>, 4> dests;
  llvm::BasicBlock *defaultDest
    = emitBBMapForSwitchEnum(*this, dests, inst);
  
  // Emit the dispatch.
  auto &EIS = getEnumImplStrategy(IGM, inst->getOperand()->getType());
  EIS.emitValueSwitch(*this, value, dests, defaultDest);
  
  // Bind arguments for cases that want them.
  for (unsigned i = 0, e = inst->getNumCases(); i < e; ++i) {
    auto casePair = inst->getCase(i);

    if (!casePair.second->args_empty()) {
      auto waypointBB = dests[i].second;
      auto &destLBB = getLoweredBB(casePair.second);
      
      Builder.emitBlock(waypointBB);
      
      Explosion inValue = getLoweredExplosion(inst->getOperand());
      Explosion projected;
      emitProjectLoadableEnum(*this, inst->getOperand()->getType(),
                               inValue, casePair.first, projected);
      
      unsigned phiIndex = 0;
      addIncomingExplosionToPHINodes(*this, destLBB, phiIndex, projected);
      
      Builder.CreateBr(destLBB.bb);
    }
  }
}

void
IRGenSILFunction::visitSwitchEnumAddrInst(SwitchEnumAddrInst *inst) {
  Address value = getLoweredAddress(inst->getOperand());
  
  // Map the SIL dest bbs to their LLVM bbs.
  SmallVector<std::pair<EnumElementDecl*, llvm::BasicBlock*>, 4> dests;
  llvm::BasicBlock *defaultDest
    = emitBBMapForSwitchEnum(*this, dests, inst);
  
  // Emit the dispatch.
  emitSwitchAddressOnlyEnumDispatch(*this, inst->getOperand()->getType(),
                                     value, dests, defaultDest);
}

// FIXME: We could lower select_enum directly to LLVM select in a lot of cases.
// For now, just emit a switch and phi nodes, like a chump.
template <class C, class T, class B>
static llvm::BasicBlock *
emitBBMapForSelect(IRGenSILFunction &IGF, Explosion &resultPHI,
                   SmallVectorImpl<std::pair<T, llvm::BasicBlock *>> &BBs,
                   llvm::BasicBlock *&defaultBB,
                   SelectInstBase<C, T, B> *inst) {

  auto origBB = IGF.Builder.GetInsertBlock();

  // Set up a continuation BB and phi nodes to receive the result value.
  llvm::BasicBlock *contBB = IGF.createBasicBlock("select_enum");
  IGF.Builder.SetInsertPoint(contBB);

  // Emit an explosion of phi node(s) to receive the value.
  SmallVector<llvm::Value*, 4> phis;
  auto &ti = IGF.getTypeInfo(inst->getType());
  emitPHINodesForType(IGF, inst->getType(), ti,
                      inst->getNumCases() + inst->hasDefault(),
                      phis);
  resultPHI.add(phis);
  
  IGF.Builder.SetInsertPoint(origBB);
  
  auto addIncoming = [&](SILValue value) {
    if (value->getType().isAddress()) {
      addIncomingAddressToPHINodes(IGF, resultPHI.getAll(),
                                   IGF.getLoweredAddress(value));
    } else {
      Explosion ex = IGF.getLoweredExplosion(value);
      addIncomingExplosionToPHINodes(IGF, resultPHI.getAll(), ex);
    }
  };
  
  for (unsigned i = 0, e = inst->getNumCases(); i < e; ++i) {
    auto casePair = inst->getCase(i);
    
    // Create a basic block destination for this case.
    llvm::BasicBlock *destBB = IGF.createBasicBlock("");
    IGF.Builder.emitBlock(destBB);
    
    // Feed the corresponding result into the phi nodes.
    addIncoming(casePair.second);

    // Jump immediately to the continuation.
    IGF.Builder.CreateBr(contBB);
    BBs.push_back(std::make_pair(casePair.first, destBB));
  }
  
  if (inst->hasDefault()) {
    defaultBB = IGF.createBasicBlock("");
    IGF.Builder.emitBlock(defaultBB);
    
    addIncoming(inst->getDefaultResult());
    
    IGF.Builder.CreateBr(contBB);
  } else {
    defaultBB = nullptr;
  }
  
  IGF.Builder.emitBlock(contBB);
  
  IGF.Builder.SetInsertPoint(origBB);
  return contBB;
}

// Try to map the value of a select_enum directly to an int type with a simple
// cast from the tag value to the result type. Optionally also by adding a
// constant offset.
// This is useful, e.g. for rawValue or hashValue of C-like enums.
static llvm::Value *
mapTriviallyToInt(IRGenSILFunction &IGF, const EnumImplStrategy &EIS, SelectEnumInst *inst) {

  // All cases must be covered
  if (inst->hasDefault())
    return nullptr;
  
  auto &ti = IGF.getTypeInfo(inst->getType());
  ExplosionSchema schema = ti.getSchema();

  // Check if the select_enum's result is a single integer scalar.
  if (schema.size() != 1)
    return nullptr;
  
  if (!schema[0].isScalar())
    return nullptr;
  
  llvm::Type *type = schema[0].getScalarType();
  auto *resultType = dyn_cast<llvm::IntegerType>(type);
  if (!resultType)
    return nullptr;
  
  // Check if the case values directly map to the tag values, maybe with a
  // constant offset.
  APInt commonOffset;
  bool offsetValid = false;

  for (unsigned i = 0, e = inst->getNumCases(); i < e; ++i) {
    auto casePair = inst->getCase(i);

    int64_t index = EIS.getDiscriminatorIndex(casePair.first);
    if (index < 0)
      return nullptr;
    
    auto *intLit = dyn_cast<IntegerLiteralInst>(casePair.second);
    if (!intLit)
      return nullptr;
    
    APInt caseValue = intLit->getValue();
    APInt offset = caseValue - index;
    if (offsetValid) {
      if (offset != commonOffset)
        return nullptr;
    } else {
      commonOffset = offset;
      offsetValid = true;
    }
  }
  
  // Ask the enum implementation strategy to extract the enum tag as an integer
  // value.
  Explosion enumValue = IGF.getLoweredExplosion(inst->getEnumOperand());
  llvm::Value *result = EIS.emitExtractDiscriminator(IGF, enumValue);
  if (!result) {
    (void)enumValue.claimAll();
    return nullptr;
  }

  // Cast to the result type.
  result = IGF.Builder.CreateIntCast(result, resultType, false);
  if (commonOffset != 0) {
    // The offset, if any.
    auto *offsetConst = llvm::ConstantInt::get(resultType, commonOffset);
    result = IGF.Builder.CreateAdd(result, offsetConst);
  }
  return result;
}

template <class C, class T, class B>
static LoweredValue getLoweredValueForSelect(IRGenSILFunction &IGF,
                                             Explosion &result,
                                             SelectInstBase<C, T, B> *inst) {
  if (inst->getType().isAddress())
    // FIXME: Loses potentially better alignment info we might have.
    return LoweredValue(Address(
        result.claimNext(), IGF.getTypeInfo(inst->getType()).getStorageType(),
        IGF.getTypeInfo(inst->getType()).getBestKnownAlignment()));
  return LoweredValue(result);
}

static void emitSingleEnumMemberSelectResult(IRGenSILFunction &IGF,
                                             SelectEnumInstBase *inst,
                                             llvm::Value *isTrue,
                                             Explosion &result) {
  assert((inst->getNumCases() == 1 && inst->hasDefault()) ||
         (inst->getNumCases() == 2 && !inst->hasDefault()));
  
  // Extract the true values.
  auto trueValue = inst->getCase(0).second;
  SmallVector<llvm::Value*, 4> TrueValues;
  if (trueValue->getType().isAddress()) {
    TrueValues.push_back(IGF.getLoweredAddress(trueValue).getAddress());
  } else {
    Explosion ex = IGF.getLoweredExplosion(trueValue);
    while (!ex.empty())
      TrueValues.push_back(ex.claimNext());
  }
    
  // Extract the false values.
  auto falseValue =
    inst->hasDefault() ? inst->getDefaultResult() : inst->getCase(1).second;
  SmallVector<llvm::Value*, 4> FalseValues;
  if (falseValue->getType().isAddress()) {
    FalseValues.push_back(IGF.getLoweredAddress(falseValue).getAddress());
  } else {
    Explosion ex = IGF.getLoweredExplosion(falseValue);
    while (!ex.empty())
      FalseValues.push_back(ex.claimNext());
  }
  
  assert(TrueValues.size() == FalseValues.size() &&
         "explosions didn't produce same element count?");
  for (unsigned i = 0, e = FalseValues.size(); i != e; ++i) {
    auto *TV = TrueValues[i], *FV = FalseValues[i];
    // It is pretty common to select between zero and 1 as the result of the
    // select.  Instead of emitting an obviously dumb select, emit nothing or
    // a zext.
    if (auto *TC = dyn_cast<llvm::ConstantInt>(TV))
      if (auto *FC = dyn_cast<llvm::ConstantInt>(FV))
        if (TC->isOne() && FC->isZero()) {
          result.add(IGF.Builder.CreateZExtOrBitCast(isTrue, TV->getType()));
          continue;
        }
        
    result.add(IGF.Builder.CreateSelect(isTrue, TV, FalseValues[i]));
  }
}


void IRGenSILFunction::visitSelectEnumInst(SelectEnumInst *inst) {
  auto &EIS = getEnumImplStrategy(IGM, inst->getEnumOperand()->getType());
  Explosion result;

  if (llvm::Value *R = mapTriviallyToInt(*this, EIS, inst)) {
    result.add(R);
  } else if ((inst->getNumCases() == 1 && inst->hasDefault()) ||
             (inst->getNumCases() == 2 && !inst->hasDefault())) {
    // If this is testing for one case, do simpler codegen.  This is
    // particularly common when testing optionals.
    Explosion value = getLoweredExplosion(inst->getEnumOperand());
    auto isTrue = EIS.emitValueCaseTest(*this, value, inst->getCase(0).first);
    emitSingleEnumMemberSelectResult(*this, inst, isTrue, result);
  } else {
    Explosion value = getLoweredExplosion(inst->getEnumOperand());

    // Map the SIL dest bbs to their LLVM bbs.
    SmallVector<std::pair<EnumElementDecl*, llvm::BasicBlock*>, 4> dests;
    llvm::BasicBlock *defaultDest;
    llvm::BasicBlock *contBB
      = emitBBMapForSelect(*this, result, dests, defaultDest, inst);
    
    // Emit the dispatch.
    EIS.emitValueSwitch(*this, value, dests, defaultDest);

    // emitBBMapForSelectEnum set up a continuation block and phi nodes to
    // receive the result.
    Builder.SetInsertPoint(contBB);
  }
  setLoweredValue(inst,
                  getLoweredValueForSelect(*this, result, inst));
}

void IRGenSILFunction::visitSelectEnumAddrInst(SelectEnumAddrInst *inst) {
  Address value = getLoweredAddress(inst->getEnumOperand());
  Explosion result;

  if ((inst->getNumCases() == 1 && inst->hasDefault()) ||
      (inst->getNumCases() == 2 && !inst->hasDefault())) {
    auto &EIS = getEnumImplStrategy(IGM, inst->getEnumOperand()->getType());
    // If this is testing for one case, do simpler codegen.  This is
    // particularly common when testing optionals.
    auto isTrue = EIS.emitIndirectCaseTest(*this,
                                           inst->getEnumOperand()->getType(),
                                           value, inst->getCase(0).first);
    emitSingleEnumMemberSelectResult(*this, inst, isTrue, result);
  } else {
      // Map the SIL dest bbs to their LLVM bbs.
    SmallVector<std::pair<EnumElementDecl*, llvm::BasicBlock*>, 4> dests;
    llvm::BasicBlock *defaultDest;
    llvm::BasicBlock *contBB
      = emitBBMapForSelect(*this, result, dests, defaultDest, inst);
    
    // Emit the dispatch.
    emitSwitchAddressOnlyEnumDispatch(*this, inst->getEnumOperand()->getType(),
                                      value, dests, defaultDest);
    
    // emitBBMapForSelectEnum set up a phi node to receive the result.
    Builder.SetInsertPoint(contBB);
  }
  
  setLoweredValue(inst,
                  getLoweredValueForSelect(*this, result, inst));
}

void IRGenSILFunction::visitSelectValueInst(SelectValueInst *inst) {
  Explosion value = getLoweredExplosion(inst->getOperand());

  // Map the SIL dest bbs to their LLVM bbs.
  SmallVector<std::pair<SILValue, llvm::BasicBlock*>, 4> dests;
  llvm::BasicBlock *defaultDest;
  Explosion result;
  auto *contBB = emitBBMapForSelect(*this, result, dests, defaultDest, inst);

  // Emit the dispatch.
  emitSwitchValueDispatch(*this, inst->getOperand()->getType(), value, dests,
                          defaultDest);

  // emitBBMapForSelectEnum set up a continuation block and phi nodes to
  // receive the result.
  Builder.SetInsertPoint(contBB);

  setLoweredValue(inst,
                  getLoweredValueForSelect(*this, result, inst));
}

void IRGenSILFunction::visitDynamicMethodBranchInst(DynamicMethodBranchInst *i){
  LoweredBB &hasMethodBB = getLoweredBB(i->getHasMethodBB());
  LoweredBB &noMethodBB = getLoweredBB(i->getNoMethodBB());

  // Emit the respondsToSelector: call.
  StringRef selector;
  llvm::SmallString<64> selectorBuffer;
  if (auto fnDecl = dyn_cast<FuncDecl>(i->getMember().getDecl()))
    selector = fnDecl->getObjCSelector().getString(selectorBuffer);
  else if (auto var = dyn_cast<AbstractStorageDecl>(i->getMember().getDecl()))
    selector = var->getObjCGetterSelector().getString(selectorBuffer);
  else
    llvm_unreachable("Unhandled dynamic method branch query");

  llvm::Value *object = getLoweredExplosion(i->getOperand()).claimNext();
  if (object->getType() != IGM.ObjCPtrTy)
    object = Builder.CreateBitCast(object, IGM.ObjCPtrTy);
  llvm::Value *loadSel = emitObjCSelectorRefLoad(selector);
  
  llvm::Value *respondsToSelector
    = emitObjCSelectorRefLoad("respondsToSelector:");
  
  llvm::Constant *messenger = IGM.getObjCMsgSendFn();
  llvm::Type *argTys[] = {
    IGM.ObjCPtrTy,
    IGM.Int8PtrTy,
    IGM.Int8PtrTy,
  };
  auto respondsToSelectorTy = llvm::FunctionType::get(IGM.Int1Ty, argTys,
                                                      /*isVarArg*/ false);
  messenger = llvm::ConstantExpr::getBitCast(
      messenger, respondsToSelectorTy->getPointerTo());
  llvm::CallInst *call = Builder.CreateCall(
      respondsToSelectorTy, messenger, {object, respondsToSelector, loadSel});
  call->setDoesNotThrow();

  // FIXME: Assume (probably safely) that the hasMethodBB has only us as a
  // predecessor, and cannibalize its bb argument so we can represent is as an
  // ObjCMethod lowered value. This is hella gross but saves us having to
  // implement ObjCMethod-to-Explosion lowering and creating a thunk we don't
  // want.
  assert(std::next(i->getHasMethodBB()->pred_begin())
           == i->getHasMethodBB()->pred_end()
         && "lowering dynamic_method_br with multiple preds for destination "
            "not implemented");
  // Kill the existing lowered value for the bb arg and its phi nodes.
  SILValue methodArg = i->getHasMethodBB()->args_begin()[0];
  Explosion formerLLArg = getLoweredExplosion(methodArg);
  for (llvm::Value *val : formerLLArg.claimAll()) {
    auto phi = cast<llvm::PHINode>(val);
    assert(phi->getNumIncomingValues() == 0 && "phi already used");
    phi->removeFromParent();
    delete phi;
  }
  LoweredValues.erase(methodArg);
  
  // Replace the lowered value with an ObjCMethod lowering.
  setLoweredObjCMethod(methodArg, i->getMember());
  
  // Create the branch.
  Builder.CreateCondBr(call, hasMethodBB.bb, noMethodBB.bb);
}

void IRGenSILFunction::visitBranchInst(swift::BranchInst *i) {
  LoweredBB &lbb = getLoweredBB(i->getDestBB());
  addIncomingSILArgumentsToPHINodes(*this, lbb, i->getArgs());
  Builder.CreateBr(lbb.bb);
}

void IRGenSILFunction::visitCondBranchInst(swift::CondBranchInst *i) {
  LoweredBB &trueBB = getLoweredBB(i->getTrueBB());
  LoweredBB &falseBB = getLoweredBB(i->getFalseBB());
  llvm::Value *condValue =
    getLoweredExplosion(i->getCondition()).claimNext();

  addIncomingSILArgumentsToPHINodes(*this, trueBB, i->getTrueArgs());
  addIncomingSILArgumentsToPHINodes(*this, falseBB, i->getFalseArgs());

  llvm::MDNode *Weights = nullptr;
  auto TrueBBCount = i->getTrueBBCount();
  auto FalseBBCount = i->getFalseBBCount();
  if (TrueBBCount || FalseBBCount)
    Weights = IGM.createProfileWeights(TrueBBCount ? TrueBBCount.getValue() : 0,
        FalseBBCount ? FalseBBCount.getValue() : 0);

  Builder.CreateCondBr(condValue, trueBB.bb, falseBB.bb, Weights);
}

void IRGenSILFunction::visitRetainValueInst(swift::RetainValueInst *i) {
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;
  cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType()))
      .copy(*this, in, out, i->isAtomic() ? irgen::Atomicity::Atomic
                                          : irgen::Atomicity::NonAtomic);
  (void)out.claimAll();
}

void IRGenSILFunction::visitRetainValueAddrInst(swift::RetainValueAddrInst *i) {
  SILValue operandValue = i->getOperand();
  Address addr = getLoweredAddress(operandValue);
  SILType addrTy = operandValue->getType();
  SILType objectT = addrTy.getObjectType();
  llvm::Type *llvmType = addr.getAddress()->getType();
  const TypeInfo &addrTI = getTypeInfo(addrTy);
  auto atomicity = i->isAtomic() ? Atomicity::Atomic : Atomicity::NonAtomic;
  auto *outlinedF = cast<llvm::Function>(
      IGM.getOrCreateRetainFunction(addrTI, objectT, llvmType, atomicity));
  llvm::Value *args[] = {addr.getAddress()};
  llvm::CallInst *call =
      Builder.CreateCall(outlinedF->getFunctionType(), outlinedF, args);
  call->setCallingConv(IGM.DefaultCC);
}

void IRGenSILFunction::visitCopyValueInst(swift::CopyValueInst *i) {
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;
  cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType()))
      .copy(*this, in, out, getDefaultAtomicity());
  setLoweredExplosion(i, out);
}

// TODO: Implement this more generally for arbitrary values. Currently the
// SIL verifier restricts it to single-refcounted-pointer types.
void IRGenSILFunction::visitAutoreleaseValueInst(swift::AutoreleaseValueInst *i)
{
  Explosion in = getLoweredExplosion(i->getOperand());
  auto val = in.claimNext();
  
  emitObjCAutoreleaseCall(val);
}

void IRGenSILFunction::visitSetDeallocatingInst(SetDeallocatingInst *i) {
  auto *ARI = dyn_cast<AllocRefInst>(i->getOperand());
  if (ARI && StackAllocs.count(ARI)) {
    // A small peep-hole optimization: If the operand is allocated on stack and
    // there is no "significant" code between the set_deallocating and the final
    // dealloc_ref, the set_deallocating is not required.
    //   %0 = alloc_ref [stack]
    //     ...
    //   set_deallocating %0 // not needed
    //     // code which does not depend on the RC_DEALLOCATING_FLAG flag.
    //   dealloc_ref %0      // stems from the inlined deallocator
    //     ...
    //   dealloc_stack_ref %0
    SILBasicBlock::iterator Iter(i);
    SILBasicBlock::iterator End = i->getParent()->end();
    for (++Iter; Iter != End; ++Iter) {
      SILInstruction *I = &*Iter;
      if (auto *DRI = dyn_cast<DeallocRefInst>(I)) {
        if (DRI->getOperand() == ARI) {
          // The set_deallocating is followed by a dealloc_ref -> we can ignore
          // it.
          return;
        }
      }
      // Assume that any instruction with side-effects may depend on the
      // RC_DEALLOCATING_FLAG flag.
      if (I->mayHaveSideEffects())
        break;
    }
  }
  Explosion lowered = getLoweredExplosion(i->getOperand());
  emitNativeSetDeallocating(lowered.claimNext());
}

void IRGenSILFunction::visitReleaseValueInst(swift::ReleaseValueInst *i) {
  auto operand = i->getOperand();
  auto ty = operand->getType();
  Explosion in = getLoweredExplosion(operand);
  cast<LoadableTypeInfo>(getTypeInfo(ty))
      .consume(*this, in, i->isAtomic() ? irgen::Atomicity::Atomic
                                        : irgen::Atomicity::NonAtomic,
               ty);
}

void IRGenSILFunction::visitReleaseValueAddrInst(
    swift::ReleaseValueAddrInst *i) {
  SILValue operandValue = i->getOperand();
  Address addr = getLoweredAddress(operandValue);
  SILType addrTy = operandValue->getType();
  SILType objectT = addrTy.getObjectType();
  llvm::Type *llvmType = addr.getAddress()->getType();
  const TypeInfo &addrTI = getTypeInfo(addrTy);
  auto atomicity = i->isAtomic() ? Atomicity::Atomic : Atomicity::NonAtomic;
  auto *outlinedF = cast<llvm::Function>(
      IGM.getOrCreateReleaseFunction(addrTI, objectT, llvmType, atomicity));
  llvm::Value *args[] = {addr.getAddress()};
  llvm::CallInst *call =
      Builder.CreateCall(outlinedF->getFunctionType(), outlinedF, args);
  call->setCallingConv(IGM.DefaultCC);
}

void IRGenSILFunction::visitDestroyValueInst(swift::DestroyValueInst *i) {
  auto operand = i->getOperand();
  auto ty = operand->getType();
  Explosion in = getLoweredExplosion(operand);
  cast<LoadableTypeInfo>(getTypeInfo(ty))
      .consume(*this, in, getDefaultAtomicity(), ty);
}

void IRGenSILFunction::visitStructInst(swift::StructInst *i) {
  Explosion out;
  for (SILValue elt : i->getElements())
    out.add(getLoweredExplosion(elt).claimAll());
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitTupleInst(swift::TupleInst *i) {
  Explosion out;
  for (SILValue elt : i->getElements())
    out.add(getLoweredExplosion(elt).claimAll());
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitEnumInst(swift::EnumInst *i) {
  Explosion data = (i->hasOperand())
    ? getLoweredExplosion(i->getOperand())
    : Explosion();
  Explosion out;
  emitInjectLoadableEnum(*this, i->getType(), i->getElement(), data, out);
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitInitEnumDataAddrInst(swift::InitEnumDataAddrInst *i) {
  Address enumAddr = getLoweredAddress(i->getOperand());
  Address dataAddr = emitProjectEnumAddressForStore(*this,
                                                     i->getOperand()->getType(),
                                                     enumAddr,
                                                     i->getElement());
  setLoweredAddress(i, dataAddr);
}

void IRGenSILFunction::visitUncheckedEnumDataInst(swift::UncheckedEnumDataInst *i) {
  Explosion enumVal = getLoweredExplosion(i->getOperand());
  Explosion data;
  emitProjectLoadableEnum(*this, i->getOperand()->getType(),
                          enumVal, i->getElement(), data);
  setLoweredExplosion(i, data);
}

void IRGenSILFunction::visitUncheckedTakeEnumDataAddrInst(swift::UncheckedTakeEnumDataAddrInst *i) {
  Address enumAddr = getLoweredAddress(i->getOperand());
  Address dataAddr = emitDestructiveProjectEnumAddressForLoad(*this,
                                                    i->getOperand()->getType(),
                                                    enumAddr,
                                                    i->getElement());
  setLoweredAddress(i, dataAddr);
}

void IRGenSILFunction::visitInjectEnumAddrInst(swift::InjectEnumAddrInst *i) {
  Address enumAddr = getLoweredAddress(i->getOperand());
  emitStoreEnumTagToAddress(*this, i->getOperand()->getType(),
                             enumAddr, i->getElement());
}

void IRGenSILFunction::visitTupleExtractInst(swift::TupleExtractInst *i) {
  Explosion fullTuple = getLoweredExplosion(i->getOperand());
  Explosion output;
  SILType baseType = i->getOperand()->getType();
  
  projectTupleElementFromExplosion(*this,
                                   baseType,
                                   fullTuple,
                                   i->getFieldIndex(),
                                   output);
  (void)fullTuple.claimAll();
  setLoweredExplosion(i, output);
}

void IRGenSILFunction::visitTupleElementAddrInst(swift::TupleElementAddrInst *i)
{
  Address base = getLoweredAddress(i->getOperand());
  SILType baseType = i->getOperand()->getType();

  Address field = projectTupleElementAddress(*this, base, baseType,
                                             i->getFieldIndex());
  setLoweredAddress(i, field);
}

void IRGenSILFunction::visitStructExtractInst(swift::StructExtractInst *i) {
  Explosion operand = getLoweredExplosion(i->getOperand());
  Explosion lowered;
  SILType baseType = i->getOperand()->getType();
  
  projectPhysicalStructMemberFromExplosion(*this,
                                           baseType,
                                           operand,
                                           i->getField(),
                                           lowered);

  (void)operand.claimAll();
  setLoweredExplosion(i, lowered);
}

void IRGenSILFunction::visitStructElementAddrInst(
                                              swift::StructElementAddrInst *i) {
  Address base = getLoweredAddress(i->getOperand());
  SILType baseType = i->getOperand()->getType();

  Address field = projectPhysicalStructMemberAddress(*this, base, baseType,
                                                     i->getField());
  setLoweredAddress(i, field);
}

void IRGenSILFunction::visitRefElementAddrInst(swift::RefElementAddrInst *i) {
  Explosion base = getLoweredExplosion(i->getOperand());
  llvm::Value *value = base.claimNext();

  SILType baseTy = i->getOperand()->getType();
  auto fnSig = CurSILFn->getGenericSignature();
  Address field = projectPhysicalClassMemberAddress(*this, value, baseTy,
                                                    i->getType(), i->getField(),
                                                    fnSig)
                      .getAddress();
  setLoweredAddress(i, field);
}

void IRGenSILFunction::visitRefTailAddrInst(RefTailAddrInst *i) {
  SILValue Ref = i->getOperand();
  llvm::Value *RefValue = getLoweredExplosion(Ref).claimNext();

  Address TailAddr = emitTailProjection(*this, RefValue, Ref->getType(),
                                        i->getTailType(),
                                        CurSILFn->getGenericSignature());
  setLoweredAddress(i, TailAddr);
}

static bool isInvariantAddress(SILValue v) {
  SILValue accessedAddress = getTypedAccessAddress(v);
  if (auto *ptrRoot = dyn_cast<PointerToAddressInst>(accessedAddress)) {
    return ptrRoot->isInvariant();
  }
  // TODO: We could be more aggressive about considering addresses based on
  // `let` variables as invariant when the type of the address is known not to
  // have any shareably-mutable interior storage (in other words, no weak refs,
  // atomics, etc.). However, this currently miscompiles some programs.
  // if (accessedAddress->getType().isAddress() && isLetAddress(accessedAddress)) {
  //  return true;
  // }
  return false;
}

void IRGenSILFunction::visitLoadInst(swift::LoadInst *i) {
  Explosion lowered;
  Address source = getLoweredAddress(i->getOperand());
  SILType objType = i->getType().getObjectType();
  const auto &typeInfo = cast<LoadableTypeInfo>(getTypeInfo(objType));

  switch (i->getOwnershipQualifier()) {
  case LoadOwnershipQualifier::Unqualified:
  case LoadOwnershipQualifier::Trivial:
  case LoadOwnershipQualifier::Take:
    typeInfo.loadAsTake(*this, source, lowered);
    break;
  case LoadOwnershipQualifier::Copy:
    typeInfo.loadAsCopy(*this, source, lowered);
    break;
  }
  
  if (isInvariantAddress(i->getOperand())) {
    // It'd be better to push this down into `loadAs` methods, perhaps...
    for (auto value : lowered.getAll())
      if (auto load = dyn_cast<llvm::LoadInst>(value))
        setInvariantLoad(load);
  }
  setLoweredExplosion(i, lowered);
}

void IRGenSILFunction::visitStoreInst(swift::StoreInst *i) {
  Explosion source = getLoweredExplosion(i->getSrc());
  Address dest = getLoweredAddress(i->getDest());
  SILType objType = i->getSrc()->getType().getObjectType();

  const auto &typeInfo = cast<LoadableTypeInfo>(getTypeInfo(objType));
  switch (i->getOwnershipQualifier()) {
  case StoreOwnershipQualifier::Unqualified:
  case StoreOwnershipQualifier::Init:
  case StoreOwnershipQualifier::Trivial:
    typeInfo.initialize(*this, source, dest, false);
    break;
  case StoreOwnershipQualifier::Assign:
    typeInfo.assign(*this, source, dest, false, objType);
    break;
  }
}

/// Emit the artificial error result argument.
void IRGenSILFunction::emitErrorResultVar(CanSILFunctionType FnTy,
                                          SILResultInfo ErrorInfo,
                                          DebugValueInst *DbgValue) {
  // We don't need a shadow error variable for debugging on ABI's that return
  // swifterror in a register.
  if (IGM.ShouldUseSwiftError)
    return;
  auto ErrorResultSlot = getCalleeErrorResultSlot(IGM.silConv.getSILType(
      ErrorInfo, FnTy, IGM.getMaximalTypeExpansionContext()));
  auto Var = DbgValue->getVarInfo();
  assert(Var && "error result without debug info");
  auto Storage =
      emitShadowCopyIfNeeded(ErrorResultSlot.getAddress(), getDebugScope(),
                             *Var, false, false /*was move*/);
  if (!IGM.DebugInfo)
    return;
  auto DbgTy = DebugTypeInfo::getErrorResult(
      ErrorInfo.getReturnValueType(IGM.getSILModule(), FnTy,
                                   IGM.getMaximalTypeExpansionContext()),IGM);
  IGM.DebugInfo->emitVariableDeclaration(Builder, Storage, DbgTy,
                                         getDebugScope(), {}, *Var,
                                         IndirectValue, ArtificialValue);
}

void IRGenSILFunction::emitPoisonDebugValueInst(DebugValueInst *i) {
  auto varInfo = i->getVarInfo();
  assert(varInfo && "debug_value without debug info");

  bool isAnonymous = false;
  varInfo->Name = getVarName(i, isAnonymous);

  SILValue silVal = i->getOperand();
  SILType silTy = silVal->getType();
  SILType unwrappedTy = silTy.unwrapOptionalType();
  CanType refTy = unwrappedTy.getASTType();
  // TODO: Handling nontrivial aggregates requires implementing poisonRefs
  // within TypeInfo. However, this could inflate code size for large types.
  assert(refTy->isAnyClassReferenceType() && "type can't handle poison");

  Explosion e = getLoweredExplosion(silVal);
  llvm::Value *storage = e.claimNext();
  auto storageTy = storage->getType();
  // Safeguard: don't try to poison an non-word sized value. Not sure how this
  // could ever happen.
  if (!storageTy->isPointerTy() && storageTy != IGM.SizeTy)
    return;

  // Only the first word of the value is poisoned.
  //
  // TODO: This assumes that only class references are poisoned (as guaranteed
  // by MandatoryCopyPropagation). And it assumes the reference is the first
  // value (class existential witnesses are laid out after the class reference).
  bool singleValueExplosion = e.empty();
  (void)e.claimAll();

  // Only poison shadow references if this storage is purely used as a shadow
  // copy--poison should never affect program behavior. Also filter everything
  // not handled by emitShadowCopyIfNeeded to avoid extra shadow copies.
  if (!shouldShadowVariable(*varInfo, isAnonymous)
      || !shouldShadowStorage(storage)) {
    return;
  }

  // The original decl scope.
  const SILDebugScope *scope = i->getDebugScope();

  // Shadow allocas are pointer aligned.
  auto ptrAlign = IGM.getPointerAlignment();

  // Emit or recover the unique shadow copy.
  //
  // FIXME: To limit perturbing original source, this follows the strange
  // emitShadowCopyIfNeeded logic that has separate paths for single-value
  // vs. multi-value explosions.
  Address shadowAddress;
  if (singleValueExplosion) {
    shadowAddress = emitShadowCopy(storage, scope, *varInfo, ptrAlign, false,
                                   false /*was moved*/);
  } else {
    assert(refTy->isClassExistentialType() && "unknown multi-value explosion");
    // FIXME: Handling Optional existentials requires TypeInfo
    // support. Otherwise we would need to assume the layout of the reference
    // and bitcast everything below to scalar integers.
    if (silTy != unwrappedTy)
      return;

    unsigned argNo = varInfo->ArgNo;
    auto &alloca = ShadowStackSlots[{argNo, {scope, varInfo->Name}}];
    if (!alloca.isValid()) {
      auto &lti = cast<LoadableTypeInfo>(IGM.getTypeInfo(silTy));
      alloca =
        lti.allocateStack(*this, silTy, varInfo->Name + ".debug").getAddress();
    }
    shadowAddress = emitClassExistentialValueAddress(*this, alloca, silTy);
  }
  Size::int_type poisonInt = IGM.TargetInfo.ReferencePoisonDebugValue;
  assert((poisonInt & IGM.TargetInfo.PointerSpareBits.asAPInt()) == 0);
  llvm::Value *poisonedVal = llvm::ConstantInt::get(IGM.SizeTy, poisonInt);

  // If the current value is nil (Optional's extra inhabitant), then don't
  // overwrite it with poison. This way, lldb will correctly display
  // Optional.None rather than telling the user that an object was
  // deinitialized, when there was no object to begin with. This could also be
  // done with a spare-bits mask to handle arbitrary enums but extra inhabitants
  // are tricky.
  if (!storageTy->isPointerTy()) {
    assert(storageTy == IGM.SizeTy && "can't handle non-word values");
    llvm::Value *currentBits =
      Builder.CreateBitOrPointerCast(storage, IGM.SizeTy);
    llvm::Value *zeroWord = llvm::ConstantInt::get(IGM.SizeTy, 0);
    llvm::Value *isNil = Builder.CreateICmpEQ(currentBits, zeroWord);
    poisonedVal = Builder.CreateSelect(isNil, currentBits, poisonedVal);
  }
  llvm::Value *newShadowVal =
    Builder.CreateBitOrPointerCast(poisonedVal, storageTy);

  assert(canAllocaStoreValue(shadowAddress, newShadowVal, *varInfo, scope) &&
         "shadow copy can't handle poison");
    
  // The poison stores have an artificial location within the original variable
  // declaration's scope.
  ArtificialLocation autoRestore(scope, IGM.DebugInfo.get(), Builder);
  Builder.CreateStore(newShadowVal, shadowAddress.getAddress(), ptrAlign);
}

/// Determine whether the debug-info-carrying instruction \c i belongs to an
/// async function and thus may get allocated in the coroutine context. These
/// variables need to be marked with the Coro flag, so LLVM's CoroSplit pass can
/// recognize them.
static bool InCoroContext(SILFunction &f, SILInstruction &i) {
  return f.isAsync() && !i.getDebugScope()->InlinedCallSite;
}

void IRGenSILFunction::visitDebugValueInst(DebugValueInst *i) {
  auto SILVal = i->getOperand();
  bool IsAddrVal = SILVal->getType().isAddress();
  if (i->poisonRefs()) {
    assert(!IsAddrVal &&
           "SIL values with address type should not have poison");
    emitPoisonDebugValueInst(i);
    return;
  }
  if (i->getDebugScope()->getInlinedFunction()->isTransparent())
    return;

  auto VarInfo = i->getVarInfo();
  assert(VarInfo && "debug_value without debug info");
  if (isa<SILUndef>(SILVal)) {
    // We cannot track the location of inlined error arguments because it has no
    // representation in SIL.
    if (!IsAddrVal &&
        !i->getDebugScope()->InlinedCallSite && VarInfo->Name == "$error") {
      auto funcTy = CurSILFn->getLoweredFunctionType();
      emitErrorResultVar(funcTy, funcTy->getErrorResult(), i);
    }

    // If we were not moved return early. If this SILUndef was moved, then we
    // need to let it through so we can ensure the debug info invalidated.
    if (!i->getUsesMoveableValueDebugInfo())
      return;
  }
  bool IsInCoro = InCoroContext(*CurSILFn, *i);

  bool IsAnonymous = false;
  VarInfo->Name = getVarName(i, IsAnonymous);
  DebugTypeInfo DbgTy;
  SILType SILTy;
  bool IsFragmentType = false;
  if (auto MaybeSILTy = VarInfo->Type) {
    // If there is auxiliary type info, use it
    SILTy = *MaybeSILTy;
  } else {
    SILTy = SILVal->getType();
    if (VarInfo->DIExpr)
      IsFragmentType = VarInfo->DIExpr.hasFragment();
  }

  auto RealTy = SILTy.getASTType();
  if (IsAddrVal && IsInCoro)
    if (auto *PBI = dyn_cast<ProjectBoxInst>(i->getOperand())) {
      // Usually debug info only ever describes the *result* of a projectBox
      // call. To allow the debugger to display a boxed parameter of an async
      // continuation object, however, the debug info can only describe the box
      // itself and thus also needs to emit a box type for it so the debugger
      // knows to call into Remote Mirrors to unbox the value.
      RealTy = PBI->getOperand()->getType().getASTType();
      assert(isa<SILBoxType>(RealTy));
    }

  // Figure out the debug variable type
  if (VarDecl *Decl = i->getDecl()) {
    DbgTy = DebugTypeInfo::getLocalVariable(Decl, RealTy, getTypeInfo(SILTy),
                                            IGM, IsFragmentType);
  } else if (!SILTy.hasArchetype() && !VarInfo->Name.empty()) {
    // Handle the cases that read from a SIL file
    DbgTy = DebugTypeInfo::getFromTypeInfo(RealTy, getTypeInfo(SILTy), IGM,
                                           IsFragmentType);
  } else
    return;

  // Since debug_value is expressing indirection explicitly via op_deref,
  // we're not using either IndirectValue or CoroIndirectValue here.
  IndirectionKind Indirection = IsInCoro? CoroDirectValue : DirectValue;

  // Put the value into a shadow-copy stack slot at -Onone.
  llvm::SmallVector<llvm::Value *, 8> Copy;
  if (IsAddrVal)
    Copy.emplace_back(emitShadowCopyIfNeeded(
        getLoweredAddress(SILVal).getAddress(), i->getDebugScope(), *VarInfo,
        IsAnonymous, i->getUsesMoveableValueDebugInfo()));
  else
    emitShadowCopyIfNeeded(SILVal, i->getDebugScope(), *VarInfo, IsAnonymous,
                           i->getUsesMoveableValueDebugInfo(), Copy);

  bindArchetypes(DbgTy.getType());
  if (!IGM.DebugInfo)
    return;

  emitDebugVariableDeclaration(
      Copy, DbgTy, SILTy, i->getDebugScope(), i->getLoc(), *VarInfo,
      Indirection, AddrDbgInstrKind(i->getUsesMoveableValueDebugInfo()));
}

void IRGenSILFunction::visitDebugStepInst(DebugStepInst *i) {
  // Unfortunately there is no LLVM-equivalent of a debug_step instruction.
  // Also LLVM doesn't provide a plain NOP instruction.
  // Therefore we have to solve this with inline assembly.
  // Strictly speaking, this is not architecture independent. But there are
  // probably few assembly languages which don't use "nop" for nop instructions.
  auto *AsmFnTy = llvm::FunctionType::get(IGM.VoidTy, {}, false);
  auto *InlineAsm = llvm::InlineAsm::get(AsmFnTy, "nop", "", true);
  Builder.CreateAsmCall(InlineAsm, {});
}

void IRGenSILFunction::visitFixLifetimeInst(swift::FixLifetimeInst *i) {
  if (i->getOperand()->getType().isAddress()) {
    // Just pass in the address to fix lifetime if we have one. We will not do
    // anything to it so nothing bad should happen.
    emitFixLifetime(getLoweredAddress(i->getOperand()).getAddress());
    return;
  }

  // Handle objects.
  Explosion in = getLoweredExplosion(i->getOperand());
  cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType()))
    .fixLifetime(*this, in);
}

void IRGenSILFunction::visitMarkDependenceInst(swift::MarkDependenceInst *i) {
  // Dependency-marking is purely for SIL.  Just forward the input as
  // the result.

  SILValue value = i->getValue();
  if (value->getType().isAddress()) {
    setLoweredAddress(i, getLoweredAddress(value));
  } else {
    Explosion temp = getLoweredExplosion(value);
    setLoweredExplosion(i, temp);
  }
}

void IRGenSILFunction::visitCopyBlockInst(CopyBlockInst *i) {
  Explosion lowered = getLoweredExplosion(i->getOperand());
  llvm::Value *copied = emitBlockCopyCall(lowered.claimNext());
  Explosion result;
  result.add(copied);
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitStrongRetainInst(swift::StrongRetainInst *i) {
  Explosion lowered = getLoweredExplosion(i->getOperand());
  auto &ti = cast<ReferenceTypeInfo>(getTypeInfo(i->getOperand()->getType()));
  ti.strongRetain(*this, lowered, i->isAtomic() ? irgen::Atomicity::Atomic
                                                : irgen::Atomicity::NonAtomic);
}

void IRGenSILFunction::visitStrongReleaseInst(swift::StrongReleaseInst *i) {
  Explosion lowered = getLoweredExplosion(i->getOperand());
  auto &ti = cast<ReferenceTypeInfo>(getTypeInfo(i->getOperand()->getType()));
  ti.strongRelease(*this, lowered, i->isAtomic() ? irgen::Atomicity::Atomic
                                                 : irgen::Atomicity::NonAtomic);
}

/// Given a SILType which is a ReferenceStorageType, return the type
/// info for the underlying reference type.
static const ReferenceTypeInfo &getReferentTypeInfo(IRGenFunction &IGF,
                                                    SILType silType) {
  auto type = silType.castTo<ReferenceStorageType>().getReferentType();
  if (auto ty = type->getOptionalObjectType())
    type = ty->getCanonicalType();
  return cast<ReferenceTypeInfo>(IGF.getTypeInfoForLowered(type));
}

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  void IRGenSILFunction::visitLoad##Name##Inst(swift::Load##Name##Inst *i) { \
    Address source = getLoweredAddress(i->getOperand()); \
    auto silTy = i->getOperand()->getType(); \
    auto ty = cast<Name##StorageType>(silTy.getASTType()); \
    auto isOptional = bool(ty.getReferentType()->getOptionalObjectType()); \
    auto &ti = getReferentTypeInfo(*this, silTy); \
    Explosion result; \
    if (i->isTake()) { \
      ti.name##TakeStrong(*this, source, result, isOptional); \
    } else { \
      ti.name##LoadStrong(*this, source, result, isOptional); \
    } \
    setLoweredExplosion(i, result); \
  } \
  void IRGenSILFunction::visitStore##Name##Inst(swift::Store##Name##Inst *i) { \
    Explosion source = getLoweredExplosion(i->getSrc()); \
    Address dest = getLoweredAddress(i->getDest()); \
    auto silTy = i->getDest()->getType(); \
    auto ty = cast<Name##StorageType>(silTy.getASTType()); \
    auto isOptional = bool(ty.getReferentType()->getOptionalObjectType()); \
    auto &ti = getReferentTypeInfo(*this, silTy); \
    if (i->isInitializationOfDest()) { \
      ti.name##Init(*this, source, dest, isOptional); \
    } else { \
      ti.name##Assign(*this, source, dest, isOptional); \
    } \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...)                   \
  void IRGenSILFunction::visitStrongRetain##Name##Inst(                        \
      swift::StrongRetain##Name##Inst *i) {                                    \
    Explosion lowered = getLoweredExplosion(i->getOperand());                  \
    auto &ti = getReferentTypeInfo(*this, i->getOperand()->getType());         \
    ti.strongRetain##Name(*this, lowered,                                      \
                          i->isAtomic() ? irgen::Atomicity::Atomic             \
                                        : irgen::Atomicity::NonAtomic);        \
  }                                                                            \
  void IRGenSILFunction::visit##Name##RetainInst(swift::Name##RetainInst *i) { \
    Explosion lowered = getLoweredExplosion(i->getOperand());                  \
    auto &ti = getReferentTypeInfo(*this, i->getOperand()->getType());         \
    ti.name##Retain(*this, lowered,                                            \
                    i->isAtomic() ? irgen::Atomicity::Atomic                   \
                                  : irgen::Atomicity::NonAtomic);              \
  }                                                                            \
  void IRGenSILFunction::visit##Name##ReleaseInst(                             \
      swift::Name##ReleaseInst *i) {                                           \
    Explosion lowered = getLoweredExplosion(i->getOperand());                  \
    auto &ti = getReferentTypeInfo(*this, i->getOperand()->getType());         \
    ti.name##Release(*this, lowered,                                           \
                     i->isAtomic() ? irgen::Atomicity::Atomic                  \
                                   : irgen::Atomicity::NonAtomic);             \
  }                                                                            \
  void IRGenSILFunction::visitStrongCopy##Name##ValueInst(                     \
      swift::StrongCopy##Name##ValueInst *i) {                                 \
    Explosion in = getLoweredExplosion(i->getOperand());                       \
    auto silTy = i->getOperand()->getType();                                   \
    auto ty = cast<Name##StorageType>(silTy.getASTType());                     \
    auto isOptional = bool(ty.getReferentType()->getOptionalObjectType());     \
    auto &ti = getReferentTypeInfo(*this, silTy);                              \
    ti.strongRetain##Name(*this, in, irgen::Atomicity::Atomic);                \
    /* Semantically we are just passing through the input parameter but as a   \
     */                                                                        \
    /* strong reference... at LLVM IR level these type differences don't */    \
    /* matter. So just set the lowered explosion appropriately. */             \
    Explosion output = getLoweredExplosion(i->getOperand());                   \
    if (isOptional) {                                                          \
      auto values = output.claimAll();                                         \
      output.reset();                                                          \
      for (auto value : values) {                                              \
        output.add(Builder.CreatePtrToInt(value, IGM.IntPtrTy));               \
      }                                                                        \
    }                                                                          \
    setLoweredExplosion(i, output);                                            \
  }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...")
#define UNCHECKED_REF_STORAGE(Name, name, ...)                                 \
  void IRGenSILFunction::visitStrongCopy##Name##ValueInst(                     \
      swift::StrongCopy##Name##ValueInst *i) {                                 \
    Explosion in = getLoweredExplosion(i->getOperand());                       \
    auto silTy = i->getOperand()->getType();                                   \
    auto ty = cast<Name##StorageType>(silTy.getASTType());                     \
    auto isOptional = bool(ty.getReferentType()->getOptionalObjectType());     \
    auto &ti = getReferentTypeInfo(*this, silTy);                              \
    /* Since we are unchecked, we just use strong retain here. We do not       \
     * perform any checks */                                                   \
    ti.strongRetain(*this, in, irgen::Atomicity::Atomic);                      \
    /* Semantically we are just passing through the input parameter but as a   \
     */                                                                        \
    /* strong reference... at LLVM IR level these type differences don't */    \
    /* matter. So just set the lowered explosion appropriately. */             \
    Explosion output = getLoweredExplosion(i->getOperand());                   \
    if (isOptional) {                                                          \
      auto values = output.claimAll();                                         \
      output.reset();                                                          \
      for (auto value : values) {                                              \
        output.add(Builder.CreatePtrToInt(value, IGM.IntPtrTy));               \
      }                                                                        \
    }                                                                          \
    setLoweredExplosion(i, output);                                            \
  }
#include "swift/AST/ReferenceStorage.def"
#undef COMMON_CHECKED_REF_STORAGE

static bool hasReferenceSemantics(IRGenSILFunction &IGF,
                                  SILType silType) {
  auto operType = silType.getASTType();
  auto valueType = operType->getOptionalObjectType();
  auto objType = valueType ? valueType : operType;
  return (objType->mayHaveSuperclass()
          || objType->isClassExistentialType()
          || objType->is<BuiltinNativeObjectType>()
          || objType->is<BuiltinBridgeObjectType>());
}

static llvm::Value *emitIsUnique(IRGenSILFunction &IGF, SILValue operand,
                                 SourceLoc loc) {
  if (!hasReferenceSemantics(IGF, operand->getType())) {
    IGF.emitTrap("isUnique called for a non-reference", /*EmitUnreachable=*/false);
    return llvm::UndefValue::get(IGF.IGM.Int1Ty);
  }

  auto &operTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(operand->getType()));
  LoadedRef ref =
    operTI.loadRefcountedPtr(IGF, loc, IGF.getLoweredAddress(operand));
  return
    IGF.emitIsUniqueCall(ref.getValue(), ref.getStyle(), loc, ref.isNonNull());
}

void IRGenSILFunction::visitIsUniqueInst(swift::IsUniqueInst *i) {
  llvm::Value *result = emitIsUnique(*this, i->getOperand(),
                                     i->getLoc().getSourceLoc());
  Explosion out;
  out.add(result);
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitBeginCOWMutationInst(BeginCOWMutationInst *i) {
  SILValue ref = i->getOperand();
  Explosion bufferEx = getLoweredExplosion(ref);
  llvm::Value *buffer = *bufferEx.begin();
  setLoweredExplosion(i->getBufferResult(), bufferEx);

  Explosion isUnique;
  if (hasReferenceSemantics(*this, ref->getType())) {
    if (i->getUniquenessResult()->use_empty()) {
      // No need to call isUnique if the result is not used.
      isUnique.add(llvm::UndefValue::get(IGM.Int1Ty));
    } else {
      ReferenceCounting style = cast<ReferenceTypeInfo>(
          getTypeInfo(ref->getType())).getReferenceCountingType();
      if (i->isNative())
        style = ReferenceCounting::Native;

      llvm::Value *castBuffer =
        Builder.CreateBitCast(buffer, IGM.getReferenceType(style));

      isUnique.add(emitIsUniqueCall(castBuffer, style, i->getLoc().getSourceLoc(),
                                    /*isNonNull*/ true));
    }
  } else {
    emitTrap("beginCOWMutation called for a non-reference",
             /*EmitUnreachable=*/false);
    isUnique.add(llvm::UndefValue::get(IGM.Int1Ty));
  }
  setLoweredExplosion(i->getUniquenessResult(), isUnique);
}

void IRGenSILFunction::visitEndCOWMutationInst(EndCOWMutationInst *i) {
  Explosion v = getLoweredExplosion(i->getOperand());
  setLoweredExplosion(i, v);
}

void IRGenSILFunction::visitIsEscapingClosureInst(
    swift::IsEscapingClosureInst *i) {
  // The closure operand is allowed to be an optional closure.
  auto operandType = i->getOperand()->getType();
  if (operandType.getOptionalObjectType())
    operandType = operandType.getOptionalObjectType();

  auto fnType = operandType.getAs<SILFunctionType>();
  assert(fnType->getExtInfo().hasContext() && "Must have a closure operand");
  (void)fnType;

  // This code relies on that an optional<()->()>'s tag fits in the function
  // pointer.
  auto &TI = cast<LoadableTypeInfo>(getTypeInfo(operandType));
  assert(TI.mayHaveExtraInhabitants(IGM) &&
         "Must have extra inhabitants to be able to handle the optional "
         "closure case");
  (void)TI;

  Explosion closure = getLoweredExplosion(i->getOperand());
  auto func = closure.claimNext();
  (void)func;
  auto context = closure.claimNext();
  assert(closure.empty());
  if (context->getType()->isIntegerTy())
    context = Builder.CreateIntToPtr(context, IGM.RefCountedPtrTy);
  auto result = emitIsEscapingClosureCall(context, i->getLoc().getSourceLoc(),
                                          i->getVerificationType());
  Explosion out;
  out.add(result);
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::emitDebugInfoForAllocStack(AllocStackInst *i,
                                                  const TypeInfo &type,
                                                  llvm::Value *addr) {
  auto VarInfo = i->getVarInfo();
  if (!VarInfo)
    return;

  VarDecl *Decl = i->getDecl();
  // Describe the underlying alloca. This way an llvm.dbg.declare intrinsic
  // is used, which is valid for the entire lifetime of the alloca.
  if (auto *BitCast = dyn_cast<llvm::BitCastInst>(addr)) {
    auto *Op0 = BitCast->getOperand(0);
    if (auto *Alloca = dyn_cast<llvm::AllocaInst>(Op0))
      addr = Alloca;
    else if (auto *CoroAllocaGet = dyn_cast<llvm::IntrinsicInst>(Op0)) {
      if (CoroAllocaGet->getIntrinsicID() == llvm::Intrinsic::coro_alloca_get)
        addr = CoroAllocaGet;
    } else if (auto *call = dyn_cast<llvm::CallInst>(Op0)) {
      addr = call;
      bool isTaskAlloc = isCallToSwiftTaskAlloc(call);
      assert(isTaskAlloc && "expecting call to swift_task_alloc");
      (void)isTaskAlloc;
    }
  }

  auto DS = i->getDebugScope();
  if (!DS)
    return;

  if (i->getDebugScope()->getInlinedFunction()->isTransparent())
    return;
  
  bool IsAnonymous = false;
  VarInfo->Name = getVarName(i, IsAnonymous);

  // At this point addr must be an alloca or an undef.
  assert(isa<llvm::AllocaInst>(addr) || isa<llvm::UndefValue>(addr) ||
         isa<llvm::IntrinsicInst>(addr) || isCallToSwiftTaskAlloc(addr));

  auto Indirection = DirectValue;
  if (InCoroContext(*CurSILFn, *i))
    Indirection =
        isCallToSwiftTaskAlloc(addr) ? CoroIndirectValue : CoroDirectValue;

  if (!IGM.IRGen.Opts.DisableDebuggerShadowCopies &&
      !IGM.IRGen.Opts.shouldOptimize())
    if (auto *Alloca = dyn_cast<llvm::AllocaInst>(addr))
      if (!Alloca->isStaticAlloca()) {
        // Store the address of the dynamic alloca on the stack.
        addr = emitShadowCopy(addr, DS, *VarInfo, IGM.getPointerAlignment(),
                              /*init*/ true, i->getUsesMoveableValueDebugInfo())
                   .getAddress();
        Indirection =
            InCoroContext(*CurSILFn, *i) ? CoroIndirectValue : IndirectValue;
      }

  // Ignore compiler-generated patterns but not optional bindings.
  if (Decl) {
    if (auto *Pattern = Decl->getParentPattern()) {
      if (Pattern->isImplicit() &&
          Pattern->getKind() != PatternKind::OptionalSome)
        return;
    }
  }

  SILType SILTy;
  bool IsFragmentType = false;
  if (auto MaybeSILTy = VarInfo->Type) {
    // If there is auxiliary type info, use it
    SILTy = *MaybeSILTy;
  } else {
    SILTy = i->getType();
    if (VarInfo->DIExpr)
      IsFragmentType = VarInfo->DIExpr.hasFragment();
  }
  auto RealType = SILTy.getASTType();
  DebugTypeInfo DbgTy;
  if (Decl) {
    DbgTy = DebugTypeInfo::getLocalVariable(Decl, RealType, type, IGM,
                                            IsFragmentType);
  } else if (i->getFunction()->isBare() && !SILTy.hasArchetype() &&
             !VarInfo->Name.empty()) {
    DbgTy = DebugTypeInfo::getFromTypeInfo(RealType, getTypeInfo(SILTy), IGM,
                                           IsFragmentType);
  } else
    return;

  bindArchetypes(DbgTy.getType());
  if (IGM.DebugInfo) {
    emitDebugVariableDeclaration(
        addr, DbgTy, SILTy, DS, i->getLoc(), *VarInfo, Indirection,
        AddrDbgInstrKind(i->getUsesMoveableValueDebugInfo()));
  }
}

void IRGenSILFunction::visitAllocStackInst(swift::AllocStackInst *i) {
  const TypeInfo &type = getTypeInfo(i->getElementType());
  // Derive name from SIL location.
  StringRef dbgname;
  VarDecl *Decl = i->getDecl();
# ifndef NDEBUG
  // If this is a DEBUG build, use pretty names for the LLVM IR.
  bool IsAnonymous = false;
  dbgname = getVarName(i, IsAnonymous);
# endif

  auto stackAddr = type.allocateStack(*this, i->getElementType(), dbgname);
  setLoweredStackAddress(i, stackAddr);
  Address addr = stackAddr.getAddress();

  // Generate Debug Info.
  if (!i->getVarInfo())
    return;

  if (Decl) {
    Type Desugared = Decl->getType()->getDesugaredType();
    if (Desugared->getClassOrBoundGenericClass() ||
        Desugared->getStructOrBoundGenericStruct())
      zeroInit(dyn_cast<llvm::AllocaInst>(addr.getAddress()));
  }
  emitDebugInfoForAllocStack(i, type, addr.getAddress());
}

void IRGenSILFunction::visitAllocPackInst(swift::AllocPackInst *i) {
  auto addr = allocatePack(*this, i->getPackType());
  setLoweredStackAddress(i, addr);
}

static void
buildTailArrays(IRGenSILFunction &IGF,
                SmallVectorImpl<std::pair<SILType, llvm::Value *>> &TailArrays,
                AllocRefInstBase *ARI) {
  auto Types = ARI->getTailAllocatedTypes();
  auto Counts = ARI->getTailAllocatedCounts();
  for (unsigned Idx = 0, NumTypes = Types.size(); Idx < NumTypes; ++Idx) {
    Explosion ElemCount = IGF.getLoweredExplosion(Counts[Idx].get());
    TailArrays.push_back({Types[Idx], ElemCount.claimNext()});
  }
}

void IRGenSILFunction::visitAllocRefInst(swift::AllocRefInst *i) {
  int StackAllocSize = -1;
  if (i->canAllocOnStack()) {
    estimateStackSize();
    // Is there enough space for stack allocation?
    StackAllocSize = IGM.IRGen.Opts.StackPromotionSizeLimit - EstimatedStackSize;
  }
  SmallVector<std::pair<SILType, llvm::Value *>, 4> TailArrays;
  buildTailArrays(*this, TailArrays, i);

  llvm::Value *alloced = emitClassAllocation(*this, i->getType(), i->isObjC(),
                                             StackAllocSize, TailArrays);
  if (StackAllocSize >= 0) {
    // Remember that this alloc_ref allocates the object on the stack.

    StackAllocs.insert(i);
    EstimatedStackSize += StackAllocSize;
  }
  Explosion e;
  e.add(alloced);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitAllocRefDynamicInst(swift::AllocRefDynamicInst *i) {
  int StackAllocSize = -1;
  if (i->canAllocOnStack()) {
    assert(i->isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType());
    estimateStackSize();
    // Is there enough space for stack allocation?
    StackAllocSize = IGM.IRGen.Opts.StackPromotionSizeLimit - EstimatedStackSize;
  }

  SmallVector<std::pair<SILType, llvm::Value *>, 4> TailArrays;
  buildTailArrays(*this, TailArrays, i);

  Explosion metadata = getLoweredExplosion(i->getMetatypeOperand());
  auto metadataValue = metadata.claimNext();
  llvm::Value *alloced = emitClassAllocationDynamic(*this, metadataValue,
                                                    i->getType(), i->isObjC(),
                                                    StackAllocSize,
                                                    TailArrays);

  if (StackAllocSize >= 0) {
    // Remember that this alloc_ref_dynamic allocates the object on the stack.
    StackAllocs.insert(i);
    EstimatedStackSize += StackAllocSize;
  }

  Explosion e;
  e.add(alloced);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitDeallocStackInst(swift::DeallocStackInst *i) {
  if (auto *closure = dyn_cast<PartialApplyInst>(i->getOperand())) {
    assert(closure->isOnStack());
    auto stackAddr = LoweredPartialApplyAllocations[i->getOperand()];
    emitDeallocateDynamicAlloca(stackAddr);
    return;
  }

  auto allocatedType = i->getOperand()->getType();
  const TypeInfo &allocatedTI = getTypeInfo(allocatedType);
  StackAddress stackAddr = getLoweredStackAddress(i->getOperand());

  allocatedTI.deallocateStack(*this, stackAddr, allocatedType);
}

void IRGenSILFunction::visitDeallocStackRefInst(DeallocStackRefInst *i) {
  Explosion self = getLoweredExplosion(i->getOperand());
  auto selfValue = self.claimNext();
  auto *ARI = i->getAllocRef();
  if (StackAllocs.count(ARI)) {
    if (IGM.IRGen.Opts.EmitStackPromotionChecks) {
      selfValue = Builder.CreateBitCast(selfValue, IGM.RefCountedPtrTy);
      emitVerifyEndOfLifetimeCall(selfValue);
    } else {
      // This has two purposes:
      // 1. Tell LLVM the lifetime of the allocated stack memory.
      // 2. Avoid tail-call optimization which may convert the call to the final
      //    release to a jump, which is done after the stack frame is
      //    destructed.
      Builder.CreateLifetimeEnd(selfValue);
    }
  }
}

void IRGenSILFunction::visitDeallocPackInst(swift::DeallocPackInst *i) {
  auto allocatedType = cast<SILPackType>(i->getOperand()->getType().getASTType());
  StackAddress stackAddr = getLoweredStackAddress(i->getOperand());
  deallocatePack(*this, stackAddr, allocatedType);
}

void IRGenSILFunction::visitDeallocRefInst(swift::DeallocRefInst *i) {
  // Lower the operand.
  Explosion self = getLoweredExplosion(i->getOperand());
  auto selfValue = self.claimNext();
  auto *ARI = dyn_cast<AllocRefInstBase>(i->getOperand());
  if (ARI && StackAllocs.count(ARI)) {
    // We can ignore dealloc_refs for stack allocated objects.
    //
    //   %0 = alloc_ref [stack]
    //     ...
    //   dealloc_ref %0     // not needed (stems from the inlined deallocator)
    //     ...
    //   dealloc_stack_ref %0
    return;
  }
  auto classType = i->getOperand()->getType();
  emitClassDeallocation(*this, classType, selfValue,
                        CurSILFn->getGenericSignature());
}

void IRGenSILFunction::visitDeallocPartialRefInst(swift::DeallocPartialRefInst *i) {
  Explosion self = getLoweredExplosion(i->getInstance());
  auto selfValue = self.claimNext();
  Explosion metadata = getLoweredExplosion(i->getMetatype());
  auto metadataValue = metadata.claimNext();
  auto classType = i->getInstance()->getType();

  emitPartialClassDeallocation(*this, classType, selfValue, metadataValue,
                               CurSILFn->getGenericSignature());
}

void IRGenSILFunction::visitDeallocBoxInst(swift::DeallocBoxInst *i) {
  Explosion owner = getLoweredExplosion(i->getOperand());
  llvm::Value *ownerPtr = owner.claimNext();

  auto boxTy = i->getOperand()->getType().castTo<SILBoxType>();
  emitDeallocateBox(*this, ownerPtr, boxTy);
}

void IRGenSILFunction::visitAllocBoxInst(swift::AllocBoxInst *i) {
  assert(i->getBoxType()->getLayout()->getFields().size() == 1
         && "multi field boxes not implemented yet");
  const TypeInfo &type = getTypeInfo(
      getSILBoxFieldType(IGM.getMaximalTypeExpansionContext(), i->getBoxType(),
                         IGM.getSILModule().Types, 0));

  // Derive name from SIL location.
  bool IsAnonymous = false;
  VarDecl *Decl = i->getDecl();
  StringRef Name = getVarName(i, IsAnonymous);
  StringRef DbgName =
# ifndef NDEBUG
    // If this is a DEBUG build, use pretty names for the LLVM IR.
    Name;
# else
    "";
# endif

  auto boxTy = i->getType().castTo<SILBoxType>();
  OwnedAddress boxWithAddr = emitAllocateBox(*this, boxTy,
                                             CurSILFn->getGenericEnvironment(),
                                             DbgName);
  setLoweredBox(i, boxWithAddr);

  if (i->getDebugScope()->getInlinedFunction()->isTransparent())
    return;

  if (!Decl)
    return;

  // FIXME: This is a workaround to not produce local variables for
  // capture list arguments like "[weak self]". The better solution
  // would be to require all variables to be described with a
  // SILDebugValue(Addr) and then not describe capture list
  // arguments.
  if (Name == IGM.Context.Id_self.str())
    return;

  assert(i->getBoxType()->getLayout()->getFields().size() == 1 &&
         "box for a local variable should only have one field");
  auto SILTy = getSILBoxFieldType(
      IGM.getMaximalTypeExpansionContext(),
      i->getBoxType(), IGM.getSILModule().Types, 0);
  auto RealType = SILTy.getASTType();
  auto DbgTy =
      DebugTypeInfo::getLocalVariable(Decl, RealType, type, IGM, false);

  auto VarInfo = i->getVarInfo();
  if (!VarInfo)
    return;

  auto Storage =
      emitShadowCopyIfNeeded(boxWithAddr.getAddress(), i->getDebugScope(),
                             *VarInfo, IsAnonymous, false /*was moved*/);

  if (!IGM.DebugInfo)
    return;

  IGM.DebugInfo->emitVariableDeclaration(
      Builder, Storage, DbgTy, i->getDebugScope(), i->getLoc(), *VarInfo,
      InCoroContext(*CurSILFn, *i) ? CoroIndirectValue : IndirectValue);
}

void IRGenSILFunction::visitProjectBoxInst(swift::ProjectBoxInst *i) {
  auto boxTy = i->getOperand()->getType().castTo<SILBoxType>();

  const LoweredValue &val = getLoweredValue(i->getOperand());
  if (val.isBoxWithAddress()) {
    // The operand is an alloc_box. We can directly reuse the address.
    setLoweredAddress(i, val.getAddressOfBox());
  } else {
    // The slow-path: we have to emit code to get from the box to it's
    // value address.
    Explosion box = val.getExplosion(*this, i->getOperand()->getType());
    auto addr = emitProjectBox(*this, box.claimNext(), boxTy);
    setLoweredAddress(i, addr);
  }
}

static ExclusivityFlags getExclusivityAction(SILAccessKind kind) {
  switch (kind) {
  case SILAccessKind::Read:
    return ExclusivityFlags::Read;
  case SILAccessKind::Modify:
    return ExclusivityFlags::Modify;
  case SILAccessKind::Init:
  case SILAccessKind::Deinit:
    llvm_unreachable("init/deinit access should not use dynamic enforcement");
  }
  llvm_unreachable("bad access kind");
}

static ExclusivityFlags getExclusivityFlags(SILModule &M,
                                            SILAccessKind kind,
                                            bool noNestedConflict) {
  auto flags = getExclusivityAction(kind);

  if (!noNestedConflict)
    flags |= ExclusivityFlags::Tracking;

  return flags;
}

static SILAccessEnforcement getEffectiveEnforcement(IRGenFunction &IGF,
                                                    BeginAccessInst *access) {
  auto enforcement = access->getEnforcement();

  // Don't use dynamic enforcement for known-empty types; there's no
  // actual memory there, and the address may not be valid and unique.
  // This is really a hack; we don't necessarily know that all clients
  // will agree whether a type is empty.  On the other hand, the situations
  // where IRGen generates a meaningless address should always be a subset
  // of cases where this triggers, because of the restrictions on abstracting
  // over addresses and the fact that we use static enforcement on inouts.
  if (enforcement == SILAccessEnforcement::Dynamic &&
      IGF.IGM.getTypeInfo(access->getSource()->getType())
             .isKnownEmpty(ResilienceExpansion::Maximal)) {
    enforcement = SILAccessEnforcement::Unsafe;
  }

  return enforcement;
}

template <class BeginAccessInst>
static ExclusivityFlags getExclusivityFlags(BeginAccessInst *i) {
  return getExclusivityFlags(i->getModule(), i->getAccessKind(),
                             i->hasNoNestedConflict());
}

void IRGenSILFunction::visitBeginAccessInst(BeginAccessInst *access) {
  Address addr = getLoweredAddress(access->getOperand());
  switch (getEffectiveEnforcement(*this, access)) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("unknown access enforcement in IRGen!");

  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
    // nothing to do
    setLoweredAddress(access, addr);
    return;

  case SILAccessEnforcement::Dynamic: {
    llvm::Value *scratch = createAlloca(IGM.getFixedBufferTy(),
                                        IGM.getPointerAlignment(),
                                        "access-scratch").getAddress();
    Builder.CreateLifetimeStart(scratch);

    llvm::Value *pointer =
      Builder.CreateBitCast(addr.getAddress(), IGM.Int8PtrTy);
    llvm::Value *flags =
      llvm::ConstantInt::get(IGM.SizeTy, uint64_t(getExclusivityFlags(access)));
    llvm::Value *pc = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    auto call = Builder.CreateCall(IGM.getBeginAccessFunctionPointer(),
                                   {pointer, scratch, flags, pc});
    call->setDoesNotThrow();

    setLoweredDynamicallyEnforcedAddress(access, addr, scratch);
    return;
  }
  case SILAccessEnforcement::Signed: {
    auto &ti = getTypeInfo(access->getType());
    auto *sea = cast<StructElementAddrInst>(access->getOperand());
    auto *Int64PtrTy = llvm::Type::getInt64PtrTy(IGM.getLLVMContext());
    auto *Int64PtrPtrTy = Int64PtrTy->getPointerTo();
    if (access->getAccessKind() == SILAccessKind::Read) {
      // When we see a signed read access, generate code to:
      // authenticate the signed pointer if non-null, and store the
      // authenticated value to a shadow stack location. Set the lowered address
      // of the access to this stack location.
      auto pointerAuthQual = sea->getField()->getPointerAuthQualifier();
      auto *pointerToSignedFptr = getLoweredAddress(sea).getAddress();
      auto *pointerToIntPtr =
          Builder.CreateBitCast(pointerToSignedFptr, Int64PtrPtrTy);
      auto *signedFptr = Builder.CreateLoad(pointerToIntPtr, Int64PtrTy,
                                            IGM.getPointerAlignment());

      // Create a stack temporary.
      auto temp = ti.allocateStack(*this, access->getType(), "ptrauth.temp");
      auto *tempAddressToIntPtr =
          Builder.CreateBitCast(temp.getAddressPointer(), Int64PtrPtrTy);

      // Branch based on pointer is null or not.
      llvm::Value *cond = Builder.CreateICmpNE(
          signedFptr, llvm::ConstantPointerNull::get(Int64PtrTy));
      auto *resignNonNull = createBasicBlock("resign-nonnull");
      auto *resignNull = createBasicBlock("resign-null");
      auto *resignCont = createBasicBlock("resign-cont");
      Builder.CreateCondBr(cond, resignNonNull, resignNull);

      // Resign if non-null.
      Builder.emitBlock(resignNonNull);
      auto oldAuthInfo =
          PointerAuthInfo::emit(*this, pointerAuthQual, pointerToSignedFptr);
      // ClangImporter imports the c function pointer as an optional type.
      PointerAuthEntity entity(
          sea->getType().getOptionalObjectType().getAs<SILFunctionType>());
      auto newAuthInfo = PointerAuthInfo::emit(
          *this, IGM.getOptions().PointerAuth.FunctionPointers,
          pointerToSignedFptr, entity);
      auto *resignedFptr =
          emitPointerAuthResign(*this, signedFptr, oldAuthInfo, newAuthInfo);
      Builder.CreateStore(resignedFptr, tempAddressToIntPtr,
                          IGM.getPointerAlignment());
      Builder.CreateBr(resignCont);

      // If null, no need to resign.
      Builder.emitBlock(resignNull);
      Builder.CreateStore(signedFptr, tempAddressToIntPtr,
                          IGM.getPointerAlignment());
      Builder.CreateBr(resignCont);

      Builder.emitBlock(resignCont);
      setLoweredAddress(access, temp.getAddress());
      return;
    }
    if (access->getAccessKind() == SILAccessKind::Modify ||
        access->getAccessKind() == SILAccessKind::Init) {
      // When we see a signed modify access, create a shadow stack location and
      // set the lowered address of the access to this stack location.
      auto temp = ti.allocateStack(*this, access->getType(), "ptrauth.temp");
      setLoweredAddress(access, temp.getAddress());
      return;
    }
    llvm_unreachable("Incompatible access kind with begin_access [signed]");
  }
  }
  llvm_unreachable("bad access enforcement");
}

static bool hasBeenInlined(BeginUnpairedAccessInst *access) {
  // Check to see if the buffer is defined locally.
  return isa<AllocStackInst>(access->getBuffer());
}

void IRGenSILFunction::visitBeginUnpairedAccessInst(
                                              BeginUnpairedAccessInst *access) {
  Address addr = getLoweredAddress(access->getSource());
  switch (access->getEnforcement()) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("unknown access enforcement in IRGen!");

  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
  case SILAccessEnforcement::Signed:
    // nothing to do
    return;

  case SILAccessEnforcement::Dynamic: {
    llvm::Value *scratch = getLoweredAddress(access->getBuffer()).getAddress();

    llvm::Value *pointer =
      Builder.CreateBitCast(addr.getAddress(), IGM.Int8PtrTy);
    llvm::Value *flags =
      llvm::ConstantInt::get(IGM.SizeTy, uint64_t(getExclusivityFlags(access)));

    // Compute the effective PC of the access.
    // Since begin_unpaired_access is designed for materializeForSet, our
    // heuristic here is as well: we've either been inlined, in which case
    // we should use the current PC (i.e. pass null), or we haven't,
    // in which case we should use the caller, which is generally ok because
    // materializeForSet can't usually be thunked.
    llvm::Value *pc;
    // Wasm doesn't have returnaddress because it can't access call frame
    // for security purposes
    if (IGM.Triple.isWasm() || hasBeenInlined(access)) {
      pc = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    } else {
      pc =
          Builder.CreateIntrinsicCall(llvm::Intrinsic::returnaddress,
                                      {llvm::ConstantInt::get(IGM.Int32Ty, 0)});
    }

    auto call = Builder.CreateCall(IGM.getBeginAccessFunctionPointer(),
                                   {pointer, scratch, flags, pc});
    call->setDoesNotThrow();
    return;
  }
  }
  llvm_unreachable("bad access enforcement");
}

void IRGenSILFunction::visitEndAccessInst(EndAccessInst *i) {
  auto access = i->getBeginAccess();
  switch (getEffectiveEnforcement(*this, access)) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("unknown access enforcement in IRGen!");

  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
    // nothing to do
    return;

  case SILAccessEnforcement::Dynamic: {
    if (access->hasNoNestedConflict())
      return;

    auto scratch = getLoweredDynamicEnforcementScratchBuffer(access);

    auto call =
        Builder.CreateCall(IGM.getEndAccessFunctionPointer(), {scratch});
    call->setDoesNotThrow();

    Builder.CreateLifetimeEnd(scratch);
    return;
  }

  case SILAccessEnforcement::Signed: {
    if (access->getAccessKind() != SILAccessKind::Modify &&
        access->getAccessKind() != SILAccessKind::Init) {
      // nothing to do.
      return;
    }
    // When we see a signed modify access, get the lowered address of the
    // access which is the shadow stack slot, sign the value if non-null and
    // write back to the struct field.
    auto *sea = cast<StructElementAddrInst>(access->getOperand());
    auto *Int64PtrTy = llvm::Type::getInt64PtrTy(IGM.getLLVMContext());
    auto *Int64PtrPtrTy = Int64PtrTy->getPointerTo();
    auto pointerAuthQual = cast<StructElementAddrInst>(access->getOperand())
                               ->getField()
                               ->getPointerAuthQualifier();
    auto *pointerToSignedFptr =
        getLoweredAddress(access->getOperand()).getAddress();
    auto *pointerToIntPtr =
        Builder.CreateBitCast(pointerToSignedFptr, Int64PtrPtrTy);
    auto tempAddress = getLoweredAddress(access);
    auto *tempAddressToIntPtr =
        Builder.CreateBitCast(tempAddress.getAddress(), Int64PtrPtrTy);
    auto *tempAddressValue = Builder.CreateLoad(tempAddressToIntPtr, Int64PtrTy,
                                                IGM.getPointerAlignment());
    // Branch based on value is null or not.
    llvm::Value *cond = Builder.CreateICmpNE(
        tempAddressValue, llvm::ConstantPointerNull::get(Int64PtrTy));
    auto *resignNonNull = createBasicBlock("resign-nonnull");
    auto *resignNull = createBasicBlock("resign-null");
    auto *resignCont = createBasicBlock("resign-cont");

    Builder.CreateCondBr(cond, resignNonNull, resignNull);

    Builder.emitBlock(resignNonNull);

    // If non-null, resign
    // ClangImporter imports the c function pointer as an optional type.
    PointerAuthEntity entity(
        sea->getType().getOptionalObjectType().getAs<SILFunctionType>());
    auto oldAuthInfo = PointerAuthInfo::emit(
        *this, IGM.getOptions().PointerAuth.FunctionPointers,
        tempAddress.getAddress(), entity);
    auto newAuthInfo =
        PointerAuthInfo::emit(*this, pointerAuthQual, pointerToSignedFptr);
    auto *signedFptr = emitPointerAuthResign(*this, tempAddressValue,
                                             oldAuthInfo, newAuthInfo);
    Builder.CreateStore(signedFptr, pointerToIntPtr, IGM.getPointerAlignment());

    Builder.CreateBr(resignCont);

    // If null, no need to resign
    Builder.emitBlock(resignNull);
    Builder.CreateStore(tempAddressValue, pointerToIntPtr, IGM.getPointerAlignment());
    Builder.CreateBr(resignCont);

    Builder.emitBlock(resignCont);
    return;
  }
  }
  llvm_unreachable("bad access enforcement");
}

void IRGenSILFunction::visitEndUnpairedAccessInst(EndUnpairedAccessInst *i) {
  switch (i->getEnforcement()) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("unknown access enforcement in IRGen!");

  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
  case SILAccessEnforcement::Signed:
    // nothing to do
    return;

  case SILAccessEnforcement::Dynamic: {
    auto scratch = getLoweredAddress(i->getBuffer()).getAddress();

    auto call =
        Builder.CreateCall(IGM.getEndAccessFunctionPointer(), {scratch});
    call->setDoesNotThrow();
    return;
  }
  }
  llvm_unreachable("bad access enforcement");
}

void IRGenSILFunction::visitConvertFunctionInst(swift::ConvertFunctionInst *i) {
  // This instruction is specified to be a no-op.
  Explosion temp = getLoweredExplosion(i->getOperand());

  auto fnType = i->getType().castTo<SILFunctionType>();
  if (temp.size() == 1 &&
      fnType->getRepresentation() != SILFunctionType::Representation::Block) {
    auto *fn = temp.claimNext();
    Explosion res;
    auto sig = IGM.getSignature(fnType);
    res.add(Builder.CreateBitCast(fn, sig.getType()->getPointerTo()));
    setLoweredExplosion(i, res);
    return;
  }

  setLoweredExplosion(i, temp);
}

void IRGenSILFunction::visitConvertEscapeToNoEscapeInst(
    swift::ConvertEscapeToNoEscapeInst *i) {
  // This instruction makes the context trivial.
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;
  // Differentiable functions contain multiple pairs of fn and ctx pointer.
  for (unsigned index : range(in.size() / 2)) {
    (void)index;
    llvm::Value *fn = in.claimNext();
    llvm::Value *ctx = in.claimNext();
    out.add(fn);
    out.add(Builder.CreateBitCast(ctx, IGM.OpaquePtrTy));
  }
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitAddressToPointerInst(swift::AddressToPointerInst *i)
{
  Explosion to;
  llvm::Value *addrValue = getLoweredAddress(i->getOperand()).getAddress();
  if (addrValue->getType() != IGM.Int8PtrTy)
    addrValue = Builder.CreateBitCast(addrValue, IGM.Int8PtrTy);
  to.add(addrValue);
  setLoweredExplosion(i, to);
}

// Ignores the isStrict flag because Swift TBAA is not lowered into LLVM IR.
void IRGenSILFunction::visitPointerToAddressInst(swift::PointerToAddressInst *i)
{
  Explosion from = getLoweredExplosion(i->getOperand());
  llvm::Value *ptrValue = from.claimNext();

  auto &ti = getTypeInfo(i->getType());
  
  llvm::Type *destType = ti.getStorageType()->getPointerTo();
  ptrValue = Builder.CreateBitCast(ptrValue, destType);

  if (i->alignment())
    setLoweredAddress(i, Address(ptrValue, ti.getStorageType(),
                                 Alignment(i->alignment()->value())));
  else
    setLoweredAddress(i, ti.getAddressForPointer(ptrValue));
}

static void emitPointerCastInst(IRGenSILFunction &IGF,
                                SILValue src,
                                SILValue dest,
                                const TypeInfo &ti) {
  Explosion from = IGF.getLoweredExplosion(src);
  llvm::Value *ptrValue = from.claimNext();
  // The input may have witness tables or other additional data, but the class
  // reference is always first.
  (void)from.claimAll();

  auto schema = ti.getSchema();
  assert(schema.size() == 1
         && schema[0].isScalar()
         && "pointer schema is not a single scalar?!");
  auto castToType = schema[0].getScalarType();

  // A retainable pointer representation may be wrapped in an optional, so we
  // need to provide inttoptr/ptrtoint in addition to bitcast.
  ptrValue = IGF.Builder.CreateBitOrPointerCast(ptrValue, castToType);
  
  Explosion to;
  to.add(ptrValue);
  IGF.setLoweredExplosion(dest, to);
}

void IRGenSILFunction::visitUncheckedRefCastInst(
                                             swift::UncheckedRefCastInst *i) {
  auto &ti = getTypeInfo(i->getType());
  emitPointerCastInst(*this, i->getOperand(), i, ti);
}

// TODO: Although runtime checks are not required, we get them anyway when
// asking the runtime to perform this cast. If this is a performance impact, we
// can add a CheckedCastMode::Unchecked.
void IRGenSILFunction::
visitUncheckedRefCastAddrInst(swift::UncheckedRefCastAddrInst *i) {
  Address dest = getLoweredAddress(i->getDest());
  Address src = getLoweredAddress(i->getSrc());
  emitCheckedCast(*this,
                  src, i->getSourceFormalType(),
                  dest, i->getTargetFormalType(),
                  CastConsumptionKind::TakeAlways,
                  CheckedCastMode::Unconditional);
}

void IRGenSILFunction::visitUncheckedAddrCastInst(
                                             swift::UncheckedAddrCastInst *i) {
  auto addr = getLoweredAddress(i->getOperand());
  auto &ti = getTypeInfo(i->getType());
  auto result = Builder.CreateElementBitCast(addr, ti.getStorageType());
  setLoweredAddress(i, result);
}

static bool isStructurallySame(const llvm::Type *T1, const llvm::Type *T2) {
  if (T1 == T2) return true;
  
  if (auto *S1 = dyn_cast<llvm::StructType>(T1))
    if (auto *S2 = dyn_cast<llvm::StructType>(T2))
      return S1->isLayoutIdentical(const_cast<llvm::StructType*>(S2));
  return false;
}

// Emit a trap in the event a type does not match expected layout constraints.
// 
// We can hit this case in specialized functions even for correct user code.
// If the user dynamically checks for correct type sizes in the generic
// function, a specialized function can contain the (not executed) bitcast
// with mismatching fixed sizes.
// Usually llvm can eliminate this code again because the user's safety
// check should be constant foldable on llvm level.
static void emitTrapAndUndefValue(IRGenSILFunction &IGF,
                                  Explosion &in,
                                  Explosion &out,
                                  const LoadableTypeInfo &outTI) {
  llvm::BasicBlock *failBB =
    llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
  IGF.Builder.CreateBr(failBB);
  IGF.FailBBs.push_back(failBB);
  
  IGF.Builder.emitBlock(failBB);
  IGF.emitTrap("mismatching type layouts", /*EmitUnreachable=*/true);

  llvm::BasicBlock *contBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
  IGF.Builder.emitBlock(contBB);
  (void)in.claimAll();
  for (auto schema : outTI.getSchema())
    out.add(llvm::UndefValue::get(schema.getScalarType()));
}

static void emitUncheckedValueBitCast(IRGenSILFunction &IGF,
                                      SourceLoc loc,
                                      Explosion &in,
                                      const LoadableTypeInfo &inTI,
                                      Explosion &out,
                                      const LoadableTypeInfo &outTI) {
  // If the transfer is doable bitwise, and if the elements of the explosion are
  // the same type, then just transfer the elements.
  if (inTI.isBitwiseTakable(ResilienceExpansion::Maximal) &&
      outTI.isBitwiseTakable(ResilienceExpansion::Maximal) &&
      isStructurallySame(inTI.getStorageType(), outTI.getStorageType())) {
    in.transferInto(out, in.size());
    return;
  }

  // TODO: We could do bitcasts entirely in the value domain in some cases, but
  // for simplicity, let's just always go through the stack for now.
  
  // Create the allocation.
  auto inStorage = IGF.createAlloca(inTI.getStorageType(),
                                  std::max(inTI.getFixedAlignment(),
                                           outTI.getFixedAlignment()),
                                  "bitcast");
  
  auto maxSize = std::max(inTI.getFixedSize(), outTI.getFixedSize());
  IGF.Builder.CreateLifetimeStart(inStorage, maxSize);
  
  // Store the 'in' value.
  inTI.initialize(IGF, in, inStorage, false);
  // Load the 'out' value as the destination type.
  auto outStorage =
      IGF.Builder.CreateElementBitCast(inStorage, outTI.getStorageType());
  outTI.loadAsTake(IGF, outStorage, out);
  
  IGF.Builder.CreateLifetimeEnd(inStorage, maxSize);
  return;
}

static void emitValueBitwiseCast(IRGenSILFunction &IGF,
                                 SourceLoc loc,
                                 Explosion &in,
                                 const LoadableTypeInfo &inTI,
                                 Explosion &out,
                                 const LoadableTypeInfo &outTI) {
  // Unfortunately, we can't check this invariant until we get to IRGen, since
  // the AST and SIL don't know anything about type layout.
  if (inTI.getFixedSize() < outTI.getFixedSize()) {
    emitTrapAndUndefValue(IGF, in, out, outTI);
    return;
  }
  emitUncheckedValueBitCast(IGF, loc, in, inTI, out, outTI);
}

void IRGenSILFunction::visitUncheckedTrivialBitCastInst(
                                      swift::UncheckedTrivialBitCastInst *i) {
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;
  
  emitValueBitwiseCast(*this, i->getLoc().getSourceLoc(),
            in,  cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType())),
            out, cast<LoadableTypeInfo>(getTypeInfo(i->getType())));
  
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::
visitUncheckedBitwiseCastInst(swift::UncheckedBitwiseCastInst *i) {
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;

  emitValueBitwiseCast(*this, i->getLoc().getSourceLoc(),
            in,  cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType())),
            out, cast<LoadableTypeInfo>(getTypeInfo(i->getType())));
  
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitRefToRawPointerInst(
                                             swift::RefToRawPointerInst *i) {
  auto &ti = getTypeInfo(i->getType());
  emitPointerCastInst(*this, i->getOperand(), i, ti);
}

void IRGenSILFunction::visitRawPointerToRefInst(swift::RawPointerToRefInst *i) {
  auto &ti = getTypeInfo(i->getType());
  emitPointerCastInst(*this, i->getOperand(), i, ti);
}

// SIL scalar conversions which never change the IR type.
// FIXME: Except for optionals, which get bit-packed into an integer.
static void trivialRefConversion(IRGenSILFunction &IGF,
                                 SILValue input,
                                 SILValue result) {
  Explosion temp = IGF.getLoweredExplosion(input);
  auto &inputTI = IGF.getTypeInfo(input->getType());
  auto &resultTI = IGF.getTypeInfo(result->getType());
  
  // If the types are the same, forward the existing value.
  if (inputTI.getStorageType() == resultTI.getStorageType()) {
    IGF.setLoweredExplosion(result, temp);
    return;
  }

  auto schema = resultTI.getSchema();
  Explosion out;

  for (auto schemaElt : schema) {
    auto resultTy = schemaElt.getScalarType();

    llvm::Value *value = temp.claimNext();
    if (value->getType() == resultTy) {
      // Nothing to do.  This happens with the unowned conversions.
    } else if (resultTy->isPointerTy()) {
      value = IGF.Builder.CreateIntToPtr(value, resultTy);
    } else {
      value = IGF.Builder.CreatePtrToInt(value, resultTy);
    }
    out.add(value);
  }

  IGF.setLoweredExplosion(result, out);
}

// SIL scalar conversions which never change the IR type.
// FIXME: Except for optionals, which get bit-packed into an integer.
#define NOOP_CONVERSION(KIND)                                     \
void IRGenSILFunction::visit##KIND##Inst(swift::KIND##Inst *i) {  \
  ::trivialRefConversion(*this, i->getOperand(), i); \
}
#define LOADABLE_REF_STORAGE(Name, ...) \
  NOOP_CONVERSION(Name##ToRef) \
  NOOP_CONVERSION(RefTo##Name)
#include "swift/AST/ReferenceStorage.def"
#undef NOOP_CONVERSION

void IRGenSILFunction::visitThinToThickFunctionInst(
                                            swift::ThinToThickFunctionInst *i) {
  // Take the incoming function pointer and add a null context pointer to it.
  Explosion from = getLoweredExplosion(i->getOperand());
  Explosion to;
  to.add(Builder.CreateBitCast(from.claimNext(), IGM.FunctionPtrTy));
  if (i->getType().castTo<SILFunctionType>()->isNoEscape())
    to.add(llvm::ConstantPointerNull::get(IGM.OpaquePtrTy));
  else
    to.add(IGM.RefCountedNull);
  setLoweredExplosion(i, to);
}

void IRGenSILFunction::visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *i){
  Explosion from = getLoweredExplosion(i->getOperand());
  llvm::Value *swiftMeta = from.claimNext();
  // Claim any conformances.
  (void)from.claimAll();
  CanType instanceType(i->getType().castTo<AnyMetatypeType>().getInstanceType());
  Explosion to;
  llvm::Value *classPtr =
    emitClassHeapMetadataRefForMetatype(*this, swiftMeta, instanceType);
  to.add(Builder.CreateBitCast(classPtr, IGM.ObjCClassPtrTy));
  setLoweredExplosion(i, to);
}

void IRGenSILFunction::visitObjCToThickMetatypeInst(
                         ObjCToThickMetatypeInst *i) {
  Explosion from = getLoweredExplosion(i->getOperand());
  llvm::Value *classPtr = from.claimNext();
  
  // Fetch the metadata for that class.
  Explosion to;
  auto metadata = emitObjCMetadataRefForMetadata(*this, classPtr);
  to.add(metadata);
  setLoweredExplosion(i, to);  
}

void IRGenSILFunction::visitUnconditionalCheckedCastInst(
                                       swift::UnconditionalCheckedCastInst *i) {
  Explosion value = getLoweredExplosion(i->getOperand());
  Explosion ex;
  emitScalarCheckedCast(*this, value,
                        i->getSourceLoweredType(),
                        i->getSourceFormalType(),
                        i->getTargetLoweredType(),
                        i->getTargetFormalType(),
                        CheckedCastMode::Unconditional,
                        CurSILFn->getGenericSignature(),
                        ex);
  setLoweredExplosion(i, ex);
}

void IRGenSILFunction::visitObjCMetatypeToObjectInst(
                                                  ObjCMetatypeToObjectInst *i){
  // Bitcast the @objc metatype reference, which is already an ObjC object, to
  // the destination type.
  Explosion from = getLoweredExplosion(i->getOperand());
  llvm::Value *value = from.claimNext();
  value = Builder.CreateBitCast(value, IGM.UnknownRefCountedPtrTy);
  Explosion to;
  to.add(value);
  setLoweredExplosion(i, to);
}

void IRGenSILFunction::visitObjCExistentialMetatypeToObjectInst(
                                       ObjCExistentialMetatypeToObjectInst *i){
  // Bitcast the @objc metatype reference, which is already an ObjC object, to
  // the destination type. The metatype may carry additional witness tables we
  // can drop.
  Explosion from = getLoweredExplosion(i->getOperand());
  llvm::Value *value = from.claimNext();
  (void)from.claimAll();
  value = Builder.CreateBitCast(value, IGM.UnknownRefCountedPtrTy);
  Explosion to;
  to.add(value);
  setLoweredExplosion(i, to);
}

void IRGenSILFunction::visitObjCProtocolInst(ObjCProtocolInst *i) {
  // Get the protocol reference.
  llvm::Value *protoRef = emitReferenceToObjCProtocol(*this, i->getProtocol());
  // Bitcast it to the class reference type.
  protoRef = Builder.CreateBitCast(protoRef,
                                   getTypeInfo(i->getType()).getStorageType());
  Explosion ex;
  ex.add(protoRef);
  setLoweredExplosion(i, ex);
}

void IRGenSILFunction::visitRefToBridgeObjectInst(
                                              swift::RefToBridgeObjectInst *i) {
  Explosion refEx = getLoweredExplosion(i->getConverted());
  llvm::Value *ref = refEx.claimNext();
  
  Explosion bitsEx = getLoweredExplosion(i->getBitsOperand());
  llvm::Value *bits = bitsEx.claimNext();
  
  // Mask the bits into the pointer representation.
  llvm::Value *val = Builder.CreatePtrToInt(ref, IGM.SizeTy);
  val = Builder.CreateOr(val, bits);
  val = Builder.CreateIntToPtr(val, IGM.BridgeObjectPtrTy);
  
  Explosion resultEx;
  resultEx.add(val);
  
  setLoweredExplosion(i, resultEx);
}

void IRGenSILFunction::
visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *i) {
  Explosion boEx = getLoweredExplosion(i->getOperand());
  llvm::Value *bridgeVal = boEx.claimNext();
  bridgeVal = Builder.CreatePtrToInt(bridgeVal, IGM.SizeTy);

  // This returns two bits, the first of which is "is Objective-C object", the
  // second is "is Objective-C Tagged Pointer".  Each of these bits is computed
  // by checking to see if some other bits are non-zero in the BridgeObject.
  auto bitsNonZero = [&](const SpareBitVector &bits) -> llvm::Value* {
    // If this target doesn't have the specified field, just produce false.
    if (!bits.any())
      return Builder.getInt1(0);

    llvm::Value *bitsValue =
      Builder.CreateAnd(bridgeVal, Builder.getInt(bits.asAPInt()));
    return
      Builder.CreateICmpNE(bitsValue, llvm::ConstantInt::get(IGM.SizeTy, 0));
  };

  Explosion wordEx;
  wordEx.add(bitsNonZero(IGM.TargetInfo.IsObjCPointerBit));
  wordEx.add(bitsNonZero(IGM.TargetInfo.ObjCPointerReservedBits));
  setLoweredExplosion(i, wordEx);
}

void IRGenSILFunction::visitValueToBridgeObjectInst(
    ValueToBridgeObjectInst *i) {
  Explosion in = getLoweredExplosion(i->getOperand());
  Explosion out;

  emitValueBitwiseCast(
      *this, i->getLoc().getSourceLoc(), in,
      cast<LoadableTypeInfo>(getTypeInfo(i->getOperand()->getType())), out,
      cast<LoadableTypeInfo>(getTypeInfo(i->getType())));

  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitBridgeObjectToRefInst(
                                              swift::BridgeObjectToRefInst *i) {
  Explosion boEx = getLoweredExplosion(i->getConverted());
  llvm::Value *bo = boEx.claimNext();
  Explosion resultEx;
  
  auto &refTI = getTypeInfo(i->getType());
  llvm::Type *refType = refTI.getSchema()[0].getScalarType();
  
  // If the value is an ObjC tagged pointer, pass it through verbatim.
  llvm::BasicBlock *taggedCont = nullptr,
    *tagged = nullptr,
    *notTagged = nullptr;
  llvm::Value *taggedRef = nullptr;
  llvm::Value *boBits = nullptr;
  
  ClassDecl *Cl = i->getType().getClassOrBoundGenericClass();
  if (IGM.TargetInfo.hasObjCTaggedPointers() &&
      (!Cl || !isKnownNotTaggedPointer(IGM, Cl))) {
    boBits = Builder.CreatePtrToInt(bo, IGM.SizeTy);
    APInt maskValue = IGM.TargetInfo.ObjCPointerReservedBits.asAPInt();
    llvm::Value *mask = Builder.getInt(maskValue);
    llvm::Value *reserved = Builder.CreateAnd(boBits, mask);
    llvm::Value *cond = Builder.CreateICmpEQ(reserved,
                                         llvm::ConstantInt::get(IGM.SizeTy, 0));
    tagged = createBasicBlock("tagged-pointer"),
    notTagged = createBasicBlock("not-tagged-pointer");
    taggedCont = createBasicBlock("tagged-cont");
    
    Builder.CreateCondBr(cond, notTagged, tagged);
    
    Builder.emitBlock(tagged);
    taggedRef = Builder.CreateBitCast(bo, refType);
    Builder.CreateBr(taggedCont);
    
    // If it's not a tagged pointer, mask off the spare bits.
    Builder.emitBlock(notTagged);
  }

  // Mask off the spare bits (if they exist).
  auto &spareBits = IGM.getHeapObjectSpareBits();
  llvm::Value *result;
  if (spareBits.any()) {
    APInt maskValue = ~spareBits.asAPInt();
    
    if (!boBits)
      boBits = Builder.CreatePtrToInt(bo, IGM.SizeTy);
    
    llvm::Value *mask = llvm::ConstantInt::get(IGM.getLLVMContext(), maskValue);
    llvm::Value *masked = Builder.CreateAnd(boBits, mask);
    result = Builder.CreateIntToPtr(masked, refType);
  } else {
    result = Builder.CreateBitCast(bo, refType);
  }
  
  if (taggedCont) {
    Builder.CreateBr(taggedCont);
    
    Builder.emitBlock(taggedCont);
    
    auto phi = Builder.CreatePHI(refType, 2);
    phi->addIncoming(taggedRef, tagged);
    phi->addIncoming(result, notTagged);
    
    result = phi;
  }
  
  resultEx.add(result);
  setLoweredExplosion(i, resultEx);
}

void IRGenSILFunction::visitBridgeObjectToWordInst(
                                             swift::BridgeObjectToWordInst *i) {
  Explosion boEx = getLoweredExplosion(i->getConverted());
  llvm::Value *val = boEx.claimNext();
  val = Builder.CreatePtrToInt(val, IGM.SizeTy);
  Explosion wordEx;
  wordEx.add(val);
  setLoweredExplosion(i, wordEx);
}

void IRGenSILFunction::visitUnconditionalCheckedCastAddrInst(
                                   swift::UnconditionalCheckedCastAddrInst *i) {
  Address dest = getLoweredAddress(i->getDest());
  Address src = getLoweredAddress(i->getSrc());
  emitCheckedCast(*this,
                  src, i->getSourceFormalType(),
                  dest, i->getTargetFormalType(),
                  CastConsumptionKind::TakeAlways,
                  CheckedCastMode::Unconditional);
}

void IRGenSILFunction::visitCheckedCastBranchInst(
                                              swift::CheckedCastBranchInst *i) {
  FailableCastResult castResult;
  Explosion ex;
  if (i->isExact()) {
    auto operand = i->getOperand();
    Explosion source = getLoweredExplosion(operand);
    castResult = emitClassIdenticalCast(*this, source.claimNext(),
                                        i->getSourceLoweredType(),
                                        i->getTargetLoweredType(),
                                        CurSILFn->getGenericSignature());
  } else {
    Explosion value = getLoweredExplosion(i->getOperand());
    emitScalarCheckedCast(*this, value,
                          i->getSourceLoweredType(),
                          i->getSourceFormalType(),
                          i->getTargetLoweredType(),
                          i->getTargetFormalType(),
                          CheckedCastMode::Conditional,
                          CurSILFn->getGenericSignature(),
                          ex);
    auto val = ex.claimNext();
    castResult.casted = val;
    llvm::Value *nil =
      llvm::ConstantPointerNull::get(cast<llvm::PointerType>(val->getType()));
    castResult.succeeded = Builder.CreateICmpNE(val, nil);
  }

  // Branch on the success of the cast.
  // All cast operations currently return null on failure.


  auto &successBB = getLoweredBB(i->getSuccessBB());
  llvm::Type *toTy = IGM.getTypeInfo(i->getTargetLoweredType()).getStorageType();
  if (toTy->isPointerTy())
    castResult.casted = Builder.CreateBitCast(castResult.casted, toTy);

  Builder.CreateCondBr(castResult.succeeded,
                       successBB.bb,
                       getLoweredBB(i->getFailureBB()).bb);
  
  // Feed the cast result into the nonnull branch.
  unsigned phiIndex = 0;
  Explosion ex2;
  ex2.add(castResult.casted);
  ex2.add(ex.claimAll());
  addIncomingExplosionToPHINodes(*this, successBB, phiIndex, ex2);
}

void IRGenSILFunction::visitCheckedCastAddrBranchInst(
                                          swift::CheckedCastAddrBranchInst *i) {
  Address dest = getLoweredAddress(i->getDest());
  Address src = getLoweredAddress(i->getSrc());
  llvm::Value *castSucceeded =
    emitCheckedCast(*this,
                    src, i->getSourceFormalType(),
                    dest, i->getTargetFormalType(),
                    i->getConsumptionKind(), CheckedCastMode::Conditional);
  Builder.CreateCondBr(castSucceeded,
                       getLoweredBB(i->getSuccessBB()).bb,
                       getLoweredBB(i->getFailureBB()).bb);
}

void IRGenSILFunction::visitHopToExecutorInst(HopToExecutorInst *i) {
  assert(i->getTargetExecutor()->getType().getOptionalObjectType()
           .is<BuiltinExecutorType>());
  llvm::Value *resumeFn = Builder.CreateIntrinsicCall(
          llvm::Intrinsic::coro_async_resume, {});

  Explosion executor;
  getLoweredExplosion(i->getOperand(), executor);

  emitSuspensionPoint(executor, resumeFn);
}

void IRGenSILFunction::visitKeyPathInst(swift::KeyPathInst *I) {
  auto pattern = IGM.getAddrOfKeyPathPattern(I->getPattern(), I->getLoc());
  // Build up the argument vector to instantiate the pattern here.
  Optional<StackAddress> dynamicArgsBuf;
  llvm::Value *args;
  if (!I->getSubstitutions().empty() || !I->getAllOperands().empty()) {
    auto sig = I->getPattern()->getGenericSignature();
    SubstitutionMap subs = I->getSubstitutions();

    SmallVector<GenericRequirement, 4> requirements;
    enumerateGenericSignatureRequirements(sig,
            [&](GenericRequirement reqt) { requirements.push_back(reqt); });

    llvm::Value *argsBufSize;
    llvm::Value *argsBufAlign;
    
    if (!I->getSubstitutions().empty()) {
      argsBufSize = llvm::ConstantInt::get(IGM.SizeTy,
                       IGM.getPointerSize().getValue() * requirements.size());
      argsBufAlign = llvm::ConstantInt::get(IGM.SizeTy,
                       IGM.getPointerAlignment().getMaskValue());
    } else {
      argsBufSize = llvm::ConstantInt::get(IGM.SizeTy, 0);
      argsBufAlign = llvm::ConstantInt::get(IGM.SizeTy, 0);
    }

    SmallVector<llvm::Value *, 4> operandOffsets;
    for (unsigned i : indices(I->getAllOperands())) {
      auto operand = I->getAllOperands()[i].get();
      auto &ti = getTypeInfo(operand->getType());
      auto ty = operand->getType();
      auto alignMask = ti.getAlignmentMask(*this, ty);
      if (i != 0) {
        auto notAlignMask = Builder.CreateNot(alignMask);
        argsBufSize = Builder.CreateAdd(argsBufSize, alignMask);
        argsBufSize = Builder.CreateAnd(argsBufSize, notAlignMask);
      }
      operandOffsets.push_back(argsBufSize);
      auto size = ti.getSize(*this, ty);
      argsBufSize = Builder.CreateAdd(argsBufSize, size);
      argsBufAlign = Builder.CreateOr(argsBufAlign, alignMask);
    }

    dynamicArgsBuf = emitDynamicAlloca(IGM.Int8Ty, argsBufSize, Alignment(16));
    
    Address argsBuf = dynamicArgsBuf->getAddress();
    
    if (!I->getSubstitutions().empty()) {
      emitInitOfGenericRequirementsBuffer(*this, requirements, argsBuf,
                                          MetadataState::Complete, subs);
    }
    
    for (unsigned i : indices(I->getAllOperands())) {
      auto operand = I->getAllOperands()[i].get();
      auto &ti = getTypeInfo(operand->getType());
      auto ptr = Builder.CreateInBoundsGEP(IGM.Int8Ty, argsBuf.getAddress(),
                                           operandOffsets[i]);
      auto addr = ti.getAddressForPointer(
        Builder.CreateBitCast(ptr, ti.getStorageType()->getPointerTo()));
      if (operand->getType().isAddress()) {
        ti.initializeWithTake(*this, addr, getLoweredAddress(operand),
                              operand->getType(), false);
      } else {
        Explosion operandValue = getLoweredExplosion(operand);
        cast<LoadableTypeInfo>(ti).initialize(*this, operandValue, addr, false);
      }
    }
    args = argsBuf.getAddress();
  } else {
    // No arguments necessary, so the argument ought to be ignored by any
    // callbacks in the pattern.
    assert(I->getAllOperands().empty() && "indices not implemented");
    args = llvm::UndefValue::get(IGM.Int8PtrTy);
  }
  auto patternPtr = llvm::ConstantExpr::getBitCast(pattern, IGM.Int8PtrTy);
  auto call = Builder.CreateCall(IGM.getGetKeyPathFunctionPointer(),
                                 {patternPtr, args});
  call->setDoesNotThrow();

  if (dynamicArgsBuf) {
    emitDeallocateDynamicAlloca(*dynamicArgsBuf);
  }

  auto resultStorageTy = IGM.getTypeInfo(I->getType()).getStorageType();

  Explosion e;
  e.add(Builder.CreateBitCast(call, resultStorageTy));
  setLoweredExplosion(I, e);
}

void IRGenSILFunction::visitUpcastInst(swift::UpcastInst *i) {
  auto toTy = getTypeInfo(i->getType()).getSchema()[0].getScalarType();
  Explosion from = getLoweredExplosion(i->getOperand());
  Explosion to;
  assert(from.size() == 1 && "class should explode to single value");
  llvm::Value *fromValue = from.claimNext();
  to.add(Builder.CreateBitCast(fromValue, toTy));
  setLoweredExplosion(i, to);
}

void IRGenSILFunction::visitIndexAddrInst(swift::IndexAddrInst *i) {
  Address base = getLoweredAddress(i->getBase());
  Explosion indexValues = getLoweredExplosion(i->getIndex());
  llvm::Value *index = indexValues.claimNext();
  
  auto baseTy = i->getBase()->getType();
  auto &ti = getTypeInfo(baseTy);
  
  Address dest = ti.indexArray(*this, base, index, baseTy);
  setLoweredAddress(i, dest);
}

void IRGenSILFunction::visitTailAddrInst(swift::TailAddrInst *i) {
  Address base = getLoweredAddress(i->getBase());
  Explosion indexValues = getLoweredExplosion(i->getIndex());
  llvm::Value *index = indexValues.claimNext();

  SILType baseTy = i->getBase()->getType();
  const TypeInfo &baseTI = getTypeInfo(baseTy);

  Address dest = baseTI.indexArray(*this, base, index, baseTy);
  const TypeInfo &TailTI = getTypeInfo(i->getTailType());
  dest = TailTI.roundUpToTypeAlignment(*this, dest, i->getTailType());
  llvm::Type *destType = TailTI.getStorageType();
  dest = Builder.CreateElementBitCast(dest, destType);
  setLoweredAddress(i, dest);
}

void IRGenSILFunction::visitIndexRawPointerInst(swift::IndexRawPointerInst *i) {
  Explosion baseValues = getLoweredExplosion(i->getBase());
  llvm::Value *base = baseValues.claimNext();
  
  Explosion indexValues = getLoweredExplosion(i->getIndex());
  llvm::Value *index = indexValues.claimNext();
  
  // We don't expose a non-inbounds GEP operation.
  llvm::Value *destValue = Builder.CreateInBoundsGEP(IGM.Int8Ty, base, index);

  Explosion result;
  result.add(destValue);
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitInitExistentialAddrInst(swift::InitExistentialAddrInst *i) {
  Address container = getLoweredAddress(i->getOperand());
  SILType destType = i->getOperand()->getType();
  emitOpaqueExistentialContainerInit(
      *this, container, destType, i->getFormalConcreteType(),
      i->getLoweredConcreteType(), i->getConformances());
  auto srcType = i->getLoweredConcreteType();

  // Allocate a COW box for the value if necessary.
  auto *genericEnv = CurSILFn->getGenericEnvironment();
  setLoweredAddress(
      i, emitAllocateBoxedOpaqueExistentialBuffer(
             *this, destType, srcType, container, genericEnv, false));
}

void IRGenSILFunction::visitInitExistentialValueInst(
    swift::InitExistentialValueInst *i) {
  llvm_unreachable("unsupported instruction during IRGen");
}

void IRGenSILFunction::visitInitExistentialMetatypeInst(
                                              InitExistentialMetatypeInst *i) {
  Explosion metatype = getLoweredExplosion(i->getOperand());
  Explosion result;
  emitExistentialMetatypeContainer(*this,
                                   result, i->getType(),
                                   metatype.claimNext(),
                                   i->getOperand()->getType(),
                                   i->getConformances());
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitInitExistentialRefInst(InitExistentialRefInst *i) {
  Explosion instance = getLoweredExplosion(i->getOperand());
  Explosion result;
  emitClassExistentialContainer(*this,
                               result, i->getType(),
                               instance.claimNext(),
                               i->getFormalConcreteType(),
                               i->getOperand()->getType(),
                               i->getConformances());
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitDeinitExistentialAddrInst(
                                              swift::DeinitExistentialAddrInst *i) {
  Address container = getLoweredAddress(i->getOperand());

  // Deallocate the COW box for the value if necessary.
  emitDeallocateBoxedOpaqueExistentialBuffer(*this, i->getOperand()->getType(),
                                             container);
}

void IRGenSILFunction::visitDeinitExistentialValueInst(
    swift::DeinitExistentialValueInst *i) {
  llvm_unreachable("unsupported instruction during IRGen");
}

void IRGenSILFunction::visitOpenExistentialAddrInst(OpenExistentialAddrInst *i) {
  SILType baseTy = i->getOperand()->getType();
  Address base = getLoweredAddress(i->getOperand());

  auto openedArchetype = i->getType().castTo<ArchetypeType>();

  // Insert a copy of the boxed value for COW semantics if necessary.
  auto accessKind = i->getAccessKind();
  Address object = emitOpaqueBoxedExistentialProjection(
      *this, accessKind, base, baseTy, openedArchetype,
      CurSILFn->getGenericSignature());

  setLoweredAddress(i, object);
}

void IRGenSILFunction::visitOpenExistentialRefInst(OpenExistentialRefInst *i) {

  SILType baseTy = i->getOperand()->getType();
  Explosion base = getLoweredExplosion(i->getOperand());
  auto openedArchetype = i->getType().castTo<ArchetypeType>();

  Explosion result;
  llvm::Value *instance
    = emitClassExistentialProjection(*this, base, baseTy,
                                     openedArchetype,
                                     CurSILFn->getGenericSignature());
  result.add(instance);
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitOpenExistentialMetatypeInst(
                                              OpenExistentialMetatypeInst *i) {
  SILType baseTy = i->getOperand()->getType();
  Explosion base = getLoweredExplosion(i->getOperand());
  auto openedTy = i->getType().getASTType();

  llvm::Value *metatype =
    emitExistentialMetatypeProjection(*this, base, baseTy, openedTy);
  Explosion result;
  result.add(metatype);
  setLoweredExplosion(i, result);
}

void IRGenSILFunction::visitOpenExistentialValueInst(
    OpenExistentialValueInst *i) {
  llvm_unreachable("unsupported instruction during IRGen");
}

void IRGenSILFunction::visitPackLengthInst(PackLengthInst *i) {
  auto length = emitPackShapeExpression(i->getPackType());
  setLoweredSingletonExplosion(i, length);
}

void IRGenSILFunction::visitDynamicPackIndexInst(DynamicPackIndexInst *i) {
  // At the IRGen level, this is just a type change.
  auto index = getLoweredSingletonExplosion(i->getOperand());
  setLoweredSingletonExplosion(i, index);
}

void IRGenSILFunction::visitPackPackIndexInst(PackPackIndexInst *i) {
  auto startIndexOfSlice =
    emitIndexOfStructuralPackComponent(*this, i->getIndexedPackType(),
                                       i->getComponentStartIndex());
  auto indexWithinSlice =
    getLoweredSingletonExplosion(i->getSliceIndexOperand());
  auto index = Builder.CreateAdd(startIndexOfSlice, indexWithinSlice);
  setLoweredSingletonExplosion(i, index);
}

void IRGenSILFunction::visitScalarPackIndexInst(ScalarPackIndexInst *i) {
  auto index =
    emitIndexOfStructuralPackComponent(*this, i->getIndexedPackType(),
                                       i->getComponentIndex());
  setLoweredSingletonExplosion(i, index);
}

void IRGenSILFunction::visitOpenPackElementInst(swift::OpenPackElementInst *i) {
  llvm::Value *index = getLoweredSingletonExplosion(i->getIndexOperand());

  auto *env = i->getOpenedGenericEnvironment();
  bindOpenedElementArchetypesAtIndex(*this, env, index);
  // The result is just used for type dependencies.
}

void IRGenSILFunction::visitPackElementGetInst(PackElementGetInst *i) {
  Address pack = getLoweredAddress(i->getPack());
  llvm::Value *index = getLoweredSingletonExplosion(i->getIndex());

  auto elementType = i->getElementType();
  auto &elementTI = getTypeInfo(elementType);

  auto elementStorageAddr = emitStorageAddressOfPackElement(
      *this, pack, index, elementType, i->getPackType());

  assert(elementType.isAddress() &&
         i->getPackType()->isElementAddress() &&
         "direct packs not currently supported");
  auto ptr = Builder.CreateLoad(elementStorageAddr);
  auto elementAddr = elementTI.getAddressForPointer(ptr);
  setLoweredAddress(i, elementAddr);
}

void IRGenSILFunction::visitPackElementSetInst(PackElementSetInst *i) {
  Address pack = getLoweredAddress(i->getPack());
  llvm::Value *index = getLoweredSingletonExplosion(i->getIndex());

  auto elementType = i->getElementType();
  auto elementStorageAddress = emitStorageAddressOfPackElement(
      *this, pack, index, elementType, i->getPackType());

  assert(elementType.isAddress() &&
         i->getPackType()->isElementAddress() &&
         "direct packs not currently supported");
  auto elementValue = getLoweredAddress(i->getValue());
  Builder.CreateStore(elementValue.getAddress(), elementStorageAddress);
}

void IRGenSILFunction::visitTuplePackElementAddrInst(
                                                  TuplePackElementAddrInst *i) {
  Address tuple = getLoweredAddress(i->getTuple());
  llvm::Value *index = getLoweredSingletonExplosion(i->getIndex());

  auto elementType = i->getElementType();
  auto elementAddr =
    projectTupleElementAddressByDynamicIndex(*this, tuple,
                                             i->getTuple()->getType(),
                                             index, elementType);
  setLoweredAddress(i, elementAddr);
}

void IRGenSILFunction::visitProjectBlockStorageInst(ProjectBlockStorageInst *i){
  // TODO
  Address block = getLoweredAddress(i->getOperand());
  Address capture = projectBlockStorageCapture(*this, block,
                       i->getOperand()->getType().castTo<SILBlockStorageType>());
  
  setLoweredAddress(i, capture);
}

void IRGenSILFunction::visitInitBlockStorageHeaderInst(
                                               InitBlockStorageHeaderInst *i) {
  auto addr = getLoweredAddress(i->getBlockStorage());
  
  // We currently only support static invoke functions.
  auto &invokeVal = getLoweredValue(i->getInvokeFunction());
  llvm::Constant *invokeFn = nullptr;
  ForeignFunctionInfo foreignInfo;
  if (invokeVal.kind != LoweredValue::Kind::FunctionPointer) {
    IGM.unimplemented(i->getLoc().getSourceLoc(),
                      "non-static block invoke function");
  } else {
    auto &fn = invokeVal.getFunctionPointer();
    invokeFn = fn.getDirectPointer();
    foreignInfo = fn.getForeignInfo();
  }

  assert(foreignInfo.ClangInfo && "no clang info for block function?");
  
  // Initialize the header.
  emitBlockHeader(*this, addr,
          i->getBlockStorage()->getType().castTo<SILBlockStorageType>(),
          invokeFn, i->getInvokeFunction()->getType().castTo<SILFunctionType>(),
          foreignInfo);
  
  // Cast the storage to the block type to produce the result value.
  llvm::Value *asBlock = Builder.CreateBitCast(addr.getAddress(),
                                               IGM.ObjCBlockPtrTy);
  Explosion e;
  e.add(asBlock);
  setLoweredExplosion(i, e);
}

void IRGenSILFunction::visitAllocExistentialBoxInst(AllocExistentialBoxInst *i){
  OwnedAddress boxWithAddr =
    emitBoxedExistentialContainerAllocation(*this, i->getExistentialType(),
                                            i->getFormalConcreteType(),
                                            i->getConformances(),
                                            CurSILFn->getGenericSignature());
  setLoweredBox(i, boxWithAddr);
}

void IRGenSILFunction::visitDeallocExistentialBoxInst(
                                                DeallocExistentialBoxInst *i) {
  Explosion box = getLoweredExplosion(i->getOperand());
  emitBoxedExistentialContainerDeallocation(*this, box,
                                            i->getOperand()->getType(),
                                            i->getConcreteType());
}

void IRGenSILFunction::visitOpenExistentialBoxInst(OpenExistentialBoxInst *i) {
  Explosion box = getLoweredExplosion(i->getOperand());
  auto openedArchetype = i->getType().castTo<ArchetypeType>();

  auto addr = emitOpenExistentialBox(*this, box, i->getOperand()->getType(),
                                     openedArchetype);
  setLoweredAddress(i, addr);
}

void IRGenSILFunction::visitOpenExistentialBoxValueInst(
  OpenExistentialBoxValueInst *i) {
  llvm_unreachable("unsupported instruction during IRGen");
}

void
IRGenSILFunction::visitProjectExistentialBoxInst(ProjectExistentialBoxInst *i) {
  const LoweredValue &val = getLoweredValue(i->getOperand());
  if (val.isBoxWithAddress()) {
    // The operand is an alloc_existential_box.
    // We can directly reuse the address.
    setLoweredAddress(i, val.getAddressOfBox());
  } else {
    Explosion box = getLoweredExplosion(i->getOperand());
    auto caddr = emitBoxedExistentialProjection(*this, box,
                                                i->getOperand()->getType(),
                                                i->getType().getASTType());
    setLoweredAddress(i, caddr.getAddress());
  }
}

void IRGenSILFunction::visitWitnessMethodInst(swift::WitnessMethodInst *i) {
  CanType baseTy = i->getLookupType();
  ProtocolConformanceRef conformance = i->getConformance();
  SILDeclRef member = i->getMember();
  auto fnType = IGM.getSILTypes().getConstantFunctionType(
      IGM.getMaximalTypeExpansionContext(), member);

  assert(member.requiresNewWitnessTableEntry());

  bool shouldUseDispatchThunk = false;
  if (IGM.isResilient(conformance.getRequirement(), ResilienceExpansion::Maximal)) {
    shouldUseDispatchThunk = true;
  } else if (IGM.getOptions().WitnessMethodElimination) {
    // For WME, use a thunk if the target protocol is defined in another module.
    // This way, we guarantee all wmethod call sites are visible to the LLVM VFE
    // optimization in GlobalDCE.
    auto protoDecl = cast<ProtocolDecl>(member.getDecl()->getDeclContext());
    shouldUseDispatchThunk = protoDecl->getModuleContext() != IGM.getSwiftModule();
  }

  if (shouldUseDispatchThunk) {
    llvm::Constant *fnPtr = IGM.getAddrOfDispatchThunk(member, NotForDefinition);
    llvm::Constant *secondaryValue = nullptr;

    if (fnType->isAsync()) {
      secondaryValue = fnPtr;
      auto *fnPtrType = fnPtr->getType();
      fnPtr = IGM.getAddrOfAsyncFunctionPointer(
          LinkEntity::forDispatchThunk(member));
      fnPtr = llvm::ConstantExpr::getBitCast(fnPtr, fnPtrType);
    }

    auto sig = IGM.getSignature(fnType);
    auto fn = FunctionPointer::forDirect(fnType, fnPtr, secondaryValue, sig, true);

    setLoweredFunctionPointer(i, fn);
    return;
  }

  // It would be nice if this weren't discarded.
  llvm::Value *baseMetadataCache = nullptr;

  auto fn = emitWitnessMethodValue(*this, baseTy, &baseMetadataCache, member,
                                   conformance);

  setLoweredFunctionPointer(i, fn);
}

void IRGenSILFunction::setAllocatedAddressForBuffer(SILValue v,
                                                const Address &allocedAddress) {
  overwriteAllocatedAddress(v, allocedAddress);

  // Emit the debug info for the variable if any.
  if (auto allocStack = dyn_cast<AllocStackInst>(v)) {
    emitDebugInfoForAllocStack(allocStack, getTypeInfo(v->getType()),
                               allocedAddress.getAddress());
  }
}

void IRGenSILFunction::visitCopyAddrInst(swift::CopyAddrInst *i) {
  SILType addrTy = i->getSrc()->getType();
  const TypeInfo &addrTI = getTypeInfo(addrTy);
  Address src = getLoweredAddress(i->getSrc());
  // See whether we have a deferred fixed-size buffer initialization.
  auto &loweredDest = getLoweredValue(i->getDest());
  assert(!loweredDest.isUnallocatedAddressInBuffer());
  Address dest = loweredDest.getAnyAddress();
  if (i->isInitializationOfDest()) {
    if (i->isTakeOfSrc()) {
      addrTI.initializeWithTake(*this, dest, src, addrTy, false);
    } else {
      addrTI.initializeWithCopy(*this, dest, src, addrTy, false);
    }
  } else {
    if (i->isTakeOfSrc()) {
      addrTI.assignWithTake(*this, dest, src, addrTy, false);
    } else {
      addrTI.assignWithCopy(*this, dest, src, addrTy, false);
    }
  }
}

void IRGenSILFunction::visitExplicitCopyAddrInst(
    swift::ExplicitCopyAddrInst *i) {
  SILType addrTy = i->getSrc()->getType();
  const TypeInfo &addrTI = getTypeInfo(addrTy);
  Address src = getLoweredAddress(i->getSrc());
  // See whether we have a deferred fixed-size buffer initialization.
  auto &loweredDest = getLoweredValue(i->getDest());
  assert(!loweredDest.isUnallocatedAddressInBuffer());
  Address dest = loweredDest.getAnyAddress();
  if (i->isInitializationOfDest()) {
    if (i->isTakeOfSrc()) {
      addrTI.initializeWithTake(*this, dest, src, addrTy, false);
    } else {
      addrTI.initializeWithCopy(*this, dest, src, addrTy, false);
    }
  } else {
    if (i->isTakeOfSrc()) {
      addrTI.assignWithTake(*this, dest, src, addrTy, false);
    } else {
      addrTI.assignWithCopy(*this, dest, src, addrTy, false);
    }
  }
}

// bind_memory and rebind_memory are no-ops because Swift TBAA info is not
// lowered to LLVM IR TBAA, and the output token is ignored except for
// verification.
void IRGenSILFunction::visitBindMemoryInst(swift::BindMemoryInst *i) {
  LoweredValue &token = getUndefLoweredValue(i->getType());
  setLoweredValue(i, std::move(token));
}

void IRGenSILFunction::visitRebindMemoryInst(swift::RebindMemoryInst *i) {
  LoweredValue &token = getUndefLoweredValue(i->getType());
  setLoweredValue(i, std::move(token));
}

void IRGenSILFunction::visitDestroyAddrInst(swift::DestroyAddrInst *i) {
  SILType addrTy = i->getOperand()->getType();
  const TypeInfo &addrTI = getTypeInfo(addrTy);

  Address base = getLoweredAddress(i->getOperand());
  addrTI.destroy(*this, base, addrTy, false /*isOutlined*/);
}

void IRGenSILFunction::visitCondFailInst(swift::CondFailInst *i) {
  Explosion e = getLoweredExplosion(i->getOperand());
  llvm::Value *cond = e.claimNext();

  // The condition should be false, or we die.
  auto expectedCond = Builder.CreateExpect(cond,
                                         llvm::ConstantInt::get(IGM.Int1Ty, 0));
  
  // Emit individual fail blocks so that we can map the failure back to a source
  // line.
  auto origInsertionPoint = Builder.GetInsertBlock();

  llvm::BasicBlock *failBB = llvm::BasicBlock::Create(IGM.getLLVMContext());
  llvm::BasicBlock *contBB = llvm::BasicBlock::Create(IGM.getLLVMContext());
  Builder.CreateCondBr(expectedCond, failBB, contBB);
    
  Builder.SetInsertPoint(&CurFn->back());
  Builder.emitBlock(failBB);
  if (IGM.DebugInfo)
    // If we are emitting DWARF, this does nothing. Otherwise the ``llvm.trap``
    // instruction emitted from ``Builtin.condfail`` should have an inlined
    // debug location. This is because zero is not an artificial line location
    // in CodeView.
    IGM.DebugInfo->setInlinedTrapLocation(Builder, i->getDebugScope());
  emitTrap(i->getMessage(), /*EmitUnreachable=*/true);
  
  Builder.SetInsertPoint(origInsertionPoint);
  Builder.emitBlock(contBB);
  FailBBs.push_back(failBB);
}

void IRGenSILFunction::visitIncrementProfilerCounterInst(
    IncrementProfilerCounterInst *i) {
  // If we import profiling intrinsics from a swift module but profiling is
  // not enabled, ignore the increment.
  if (!getSILModule().getOptions().GenerateProfile)
    return;

  // Retrieve the global variable that stores the PGO function name, creating it
  // if needed.
  auto funcName = i->getPGOFuncName();
  auto varLinkage = llvm::GlobalValue::LinkOnceAnyLinkage;
  auto *nameVar = IGM.Module.getNamedGlobal(
      llvm::getPGOFuncNameVarName(funcName, varLinkage));
  if (!nameVar)
    nameVar = llvm::createPGOFuncNameVar(IGM.Module, varLinkage, funcName);

  // We need to GEP the function name global to point to the first character of
  // the string.
  llvm::SmallVector<llvm::Value *, 2> indices;
  indices.append(2, llvm::ConstantInt::get(IGM.SizeTy, 0));
  auto *nameGEP = llvm::ConstantExpr::getGetElementPtr(
      nameVar->getValueType(), nameVar, makeArrayRef(indices));

  // Emit the call to the 'llvm.instrprof.increment' LLVM intrinsic.
  llvm::Value *args[] = {
    nameGEP,
    llvm::ConstantInt::get(IGM.Int64Ty, i->getPGOFuncHash()),
    llvm::ConstantInt::get(IGM.Int32Ty, i->getNumCounters()),
    llvm::ConstantInt::get(IGM.Int32Ty, i->getCounterIndex())
  };
  Builder.CreateIntrinsicCall(llvm::Intrinsic::instrprof_increment, args);
}

void IRGenSILFunction::visitSuperMethodInst(swift::SuperMethodInst *i) {
  assert(!i->getMember().isForeign);

  auto base = getLoweredExplosion(i->getOperand());
  auto baseType = i->getOperand()->getType();
  llvm::Value *baseValue = base.claimNext();

  auto method = i->getMember().getOverriddenVTableEntry();
  auto methodType = i->getType().castTo<SILFunctionType>();

  auto *classDecl = cast<ClassDecl>(method.getDecl()->getDeclContext());

  // If the class defining the vtable entry is resilient, we cannot assume
  // its offset since methods can be re-ordered resiliently. Instead, we call
  // the class method lookup function, passing in a reference to the
  // method descriptor.
  if (IGM.hasResilientMetadata(classDecl, ResilienceExpansion::Maximal)) {
    // Load the superclass of the static type of the 'self' value.
    llvm::Value *superMetadata;
    auto instanceTy = CanType(baseType.getASTType()->getMetatypeInstanceType());
    if (!IGM.hasResilientMetadata(instanceTy.getClassOrBoundGenericClass(),
                                  ResilienceExpansion::Maximal)) {
      // It's still possible that the static type of 'self' is not resilient, in
      // which case we can assume its superclass.
      //
      // An example is the following hierarchy, where ModuleA is resilient and
      // we're inside ModuleB:
      //
      // ModuleA.Base <-- defines method
      // |
      // \- ModuleB.Middle
      //    |
      //    \- ModuleB.Derived <-- static type of 'self'
      //
      // It's OK to know that the superclass of Derived is Middle, but the
      // method requires using a resilient access pattern.
      auto superTy = instanceTy->getSuperclass();
      superMetadata = emitClassHeapMetadataRef(*this, superTy->getCanonicalType(),
                                               MetadataValueType::TypeMetadata,
                                               MetadataState::Complete);
    } else {
      // Otherwise, we're in the most general case; the superclass might change,
      // so we have to load it dynamically from the metadata of the static type
      // of 'self'.
      auto *metadata = emitClassHeapMetadataRef(*this, instanceTy,
                                                MetadataValueType::TypeMetadata,
                                                MetadataState::Complete);

      auto superField = emitAddressOfSuperclassRefInClassMetadata(*this, metadata);
      superMetadata = Builder.CreateLoad(superField);
    }

    // Get the method descriptor.
    auto *methodDescriptor =
      IGM.getAddrOfMethodDescriptor(method, NotForDefinition);

    // Get the method lookup function for the class defining the method.
    auto *lookupFn = IGM.getAddrOfMethodLookupFunction(classDecl,
                                                       NotForDefinition);

    // Call the lookup function.
    llvm::Value *fnPtr =
        Builder.CreateCall(lookupFn->getFunctionType(), lookupFn,
                           {superMetadata, methodDescriptor});

    // The function returns an i8*; cast it to the correct type.
    auto sig = IGM.getSignature(methodType);
    fnPtr = Builder.CreateBitCast(fnPtr, sig.getType()->getPointerTo());

    auto &schema = methodType->isAsync()
                       ? getOptions().PointerAuth.AsyncSwiftClassMethodPointers
                       : getOptions().PointerAuth.SwiftClassMethodPointers;
    auto authInfo =
      PointerAuthInfo::emit(*this, schema, /*storageAddress=*/nullptr, method);

    auto fn = FunctionPointer::createSigned(methodType, fnPtr, authInfo, sig, true);

    setLoweredFunctionPointer(i, fn);
    return;
  }

  // Non-resilient case.

  auto fn =
      emitVirtualMethodValue(*this, baseValue, baseType, method, methodType,
                             CurSILFn->getGenericSignature(),
                             /*useSuperVTable*/ true);

  setLoweredFunctionPointer(i, fn);
}

void IRGenSILFunction::visitObjCSuperMethodInst(swift::ObjCSuperMethodInst *i) {
  assert(i->getMember().isForeign);
  setLoweredObjCMethodBounded(i, i->getMember(),
                              i->getOperand()->getType(),
                              /*startAtSuper=*/true);
}

void IRGenSILFunction::visitClassMethodInst(swift::ClassMethodInst *i) {
  assert(!i->getMember().isForeign);

  Explosion base = getLoweredExplosion(i->getOperand());
  llvm::Value *baseValue = base.claimNext();

  SILDeclRef method = i->getMember().getOverriddenVTableEntry();
  auto methodType = i->getType().castTo<SILFunctionType>();

  auto *classDecl = cast<ClassDecl>(method.getDecl()->getDeclContext());
  bool shouldUseDispatchThunk = false;
  if (IGM.hasResilientMetadata(classDecl, ResilienceExpansion::Maximal)) {
    shouldUseDispatchThunk = true;
  } else if (IGM.getOptions().VirtualFunctionElimination) {
    // For VFE, use a thunk if the target class is in another module. This
    // enables VFE (which scans function bodies for used type identifiers) to
    // work across modules by relying on:
    //
    // (1) virtual call sites are in thunks in the same module as the class,
    //     therefore they are always visible to VFE,
    // (2) if a thunk symbol is unused by any other module, we can safely
    //     eliminate it.
    //
    // See the virtual-function-elimination-two-modules.swift testcase for an
    // example of how cross-module VFE can be effectively used.
    shouldUseDispatchThunk =
        classDecl->getModuleContext() != IGM.getSwiftModule();
  }

  if (shouldUseDispatchThunk) {
    llvm::Constant *fnPtr = IGM.getAddrOfDispatchThunk(method, NotForDefinition);

    if (methodType->isAsync()) {
      auto *fnPtrType = fnPtr->getType();
      fnPtr = IGM.getAddrOfAsyncFunctionPointer(
          LinkEntity::forDispatchThunk(method));
      fnPtr = llvm::ConstantExpr::getBitCast(fnPtr, fnPtrType);
    }

    auto fnType = IGM.getSILTypes().getConstantFunctionType(
      IGM.getMaximalTypeExpansionContext(), method);
    auto sig = IGM.getSignature(fnType);
    auto fn = FunctionPointer::createUnsigned(methodType, fnPtr, sig, true);

    setLoweredFunctionPointer(i, fn);
    return;
  }

  // For Swift classes, get the method implementation from the vtable.
  // FIXME: better explosion kind, map as static.
  FunctionPointer fn = emitVirtualMethodValue(
      *this, baseValue, i->getOperand()->getType(), method, methodType,
      CurSILFn->getGenericSignature(),
      /*useSuperVTable*/ false);

  setLoweredFunctionPointer(i, fn);
}

void IRGenSILFunction::visitObjCMethodInst(swift::ObjCMethodInst *i) {
  // For Objective-C classes we need to arrange for a msgSend
  // to happen when the method is called.
  assert(i->getMember().isForeign);
  setLoweredObjCMethod(i, i->getMember());
}

void IRGenModule::emitSILStaticInitializers() {
  SmallVector<SILFunction *, 8> StaticInitializers;
  for (SILGlobalVariable &Global : getSILModule().getSILGlobals()) {
    SILInstruction *InitValue = Global.getStaticInitializerValue();
    if (!InitValue)
      continue;

#ifndef NDEBUG
    SILType loweredTy = Global.getLoweredType();
    auto &ti = getTypeInfo(loweredTy);

    auto expansion = getResilienceExpansionForLayout(&Global);
    assert(ti.isFixedSize(expansion) &&
           "cannot emit a static initializer for dynamically-sized global");
#endif

    LinkEntity entity = LinkEntity::forSILGlobalVariable(&Global, *this);
    std::string name = entity.mangleAsString();

    auto *IRGlobal =
        Module.getGlobalVariable(name, true /* = AllowLocal */);

    // A check for multi-threaded compilation: Is this the llvm module where the
    // global is defined and not only referenced (or not referenced at all).
    if (!IRGlobal || !IRGlobal->hasInitializer())
      continue;

    if (auto *OI = dyn_cast<ObjectInst>(InitValue)) {
      StructLayout *Layout = StaticObjectLayouts[&Global].get();
      llvm::Constant *InitVal = emitConstantObject(*this, OI, Layout);
      if (canMakeStaticObjectsReadOnly()) {
        IRGlobal->setInitializer(InitVal);
      } else {
        auto *ContainerTy = cast<llvm::StructType>(IRGlobal->getValueType());
        auto *zero = llvm::ConstantAggregateZero::get(ContainerTy->getElementType(0));
        IRGlobal->setInitializer(llvm::ConstantStruct::get(ContainerTy,
                                                           {zero , InitVal}));
      }
      continue;
    }

    IRGlobal->setInitializer(
      emitConstantValue(*this, cast<SingleValueInstruction>(InitValue)));
  }
}

void IRGenSILFunction::visitGetAsyncContinuationInst(
    GetAsyncContinuationInst *i) {
  Explosion out;
  emitGetAsyncContinuation(i->getLoweredResumeType(), StackAddress(), out,
                           i->throws());
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitGetAsyncContinuationAddrInst(
    GetAsyncContinuationAddrInst *i) {
  auto resultAddr = getLoweredStackAddress(i->getOperand());
  Explosion out;
  emitGetAsyncContinuation(i->getLoweredResumeType(), resultAddr, out,
                           i->throws());
  setLoweredExplosion(i, out);
}

void IRGenSILFunction::visitAwaitAsyncContinuationInst(
    AwaitAsyncContinuationInst *i) {
  Explosion resumeResult;

  bool isIndirect = i->getResumeBB()->args_empty();
  SILType resumeTy;
  if (!isIndirect)
    resumeTy = (*i->getResumeBB()->args_begin())->getType();

  auto &normalDest = getLoweredBB(i->getResumeBB());
  auto *normalDestBB = normalDest.bb;

  bool hasError = i->getErrorBB() != nullptr;
  auto *errorDestBB = hasError ? getLoweredBB(i->getErrorBB()).bb : nullptr;
  auto *errorPhi = hasError ? getLoweredBB(i->getErrorBB()).phis[0] : nullptr;
  assert(!hasError || getLoweredBB(i->getErrorBB()).phis.size() == 1 &&
                          "error basic block should only expect one value");

  emitAwaitAsyncContinuation(resumeTy, isIndirect, resumeResult,
                             normalDestBB, errorPhi, errorDestBB);
  if (!isIndirect) {
    unsigned firstIndex = 0;
    addIncomingExplosionToPHINodes(*this, normalDest, firstIndex, resumeResult);
    assert(firstIndex == normalDest.phis.size());
  }
}

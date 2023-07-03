//===--- GenFunc.cpp - Swift IR Generation for Function Types -------------===//
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
//  This file implements IR generation for function types in Swift.  This
//  includes creating the IR type as well as capturing variables and
//  performing calls.
//
//  Swift supports three representations of functions:
//
//    - thin, which are just a function pointer;
//
//    - thick, which are a pair of a function pointer and
//      an optional ref-counted opaque context pointer; and
//
//    - block, which match the Apple blocks extension: a ref-counted
//      pointer to a mostly-opaque structure with the function pointer
//      stored at a fixed offset.
//
//  The order of function parameters is as follows:
//
//    - indirect return pointer
//    - block context parameter, if applicable
//    - expanded formal parameter types
//    - implicit generic parameters
//    - thick context parameter, if applicable
//    - error result out-parameter, if applicable
//    - witness_method generic parameters, if applicable
//
//  The context and error parameters are last because they are
//  optional: we'd like to be able to turn a thin function into a
//  thick function, or a non-throwing function into a throwing one,
//  without adding a thunk.  A thick context parameter is required
//  (but can be passed undef) if an error result is required.
//
//  The additional generic parameters for witness methods follow the
//  same logic: we'd like to be able to use non-generic method
//  implementations directly as protocol witnesses if the rest of the
//  ABI matches up.
//
//  Note that some of this business with context parameters and error
//  results is just IR formalism; on most of our targets, both of
//  these are passed in registers.  This is also why passing them
//  as the final argument isn't bad for performance.
//
//  For now, function pointer types are always stored as opaque
//  pointers in LLVM IR; using a well-typed function type is
//  very challenging because of issues with recursive type expansion,
//  which can potentially introduce infinite types.  For example:
//    struct A {
//      var fn: (A) -> ()
//    }
//  Our CC lowering expands the fields of A into the argument list
//  of A.fn, which is necessarily infinite.  Attempting to use better
//  types when not in a situation like this would just make the
//  compiler complacent, leading to a long tail of undiscovered
//  crashes.  So instead we always store as i8* and require the
//  bitcast whenever we change representations.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/Linking.h"
#include "clang/AST/ASTContext.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Debug.h"

#include "BitPatternBuilder.h"
#include "Callee.h"
#include "ConstantBuilder.h"
#include "EnumPayload.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenFunc.h"
#include "GenHeap.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenPointerAuth.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "IndirectTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "Signature.h"
#include "IRGenMangler.h"

using namespace swift;
using namespace irgen;

namespace {
  /// Information about the IR-level signature of a function type.
  class FuncSignatureInfo {
  protected:
    /// The SIL function type being represented.
    const CanSILFunctionType FormalType;

    mutable Signature TheSignature;

  public:
    FuncSignatureInfo(CanSILFunctionType formalType)
      : FormalType(formalType) {}

    Signature getSignature(IRGenModule &IGM) const;
  };

  class ObjCFuncSignatureInfo : public FuncSignatureInfo {
  private:
    mutable Signature TheDirectSignature;

  public:
    ObjCFuncSignatureInfo(CanSILFunctionType formalType)
      : FuncSignatureInfo(formalType) {}

    Signature getDirectSignature(IRGenModule &IGM) const;
  };

  /// The @thin function type-info class.
  template <class Derived>
  class ThinFuncTypeInfoImpl :
    public PODSingleScalarTypeInfo<Derived, LoadableTypeInfo> {

  protected:
    const Derived &asDerived() const {
      return static_cast<const Derived &>(*this);
    }

    ThinFuncTypeInfoImpl(CanSILFunctionType formalType, llvm::Type *storageType,
                     Size size, Alignment align,
                     const SpareBitVector &spareBits)
      : PODSingleScalarTypeInfo<Derived, LoadableTypeInfo>(storageType, size, spareBits, align)
    {
    }

  public:
    TypeLayoutEntry *buildTypeLayoutEntry(IRGenModule &IGM,
                                          SILType T,
                                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(asDerived(), T);
      }
      return IGM.typeLayoutCache.getOrCreateScalarEntry(asDerived(), T,
                                                        ScalarKind::TriviallyDestroyable);
    }

    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }

    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return PointerInfo::forFunction(IGM).getExtraInhabitantCount(IGM);
    }

    APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                       unsigned bits,
                                       unsigned index) const override {
      return PointerInfo::forFunction(IGM)
               .getFixedExtraInhabitantValue(IGM, bits, index, 0);
    }

    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                         SILType T, bool isOutlined)
    const override {
      return PointerInfo::forFunction(IGF.IGM)
               .getExtraInhabitantIndex(IGF, src);
    }

    void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                              Address dest, SILType T, bool isOutlined)
    const override {
      return PointerInfo::forFunction(IGF.IGM)
               .storeExtraInhabitant(IGF, index, dest);
    }
  };

  /// The @thin function type-info class.
  class ThinFuncTypeInfo : public ThinFuncTypeInfoImpl<ThinFuncTypeInfo>,
                           public FuncSignatureInfo {
  public:

    ThinFuncTypeInfo(CanSILFunctionType formalType, llvm::Type *storageType,
                     Size size, Alignment align,
                     const SpareBitVector &spareBits) :
      ThinFuncTypeInfoImpl(formalType, storageType, size, align, spareBits),
      FuncSignatureInfo(formalType) {}


    static const ThinFuncTypeInfo *create(CanSILFunctionType formalType,
                                          llvm::Type *storageType,
                                          Size size, Alignment align,
                                          const SpareBitVector &spareBits) {
      return new ThinFuncTypeInfo(formalType, storageType, size, align,
                                  spareBits);
    }
    void initialize(IRGenFunction &IGF, Explosion &src, Address addr,
                    bool isOutlined) const override {
      auto *fn = src.claimNext();

      // We might be presented with a value of the more precise pointer to
      // function type "void(*)*" rather than the generic "i8*". Downcast to the
      // more general expected type.
      if (fn->getContext().supportsTypedPointers() &&
          fn->getType()->getNonOpaquePointerElementType()->isFunctionTy())
        fn = IGF.Builder.CreateBitCast(fn, getStorageType());

      Explosion tmp;
      tmp.add(fn);
      PODSingleScalarTypeInfo<ThinFuncTypeInfo,LoadableTypeInfo>::initialize(IGF, tmp, addr, isOutlined);
    }
  };

  /// The (objc_method) function type-info class.
  class ObjCFuncTypeInfo : public ThinFuncTypeInfoImpl<ThinFuncTypeInfo>,
                           public ObjCFuncSignatureInfo {
  public:

    ObjCFuncTypeInfo(CanSILFunctionType formalType, llvm::Type *storageType,
                     Size size, Alignment align,
                     const SpareBitVector &spareBits) :
      ThinFuncTypeInfoImpl(formalType, storageType, size, align, spareBits),
      ObjCFuncSignatureInfo(formalType) {}


    static const ObjCFuncTypeInfo *create(CanSILFunctionType formalType,
                                          llvm::Type *storageType,
                                          Size size, Alignment align,
                                          const SpareBitVector &spareBits) {
      return new ObjCFuncTypeInfo(formalType, storageType, size, align,
                                  spareBits);
    }
  };


  /// The @thick function type-info class.
  class FuncTypeInfo :
      public ScalarPairTypeInfo<FuncTypeInfo, ReferenceTypeInfo>,
      public FuncSignatureInfo {
  protected:
    FuncTypeInfo(CanSILFunctionType formalType, llvm::StructType *storageType,
                 Size size, Alignment align, SpareBitVector &&spareBits,
                 IsTriviallyDestroyable_t pod)
      : ScalarPairTypeInfo(storageType, size, std::move(spareBits), align, pod),
        FuncSignatureInfo(formalType)
    {
    }

  public:
    static const FuncTypeInfo *create(CanSILFunctionType formalType,
                                      llvm::StructType *storageType,
                                      Size size, Alignment align,
                                      SpareBitVector &&spareBits,
                                      IsTriviallyDestroyable_t pod) {
      return new FuncTypeInfo(formalType, storageType, size, align,
                              std::move(spareBits), pod);
    }
    
    // Function types do not satisfy allowsOwnership.
#define REF_STORAGE(Name, name, ...) \
    const TypeInfo * \
    create##Name##StorageType(TypeConverter &TC, \
                              bool isOptional) const override { \
      llvm_unreachable("[" #name "] function type"); \
    }
#include "swift/AST/ReferenceStorage.def"

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      } else if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
        return IGM.typeLayoutCache.getOrCreateScalarEntry(*this, T,
                                                           ScalarKind::TriviallyDestroyable);
      } else {
        return IGM.typeLayoutCache.getOrCreateScalarEntry(
            *this, T, ScalarKind::ThickFunc);
      }
    }

    static Size getFirstElementSize(IRGenModule &IGM) {
      return IGM.getPointerSize();
    }
    static StringRef getFirstElementLabel() {
      return ".fn";
    }
    static bool isFirstElementTrivial() {
      return true;
    }
    void emitRetainFirstElement(
        IRGenFunction &IGF, llvm::Value *fn,
        llvm::Optional<Atomicity> atomicity = llvm::None) const {}
    void emitReleaseFirstElement(
        IRGenFunction &IGF, llvm::Value *fn,
        llvm::Optional<Atomicity> atomicity = llvm::None) const {}
    void emitAssignFirstElement(IRGenFunction &IGF, llvm::Value *fn,
                                Address fnAddr) const {
      IGF.Builder.CreateStore(fn, fnAddr);
    }

    static Size getSecondElementOffset(IRGenModule &IGM) {
      return IGM.getPointerSize();
    }
    static Size getSecondElementSize(IRGenModule &IGM) {
      return IGM.getPointerSize();
    }
    static StringRef getSecondElementLabel() {
      return ".data";
    }
    bool isSecondElementTrivial() const {
      return isTriviallyDestroyable(ResilienceExpansion::Maximal);
    }
    void emitRetainSecondElement(
        IRGenFunction &IGF, llvm::Value *data,
        llvm::Optional<Atomicity> atomicity = llvm::None) const {
      if (!isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
        if (!atomicity) atomicity = IGF.getDefaultAtomicity();
        IGF.emitNativeStrongRetain(data, *atomicity);
      }
    }
    void emitReleaseSecondElement(
        IRGenFunction &IGF, llvm::Value *data,
        llvm::Optional<Atomicity> atomicity = llvm::None) const {
      if (!isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
        if (!atomicity) atomicity = IGF.getDefaultAtomicity();
        IGF.emitNativeStrongRelease(data, *atomicity);
      }
    }
    void emitAssignSecondElement(IRGenFunction &IGF, llvm::Value *context,
                                 Address dataAddr) const {
      if (isTriviallyDestroyable(ResilienceExpansion::Maximal))
        IGF.Builder.CreateStore(context, dataAddr);
      else
        IGF.emitNativeStrongAssign(context, dataAddr);
    }

    Address projectFunction(IRGenFunction &IGF, Address address) const {
      return projectFirstElement(IGF, address);
    }

    Address projectData(IRGenFunction &IGF, Address address) const {
      return IGF.Builder.CreateStructGEP(address, 1, IGF.IGM.getPointerSize(),
                                         address->getName() + ".data");
    }

    void strongRetain(IRGenFunction &IGF, Explosion &e,
                      Atomicity atomicity) const override {
      e.claimNext();
      emitRetainSecondElement(IGF, e.claimNext(), atomicity);
    }

    void strongRelease(IRGenFunction &IGF, Explosion &e,
                       Atomicity atomicity) const override {
      e.claimNext();
      emitReleaseSecondElement(IGF, e.claimNext(), atomicity);
    }

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
    void name##LoadStrong(IRGenFunction &IGF, Address src, \
                          Explosion &out, bool isOptional) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void name##TakeStrong(IRGenFunction &IGF, Address src, \
                          Explosion &out, bool isOptional) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void name##Init(IRGenFunction &IGF, Explosion &in, \
                    Address dest, bool isOptional) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void name##Assign(IRGenFunction &IGF, Explosion &in, \
                       Address dest, bool isOptional) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
    void strongRetain##Name(IRGenFunction &IGF, Explosion &e, \
                            Atomicity atomicity) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void strongRetain##Name##Release(IRGenFunction &IGF, \
                                     Explosion &e, \
                                     Atomicity atomicity) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void name##Retain(IRGenFunction &IGF, Explosion &e, \
                       Atomicity atomicity) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    } \
    void name##Release(IRGenFunction &IGF, Explosion &e, \
                        Atomicity atomicity) const override { \
      llvm_unreachable(#name " references to functions are not supported"); \
    }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
    NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...") \
    ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...")
#include "swift/AST/ReferenceStorage.def"

    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }

    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return PointerInfo::forFunction(IGM)
               .getExtraInhabitantCount(IGM);
    }

    APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                       unsigned bits,
                                       unsigned index) const override {
      return PointerInfo::forFunction(IGM)
               .getFixedExtraInhabitantValue(IGM, bits, index, 0);
    }

    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                         SILType T, bool isOutlined)
    const override {
      return PointerInfo::forFunction(IGF.IGM)
               .getExtraInhabitantIndex(IGF, projectFunction(IGF, src));
    }

    void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                              Address dest, SILType T, bool isOutlined)
    const override {
      return PointerInfo::forFunction(IGF.IGM)
               .storeExtraInhabitant(IGF, index, projectFunction(IGF, dest));
    }

    APInt getFixedExtraInhabitantMask(IRGenModule &IGM) const override {
      // Only the function pointer value is used for extra inhabitants.
      auto pointerSize = IGM.getPointerSize();
      auto mask = BitPatternBuilder(IGM.Triple.isLittleEndian());
      mask.appendSetBits(pointerSize.getValueInBits());
      mask.appendClearBits(pointerSize.getValueInBits());
      return mask.build().value();
    }
  };

  /// The type-info class for ObjC blocks, which are represented by an ObjC
  /// heap pointer.
  class BlockTypeInfo : public HeapTypeInfo<BlockTypeInfo>,
                        public FuncSignatureInfo
  {
  public:
    BlockTypeInfo(CanSILFunctionType ty,
                  llvm::PointerType *storageType,
                  Size size, SpareBitVector spareBits, Alignment align)
      : HeapTypeInfo(ReferenceCounting::Block, storageType, size, spareBits,
                     align),
        FuncSignatureInfo(ty) {}

    ReferenceCounting getReferenceCounting() const {
      return ReferenceCounting::Block;
    }
    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      return IGM.typeLayoutCache.getOrCreateScalarEntry(
          *this, T, ScalarKind::BlockReference);
    }
  };
  
  /// The type info class for the on-stack representation of an ObjC block.
  ///
  /// TODO: May not be fixed-layout if we capture generics.
  class BlockStorageTypeInfo final
    : public IndirectTypeInfo<BlockStorageTypeInfo, FixedTypeInfo>
  {
    Size CaptureOffset;
  public:
    BlockStorageTypeInfo(llvm::Type *type, Size size, Alignment align,
                         SpareBitVector &&spareBits,
                         IsTriviallyDestroyable_t pod, IsBitwiseTakable_t bt, Size captureOffset)
      : IndirectTypeInfo(type, size, std::move(spareBits), align, pod, bt,
                         IsCopyable,
                         IsFixedSize),
        CaptureOffset(captureOffset)
    {}
    
    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      return IGM.typeLayoutCache.getOrCreateScalarEntry(
          *this, T, ScalarKind::BlockStorage);
    }
    // The lowered type should be an LLVM struct comprising the block header
    // (IGM.ObjCBlockStructTy) as its first element and the capture as its
    // second.
    
    Address projectBlockHeader(IRGenFunction &IGF, Address storage) const {
      return IGF.Builder.CreateStructGEP(storage, 0, Size(0));
    }
    
    Address projectCapture(IRGenFunction &IGF, Address storage) const {
      return IGF.Builder.CreateStructGEP(storage, 1, CaptureOffset);
    }
    
    // TODO
    // The frontend will currently never emit copy_addr or destroy_addr for
    // block storage.

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        SILType T, bool isOutlined) const override {
      IGF.unimplemented(SourceLoc(), "copying @block_storage");
    }
    void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                            SILType T, bool isOutlined) const override {
      IGF.unimplemented(SourceLoc(), "copying @block_storage");
    }
    void destroy(IRGenFunction &IGF, Address addr, SILType T,
                 bool isOutlined) const override {
      IGF.unimplemented(SourceLoc(), "destroying @block_storage");
    }
  };
} // end anonymous namespace

const TypeInfo *TypeConverter::convertBlockStorageType(SILBlockStorageType *T) {
  // The block storage consists of the block header (ObjCBlockStructTy)
  // followed by the lowered type of the capture.
  auto &capture = IGM.getTypeInfoForLowered(T->getCaptureType());
  
  // TODO: Support dynamic-sized captures.
  const auto *fixedCapture = dyn_cast<FixedTypeInfo>(&capture);
  llvm::Type *fixedCaptureTy;
  // The block header is pointer aligned. The capture may be worse aligned.
  Alignment align = IGM.getPointerAlignment();
  Size captureOffset(
    IGM.DataLayout.getStructLayout(IGM.ObjCBlockStructTy)->getSizeInBytes());
  auto spareBits = BitPatternBuilder(IGM.Triple.isLittleEndian());
  spareBits.appendClearBits(captureOffset.getValueInBits());

  Size size = captureOffset;
  IsTriviallyDestroyable_t pod = IsNotTriviallyDestroyable;
  IsBitwiseTakable_t bt = IsNotBitwiseTakable;
  if (!fixedCapture) {
    IGM.unimplemented(SourceLoc(), "dynamic @block_storage capture");
    fixedCaptureTy = llvm::StructType::get(IGM.getLLVMContext(), {});
  } else {
    fixedCaptureTy = cast<FixedTypeInfo>(capture).getStorageType();
    align = std::max(align, fixedCapture->getFixedAlignment());
    captureOffset = captureOffset.roundUpToAlignment(align);
    spareBits.padWithSetBitsTo(captureOffset.getValueInBits());
    spareBits.append(fixedCapture->getSpareBits());

    size = captureOffset + fixedCapture->getFixedSize();
    pod = fixedCapture->isTriviallyDestroyable(ResilienceExpansion::Maximal);
    bt = fixedCapture->isBitwiseTakable(ResilienceExpansion::Maximal);
  }

  llvm::Type *storageElts[] = {
    IGM.ObjCBlockStructTy,
    fixedCaptureTy,
  };

  auto storageTy = llvm::StructType::get(IGM.getLLVMContext(), storageElts,
                                         /*packed*/ false);
  return new BlockStorageTypeInfo(storageTy, size, align, spareBits.build(),
                                  pod, bt, captureOffset);
}

Address irgen::projectBlockStorageCapture(IRGenFunction &IGF,
                                          Address storageAddr,
                                          CanSILBlockStorageType storageTy) {
  auto &tl = IGF.getTypeInfoForLowered(storageTy).as<BlockStorageTypeInfo>();
  return tl.projectCapture(IGF, storageAddr);
}

const TypeInfo *TypeConverter::convertFunctionType(SILFunctionType *T) {
  // Handle `@differentiable` functions.
  switch (T->getDifferentiabilityKind()) {
  // TODO: Ban `Normal` and `Forward` cases.
  case DifferentiabilityKind::Normal:
  case DifferentiabilityKind::Reverse:
  case DifferentiabilityKind::Forward:
    return convertNormalDifferentiableFunctionType(T);
  case DifferentiabilityKind::Linear:
    return convertLinearDifferentiableFunctionType(T);
  case DifferentiabilityKind::NonDifferentiable:
    break;
  }

  switch (T->getRepresentation()) {
  case SILFunctionType::Representation::Block:
    return new BlockTypeInfo(CanSILFunctionType(T),
                             IGM.ObjCBlockPtrTy,
                             IGM.getPointerSize(),
                             IGM.getHeapObjectSpareBits(),
                             IGM.getPointerAlignment());
      
  case SILFunctionType::Representation::Thin:
  case SILFunctionType::Representation::Method:
  case SILFunctionType::Representation::CXXMethod:
  case SILFunctionType::Representation::WitnessMethod:
  case SILFunctionType::Representation::CFunctionPointer:
  case SILFunctionType::Representation::Closure:
    return ThinFuncTypeInfo::create(CanSILFunctionType(T),
                                    IGM.FunctionPtrTy,
                                    IGM.getPointerSize(),
                                    IGM.getPointerAlignment(),
                                    IGM.getFunctionPointerSpareBits());
  case SILFunctionType::Representation::ObjCMethod:
    return ObjCFuncTypeInfo::create(CanSILFunctionType(T),
                                    IGM.FunctionPtrTy,
                                    IGM.getPointerSize(),
                                    IGM.getPointerAlignment(),
                                    IGM.getFunctionPointerSpareBits());

  case SILFunctionType::Representation::Thick: {
    SpareBitVector spareBits;
    spareBits.append(IGM.getFunctionPointerSpareBits());
    // Although the context pointer of a closure (at least, an escaping one)
    // is a refcounted pointer, we'd like to reserve the right to pack small
    // contexts into the pointer value, so let's not take any spare bits from
    // it.
    spareBits.appendClearBits(IGM.getPointerSize().getValueInBits());
    
    if (T->isNoEscape()) {
      // @noescape thick functions are trivial types.
      return FuncTypeInfo::create(
          CanSILFunctionType(T), IGM.NoEscapeFunctionPairTy,
          IGM.getPointerSize() * 2, IGM.getPointerAlignment(),
          std::move(spareBits), IsTriviallyDestroyable);
    }
    return FuncTypeInfo::create(
        CanSILFunctionType(T), IGM.FunctionPairTy, IGM.getPointerSize() * 2,
        IGM.getPointerAlignment(), std::move(spareBits), IsNotTriviallyDestroyable);
  }
  }
  llvm_unreachable("bad function type representation");
}

Signature FuncSignatureInfo::getSignature(IRGenModule &IGM) const {
  // If it's already been filled in, we're done.
  if (TheSignature.isValid())
    return TheSignature;

  // Update the cache and return.
  TheSignature = Signature::getUncached(IGM, FormalType,
                                        FunctionPointerKind(FormalType));
  assert(TheSignature.isValid());
  return TheSignature;
}

Signature ObjCFuncSignatureInfo::getDirectSignature(IRGenModule &IGM) const {
  // If it's already been filled in, we're done.
  if (TheDirectSignature.isValid())
    return TheDirectSignature;

  // Update the cache and return.
  TheDirectSignature = Signature::getUncached(IGM, FormalType,
                                        FunctionPointerKind(FormalType),
                                        /*forStaticCall*/ true);
  assert(TheDirectSignature.isValid());
  return TheDirectSignature;
}

static const FuncSignatureInfo &
getFuncSignatureInfoForLowered(IRGenModule &IGM, CanSILFunctionType type) {
  auto &ti = IGM.getTypeInfoForLowered(type);
  switch (type->getRepresentation()) {
  case SILFunctionType::Representation::Block:
    return ti.as<BlockTypeInfo>();
  case SILFunctionType::Representation::Thin:
  case SILFunctionType::Representation::CFunctionPointer:
  case SILFunctionType::Representation::Method:
  case SILFunctionType::Representation::CXXMethod:
  case SILFunctionType::Representation::WitnessMethod:
  case SILFunctionType::Representation::Closure:
    return ti.as<ThinFuncTypeInfo>();
  case SILFunctionType::Representation::ObjCMethod:
    return static_cast<const FuncSignatureInfo &>(ti.as<ObjCFuncTypeInfo>());
  case SILFunctionType::Representation::Thick:
    return ti.as<FuncTypeInfo>();
  }
  llvm_unreachable("bad function type representation");
}

Signature IRGenModule::getSignature(CanSILFunctionType type) {
  return getSignature(type, FunctionPointerKind(type));
}

Signature IRGenModule::getSignature(CanSILFunctionType type,
                                    FunctionPointerKind kind,
                                    bool forStaticCall) {
  // Don't bother caching if we're working with a special kind.
  if (kind.isSpecial())
    return Signature::getUncached(*this, type, kind);

  auto &sigInfo = getFuncSignatureInfoForLowered(*this, type);

  if (forStaticCall &&
      type->getRepresentation() == SILFunctionType::Representation::ObjCMethod) {
    auto &objcSigInfo = static_cast<const ObjCFuncSignatureInfo &>(sigInfo);
    return objcSigInfo.getDirectSignature(*this);
  }
  return sigInfo.getSignature(*this);
}

llvm::FunctionType *
IRGenModule::getFunctionType(CanSILFunctionType type,
                             llvm::AttributeList &attrs,
                             ForeignFunctionInfo *foreignInfo) {
  auto &sigInfo = getFuncSignatureInfoForLowered(*this, type);
  Signature sig = sigInfo.getSignature(*this);
  attrs = sig.getAttributes();
  if (foreignInfo) *foreignInfo = sig.getForeignInfo();
  return sig.getType();
}

ForeignFunctionInfo
IRGenModule::getForeignFunctionInfo(CanSILFunctionType type) {
  if (type->getLanguage() == SILFunctionLanguage::Swift)
    return ForeignFunctionInfo();

  auto &sigInfo = getFuncSignatureInfoForLowered(*this, type);
  return sigInfo.getSignature(*this).getForeignInfo();
}

static void emitApplyArgument(IRGenFunction &IGF,
                              CanSILFunctionType origFnTy,
                              SILParameterInfo origParam,
                              CanSILFunctionType substFnTy,
                              SILParameterInfo substParam,
                              Explosion &in,
                              Explosion &out) {
  auto silConv = IGF.IGM.silConv;
  auto context = IGF.IGM.getMaximalTypeExpansionContext();
  bool isSubstituted =
      (silConv.getSILType(substParam, substFnTy, context)
         != silConv.getSILType(origParam, origFnTy, context));

  // For indirect arguments, we just need to pass a pointer.
  if (silConv.isSILIndirect(origParam)) {
    // This address is of the substituted type.
    auto addr = in.claimNext();
    
    // If a substitution is in play, just bitcast the address.
    if (isSubstituted) {
      auto origType = IGF.IGM.getStoragePointerType(
          silConv.getSILType(origParam, origFnTy, context));
      addr = IGF.Builder.CreateBitCast(addr, origType);
    }
    
    out.add(addr);
    return;
  }
  assert(!silConv.isSILIndirect(origParam)
         && "Unexpected opaque apply parameter.");

  // Otherwise, it's an explosion, which we may need to translate,
  // both in terms of explosion level and substitution levels.

  // Handle the last unsubstituted case.
  if (!isSubstituted) {
    auto &substArgTI = cast<LoadableTypeInfo>(
        IGF.getTypeInfo(silConv.getSILType(substParam, substFnTy, context)));
    substArgTI.reexplode(in, out);
    return;
  }

  reemitAsUnsubstituted(IGF, silConv.getSILType(origParam, origFnTy, context),
                        silConv.getSILType(substParam, substFnTy, context), in,
                        out);
}

CanType irgen::getArgumentLoweringType(CanType type, SILParameterInfo paramInfo,
                                       bool isNoEscape) {
  switch (paramInfo.getConvention()) {
  // Capture value parameters by value, consuming them.
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    return type;

  // Capture pack parameters by value (a pointer).
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    return type;

  // Capture indirect parameters if the closure is not [onstack]. [onstack]
  // closures don't take ownership of their arguments so we just capture the
  // address.
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
    if (isNoEscape)
      return CanInOutType::get(type);
    else
      return type;

  // Capture inout parameters by pointer.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return CanInOutType::get(type);
  }
  llvm_unreachable("unhandled convention");
}

static bool isABIIgnoredParameterWithoutStorage(IRGenModule &IGM,
                                                IRGenFunction &IGF,
                                                CanSILFunctionType substType,
                                                unsigned paramIdx) {
  auto param = substType->getParameters()[paramIdx];
  if (param.isFormalIndirect())
    return false;

  SILType argType = IGM.silConv.getSILType(
      param, substType, IGM.getMaximalTypeExpansionContext());
  auto &ti = IGF.getTypeInfoForLowered(argType.getASTType());
  // Empty values don't matter.
  return ti.getSchema().empty();
}

/// Find the parameter index for the one (assuming there was only one) partially
/// applied argument ignoring empty types that are not passed as part of the
/// ABI.
static unsigned findSinglePartiallyAppliedParameterIndexIgnoringEmptyTypes(
    IRGenFunction &IGF, CanSILFunctionType substType,
    CanSILFunctionType outType) {
  auto substParameters = substType->getParameters();
  auto outParameters = outType->getParameters();
  unsigned firstNonEmpty = -1U;
  for (unsigned paramIdx = outParameters.size() ; paramIdx != substParameters.size(); ++paramIdx) {
    bool isEmpty =
        isABIIgnoredParameterWithoutStorage(IGF.IGM, IGF, substType, paramIdx);
    assert((isEmpty || firstNonEmpty == -1U) && "Expect at most one partially "
                                                "applied that is passed as an "
                                                "ABI argument");
    if (!isEmpty)
      firstNonEmpty = paramIdx;
  }
  assert(firstNonEmpty != -1U);
  return firstNonEmpty;
}

namespace {
class PartialApplicationForwarderEmission {
protected:
  IRGenModule &IGM;
  IRGenFunction &subIGF;
  llvm::Function *fwd;
  const llvm::Optional<FunctionPointer> &staticFnPtr;
  bool calleeHasContext;
  const Signature &origSig;
  CanSILFunctionType origType;
  CanSILFunctionType substType;
  CanSILFunctionType outType;
  SubstitutionMap subs;
  HeapLayout const *layout;
  const ArrayRef<ParameterConvention> conventions;
  SILFunctionConventions origConv;
  SILFunctionConventions outConv;
  Explosion origParams;

  // Create a new explosion for potentially reabstracted parameters.
  Explosion args;
  Address resultValueAddr;

  PartialApplicationForwarderEmission(
      IRGenModule &IGM, IRGenFunction &subIGF, llvm::Function *fwd,
      const llvm::Optional<FunctionPointer> &staticFnPtr, bool calleeHasContext,
      const Signature &origSig, CanSILFunctionType origType,
      CanSILFunctionType substType, CanSILFunctionType outType,
      SubstitutionMap subs, HeapLayout const *layout,
      ArrayRef<ParameterConvention> conventions)
      : IGM(IGM), subIGF(subIGF), fwd(fwd), staticFnPtr(staticFnPtr),
        calleeHasContext(calleeHasContext), origSig(origSig),
        origType(origType), substType(substType), outType(outType), subs(subs),
        conventions(conventions), origConv(origType, IGM.getSILModule()),
        outConv(outType, IGM.getSILModule()),
        origParams(subIGF.collectParameters()) {}

public:
  virtual void begin(){};

  virtual void gatherArgumentsFromApply() = 0;

  virtual void mapAsyncParameters(FunctionPointer fnPtr) {}
  virtual void recordAsyncParametersInsertionPoint(){};

  void gatherArgumentsFromApply(bool isAsync) {
    // Lower the forwarded arguments in the original function's generic context.
    GenericContextScope scope(IGM, origType->getInvocationGenericSignature());

    SILFunctionConventions origConv(origType, IGM.getSILModule());
    auto &outResultTI = IGM.getTypeInfo(
        outConv.getSILResultType(IGM.getMaximalTypeExpansionContext()));
    auto &nativeResultSchema = outResultTI.nativeReturnValueSchema(IGM);
    auto &origResultTI = IGM.getTypeInfo(
        origConv.getSILResultType(IGM.getMaximalTypeExpansionContext()));
    auto &origNativeSchema = origResultTI.nativeReturnValueSchema(IGM);

    // Forward the indirect return values. We might have to reabstract the
    // return value.
    bool useSRet = !isAsync;
    if (nativeResultSchema.requiresIndirect()) {
      assert(origNativeSchema.requiresIndirect());
      auto resultAddr = origParams.claimNext();
      resultAddr = subIGF.Builder.CreateBitCast(
          resultAddr, IGM.getStoragePointerType(origConv.getSILResultType(
                          IGM.getMaximalTypeExpansionContext())));
      args.add(resultAddr);
      useSRet = false;
    } else if (origNativeSchema.requiresIndirect()) {
      assert(!nativeResultSchema.requiresIndirect());
      auto stackAddr = outResultTI.allocateStack(
          subIGF,
          outConv.getSILResultType(IGM.getMaximalTypeExpansionContext()),
          "return.temp");
      resultValueAddr = stackAddr.getAddress();
      auto resultAddr = subIGF.Builder.CreateElementBitCast(
          resultValueAddr, IGM.getStorageType(origConv.getSILResultType(
                               IGM.getMaximalTypeExpansionContext())));
      args.add(resultAddr.getAddress());
      useSRet = false;
    } else if (!origNativeSchema.empty()) {
      useSRet = false;
    }
    useSRet = useSRet && origConv.getNumIndirectSILResults() == 1;
    for (auto resultType : origConv.getIndirectSILResultTypes(
             IGM.getMaximalTypeExpansionContext())) {
      auto addr = origParams.claimNext();
      addr = subIGF.Builder.CreateBitCast(
          addr, IGM.getStoragePointerType(resultType));
      auto useOpaque =
          useSRet && !isa<FixedTypeInfo>(IGM.getTypeInfo(resultType));
      if (useOpaque)
        addr = subIGF.Builder.CreateBitCast(addr, IGM.OpaquePtrTy);
      args.add(addr);
      useSRet = false;
    }

    if (isAsync)
      recordAsyncParametersInsertionPoint();

    // Reemit the parameters as unsubstituted.
    for (unsigned i = 0; i < outType->getParameters().size(); ++i) {
      auto origParamInfo = origType->getParameters()[i];
      auto &ti = IGM.getTypeInfoForLowered(origParamInfo.getArgumentType(
          IGM.getSILModule(), origType, IGM.getMaximalTypeExpansionContext()));
      auto schema = ti.getSchema();

      auto origParamSILType = IGM.silConv.getSILType(
          origParamInfo, origType, IGM.getMaximalTypeExpansionContext());
      // Forward the address of indirect value params.
      auto &nativeSchemaOrigParam = ti.nativeParameterValueSchema(IGM);
      bool isIndirectParam = origConv.isSILIndirect(origParamInfo);
      if (!isIndirectParam && nativeSchemaOrigParam.requiresIndirect()) {
        auto addr = origParams.claimNext();
        if (addr->getType() != ti.getStorageType()->getPointerTo())
          addr = subIGF.Builder.CreateBitCast(addr,
                                           ti.getStorageType()->getPointerTo());
        args.add(addr);
        continue;
      }

      auto outTypeParamInfo = outType->getParameters()[i];
      // Indirect parameters need no mapping through the native calling
      // convention.
      if (isIndirectParam) {
        emitApplyArgument(subIGF,
                          origType,
                          origParamInfo,
                          outType,
                          outTypeParamInfo,
                          origParams, args);
        continue;
      }

      // Map from the native calling convention into the explosion schema.
      auto outTypeParamSILType = IGM.silConv.getSILType(
          origParamInfo, origType, IGM.getMaximalTypeExpansionContext());
      auto &nativeSchemaOutTypeParam =
          IGM.getTypeInfo(outTypeParamSILType).nativeParameterValueSchema(IGM);
      Explosion nativeParam;
      origParams.transferInto(nativeParam, nativeSchemaOutTypeParam.size());

      bindPolymorphicParameter(subIGF, origType, substType, nativeParam, i);

      Explosion nonNativeParam = nativeSchemaOutTypeParam.mapFromNative(
          subIGF.IGM, subIGF, nativeParam, outTypeParamSILType);
      assert(nativeParam.empty());

      // Emit unsubstituted argument for call.
      Explosion nonNativeApplyArg;
      emitApplyArgument(subIGF,
                        origType, origParamInfo,
                        outType, outTypeParamInfo,
                        nonNativeParam,
                        nonNativeApplyArg);
      assert(nonNativeParam.empty());
      // Map back from the explosion scheme to the native calling convention for
      // the call.
      Explosion nativeApplyArg = nativeSchemaOrigParam.mapIntoNative(
          subIGF.IGM, subIGF, nonNativeApplyArg, origParamSILType, false);
      assert(nonNativeApplyArg.empty());
      nativeApplyArg.transferInto(args, nativeApplyArg.size());
    }
  }

  unsigned getCurrentArgumentIndex() { return args.size(); }

  bool transformArgumentToNative(SILParameterInfo origParamInfo, Explosion &in,
                                 Explosion &out) {
    return addNativeArgument(subIGF, in, origType, origParamInfo, out, false);
  }
  void addArgument(Explosion &explosion) {
    args.add(explosion.claimAll());
  }
  void addArgument(llvm::Value *argValue) { args.add(argValue); }
  void addArgument(Explosion &explosion, unsigned index) {
    addArgument(explosion);
  }
  void addArgument(llvm::Value *argValue, unsigned index) {
    addArgument(argValue);
  }

  SILParameterInfo getParameterInfo(unsigned index) {
    return substType->getParameters()[index];
  }

  llvm::Value *getContext() { return origParams.claimNext(); }

  virtual llvm::Value *getDynamicFunctionPointer() = 0;
  virtual llvm::Value *getDynamicFunctionContext() = 0;
  virtual void addDynamicFunctionContext(Explosion &explosion) = 0;
  virtual void addDynamicFunctionPointer(Explosion &explosion) = 0;

  void addSelf(Explosion &explosion) { addArgument(explosion); }
  void addWitnessSelfMetadata(llvm::Value *value) {
    addArgument(value);
  }
  void addWitnessSelfWitnessTable(llvm::Value *value) {
    addArgument(value);
  }
  virtual void forwardErrorResult() = 0;
  bool originalParametersConsumed() { return origParams.empty(); }
  void addPolymorphicArguments(Explosion polyArgs) {
    polyArgs.transferInto(args, polyArgs.size());
  }
  virtual llvm::CallInst *createCall(FunctionPointer &fnPtr) = 0;
  virtual void createReturn(llvm::CallInst *call) = 0;
  virtual void end(){};
  virtual ~PartialApplicationForwarderEmission() {}
};
class SyncPartialApplicationForwarderEmission
    : public PartialApplicationForwarderEmission {
  using super = PartialApplicationForwarderEmission;

public:
  SyncPartialApplicationForwarderEmission(
      IRGenModule &IGM, IRGenFunction &subIGF, llvm::Function *fwd,
      const llvm::Optional<FunctionPointer> &staticFnPtr, bool calleeHasContext,
      const Signature &origSig, CanSILFunctionType origType,
      CanSILFunctionType substType, CanSILFunctionType outType,
      SubstitutionMap subs, HeapLayout const *layout,
      ArrayRef<ParameterConvention> conventions)
      : PartialApplicationForwarderEmission(
            IGM, subIGF, fwd, staticFnPtr, calleeHasContext, origSig, origType,
            substType, outType, subs, layout, conventions) {}

  void begin() override { super::begin(); }
  void gatherArgumentsFromApply() override {
    super::gatherArgumentsFromApply(false);
  }
  llvm::Value *getDynamicFunctionPointer() override { return args.takeLast(); }
  llvm::Value *getDynamicFunctionContext() override { return args.takeLast(); }
  void addDynamicFunctionContext(Explosion &explosion) override {
    addArgument(explosion);
  }
  void addDynamicFunctionPointer(Explosion &explosion) override {
    addArgument(explosion);
  }
  void forwardErrorResult() override {
    llvm::Value *errorResultPtr = origParams.claimNext();
    args.add(errorResultPtr);
  }
  llvm::CallInst *createCall(FunctionPointer &fnPtr) override {
    return subIGF.Builder.CreateCall(fnPtr, args.claimAll());
  }
  void createReturn(llvm::CallInst *call) override {
    // Reabstract the result value as substituted.
    SILFunctionConventions origConv(origType, IGM.getSILModule());
    auto &outResultTI = IGM.getTypeInfo(
        outConv.getSILResultType(IGM.getMaximalTypeExpansionContext()));
    auto &nativeResultSchema = outResultTI.nativeReturnValueSchema(IGM);
    if (call->getType()->isVoidTy()) {
      if (!resultValueAddr.isValid())
        subIGF.Builder.CreateRetVoid();
      else {
        // Okay, we have called a function that expects an indirect return type
        // but the partially applied return type is direct.
        assert(!nativeResultSchema.requiresIndirect());
        Explosion loadedResult;
        cast<LoadableTypeInfo>(outResultTI)
            .loadAsTake(subIGF, resultValueAddr, loadedResult);
        Explosion nativeResult = nativeResultSchema.mapIntoNative(
            IGM, subIGF, loadedResult,
            outConv.getSILResultType(IGM.getMaximalTypeExpansionContext()),
            false);
        outResultTI.deallocateStack(
            subIGF, resultValueAddr,
            outConv.getSILResultType(IGM.getMaximalTypeExpansionContext()));
        if (nativeResult.size() == 1)
          subIGF.Builder.CreateRet(nativeResult.claimNext());
        else {
          llvm::Value *nativeAgg =
              llvm::UndefValue::get(nativeResultSchema.getExpandedType(IGM));
          for (unsigned i = 0, e = nativeResult.size(); i != e; ++i) {
            auto *elt = nativeResult.claimNext();
            nativeAgg = subIGF.Builder.CreateInsertValue(nativeAgg, elt, i);
          }
          subIGF.Builder.CreateRet(nativeAgg);
        }
      }
    } else {
      llvm::Value *callResult = call;
      // If the result type is dependent on a type parameter we might have to
      // cast to the result type - it could be substituted.
      if (origConv.getSILResultType(IGM.getMaximalTypeExpansionContext())
              .hasTypeParameter()) {
        auto ResType = fwd->getReturnType();
        if (ResType != callResult->getType())
          callResult =
              subIGF.coerceValue(callResult, ResType, subIGF.IGM.DataLayout);
      }
      subIGF.Builder.CreateRet(callResult);
    }
  }
  void end() override { super::end(); }
};
class AsyncPartialApplicationForwarderEmission
    : public PartialApplicationForwarderEmission {
  using super = PartialApplicationForwarderEmission;
  AsyncContextLayout layout;
  llvm::Value *calleeFunction;
  llvm::Value *currentResumeFn;
  Size contextSize;
  Address context;
  Address calleeContextBuffer;
  unsigned currentArgumentIndex;
  struct Self {
    enum class Kind {
      Method,
      WitnessMethod,
    };
    Kind kind;
    llvm::Value *value;
  };
  llvm::Optional<Self> self = llvm::None;
  unsigned asyncParametersInsertionIndex = 0;

  void saveValue(ElementLayout layout, Explosion &explosion) {
    Address addr = layout.project(subIGF, context, /*offsets*/ llvm::None);
    auto &ti = cast<LoadableTypeInfo>(layout.getType());
    ti.initialize(subIGF, explosion, addr, /*isOutlined*/ false);
  }

public:
  AsyncPartialApplicationForwarderEmission(
      IRGenModule &IGM, IRGenFunction &subIGF, llvm::Function *fwd,
      const llvm::Optional<FunctionPointer> &staticFnPtr, bool calleeHasContext,
      const Signature &origSig, CanSILFunctionType origType,
      CanSILFunctionType substType, CanSILFunctionType outType,
      SubstitutionMap subs, HeapLayout const *layout,
      ArrayRef<ParameterConvention> conventions)
      : PartialApplicationForwarderEmission(
            IGM, subIGF, fwd, staticFnPtr, calleeHasContext, origSig, origType,
            substType, outType, subs, layout, conventions),
        layout(getAsyncContextLayout(subIGF.IGM, origType, substType, subs)),
        currentArgumentIndex(outType->getNumParameters()) {}

  void begin() override { super::begin(); }

  void recordAsyncParametersInsertionPoint() override {
    // Ignore the original context.
    (void)origParams.claimNext();

    asyncParametersInsertionIndex = args.size();
  }
  void mapAsyncParameters(FunctionPointer fnPtr) override {
    llvm::Value *dynamicContextSize32;
    std::tie(calleeFunction, dynamicContextSize32) = getAsyncFunctionAndSize(
        subIGF, origType->getRepresentation(), fnPtr, nullptr,
        std::make_pair(true, true));
    auto *dynamicContextSize =
        subIGF.Builder.CreateZExt(dynamicContextSize32, subIGF.IGM.SizeTy);
    calleeContextBuffer =
        emitAllocAsyncContext(subIGF, dynamicContextSize);
    context = layout.emitCastTo(subIGF, calleeContextBuffer.getAddress());
    auto calleeContext =
        layout.emitCastTo(subIGF, calleeContextBuffer.getAddress());
    args.insert(asyncParametersInsertionIndex,
                subIGF.Builder.CreateBitOrPointerCast(
                    calleeContextBuffer.getAddress(), IGM.SwiftContextPtrTy));

    // Set caller info into the context.
    { // caller context
      Explosion explosion;
      auto fieldLayout = layout.getParentLayout();
      auto *context = subIGF.getAsyncContext();
      if (auto schema =
              subIGF.IGM.getOptions().PointerAuth.AsyncContextParent) {
        Address fieldAddr =
            fieldLayout.project(subIGF, calleeContext, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            subIGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        context = emitPointerAuthSign(subIGF, context, authInfo);
      }
      explosion.add(context);
      saveValue(fieldLayout, explosion);
    }
    { // Return to caller function.
      auto fieldLayout = layout.getResumeParentLayout();
      currentResumeFn = subIGF.Builder.CreateIntrinsicCall(
          llvm::Intrinsic::coro_async_resume, {});
      auto fnVal = currentResumeFn;
      // Sign the pointer.
      if (auto schema = subIGF.IGM.getOptions().PointerAuth.AsyncContextResume) {
        Address fieldAddr =
            fieldLayout.project(subIGF, calleeContext, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            subIGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        fnVal = emitPointerAuthSign(subIGF, fnVal, authInfo);
      }
      fnVal = subIGF.Builder.CreateBitCast(
          fnVal, subIGF.IGM.TaskContinuationFunctionPtrTy);
      Explosion explosion;
      explosion.add(fnVal);
      saveValue(fieldLayout, explosion);
    }
  }
  void gatherArgumentsFromApply() override {
    super::gatherArgumentsFromApply(true);
  }
  llvm::Value *getDynamicFunctionPointer() override { return args.takeLast(); }
  llvm::Value *getDynamicFunctionContext() override {
    return args.takeLast();
  }
  void addDynamicFunctionContext(Explosion &explosion) override {
    addArgument(explosion);
  }
  void addDynamicFunctionPointer(Explosion &explosion) override {
    addArgument(explosion);
  }

  void forwardErrorResult() override {
    // Nothing to do here.  The error result pointer is already in the
    // appropriate position.
  }
  llvm::CallInst *createCall(FunctionPointer &fnPtr) override {
    PointerAuthInfo newAuthInfo =
        fnPtr.getAuthInfo().getCorrespondingCodeAuthInfo();
    auto newFnPtr = FunctionPointer::createSigned(
        FunctionPointer::Kind::Function, fnPtr.getPointer(subIGF), newAuthInfo,
        Signature::forAsyncAwait(subIGF.IGM, origType,
                                 FunctionPointerKind::defaultAsync()));
    auto &Builder = subIGF.Builder;

    auto argValues = args.claimAll();

    // Setup the suspend point.
    SmallVector<llvm::Value *, 8> arguments;
    auto signature = newFnPtr.getSignature();
    auto asyncContextIndex = signature.getAsyncContextIndex();
    auto paramAttributeFlags =
        asyncContextIndex |
        (signature.getAsyncResumeFunctionSwiftSelfIndex() << 8);
    // Index of swiftasync context | ((index of swiftself) << 8).
    arguments.push_back(
        IGM.getInt32(paramAttributeFlags));
    arguments.push_back(currentResumeFn);
    auto resumeProjFn = subIGF.getOrCreateResumePrjFn();
    arguments.push_back(
        Builder.CreateBitOrPointerCast(resumeProjFn, IGM.Int8PtrTy));
    auto dispatchFn = subIGF.createAsyncDispatchFn(
        getFunctionPointerForDispatchCall(IGM, newFnPtr), argValues);
    arguments.push_back(
        Builder.CreateBitOrPointerCast(dispatchFn, IGM.Int8PtrTy));
    arguments.push_back(
        Builder.CreateBitOrPointerCast(newFnPtr.getRawPointer(), IGM.Int8PtrTy));
    if (auto authInfo = newFnPtr.getAuthInfo()) {
      arguments.push_back(newFnPtr.getAuthInfo().getDiscriminator());
    }
    for (auto arg : argValues)
      arguments.push_back(arg);
    auto resultTy =
        cast<llvm::StructType>(signature.getType()->getReturnType());
    return subIGF.emitSuspendAsyncCall(asyncContextIndex, resultTy, arguments);
  }
  void createReturn(llvm::CallInst *call) override {
    emitDeallocAsyncContext(subIGF, calleeContextBuffer);
    forwardAsyncCallResult(subIGF, origType, layout, call);
  }
  void end() override {
    assert(context.isValid());
    super::end();
  }
};
std::unique_ptr<PartialApplicationForwarderEmission>
getPartialApplicationForwarderEmission(
    IRGenModule &IGM, IRGenFunction &subIGF, llvm::Function *fwd,
    const llvm::Optional<FunctionPointer> &staticFnPtr, bool calleeHasContext,
    const Signature &origSig, CanSILFunctionType origType,
    CanSILFunctionType substType, CanSILFunctionType outType,
    SubstitutionMap subs, HeapLayout const *layout,
    ArrayRef<ParameterConvention> conventions) {
  if (origType->isAsync()) {
    return std::make_unique<AsyncPartialApplicationForwarderEmission>(
        IGM, subIGF, fwd, staticFnPtr, calleeHasContext, origSig, origType,
        substType, outType, subs, layout, conventions);
  } else {
    return std::make_unique<SyncPartialApplicationForwarderEmission>(
        IGM, subIGF, fwd, staticFnPtr, calleeHasContext, origSig, origType,
        substType, outType, subs, layout, conventions);
  }
}

} // end anonymous namespace

/// Emit the forwarding stub function for a partial application.
///
/// If 'layout' is null, there is a single captured value of
/// Swift-refcountable type that is being used directly as the
/// context object.
static llvm::Value *emitPartialApplicationForwarder(
    IRGenModule &IGM, const llvm::Optional<FunctionPointer> &staticFnPtr,
    bool calleeHasContext, const Signature &origSig,
    CanSILFunctionType origType, CanSILFunctionType substType,
    CanSILFunctionType outType, SubstitutionMap subs, HeapLayout const *layout,
    ArrayRef<ParameterConvention> conventions) {
  auto outSig = IGM.getSignature(outType);
  llvm::AttributeList outAttrs = outSig.getAttributes();
  llvm::FunctionType *fwdTy = outSig.getType();
  SILFunctionConventions outConv(outType, IGM.getSILModule());
  llvm::Optional<AsyncContextLayout> asyncLayout;

  StringRef FnName;
  if (staticFnPtr)
    FnName = staticFnPtr->getName(IGM);

  IRGenMangler Mangler;
  std::string thunkName = Mangler.manglePartialApplyForwarder(FnName);

  // FIXME: Maybe cache the thunk by function and closure types?.
  llvm::Function *fwd =
      llvm::Function::Create(fwdTy, llvm::Function::InternalLinkage,
                             llvm::StringRef(thunkName), &IGM.Module);
  llvm::Value *asyncFunctionPtr = nullptr;
  fwd->setCallingConv(outSig.getCallingConv());

  fwd->setAttributes(outAttrs);
  // Merge initial attributes with outAttrs.
  llvm::AttrBuilder b(IGM.getLLVMContext());
  IGM.constructInitialFnAttributes(b);
  fwd->addFnAttrs(b);

  IRGenFunction subIGF(IGM, fwd);
  if (origType->isAsync()) {
    auto fpKind = FunctionPointerKind::defaultAsync();
    auto asyncContextIdx =
        Signature::forAsyncEntry(IGM, outType, fpKind)
            .getAsyncContextIndex();
    asyncLayout.emplace(irgen::getAsyncContextLayout(
        IGM, origType, substType, subs));

    //auto *calleeAFP = staticFnPtr->getDirectPointer();
    LinkEntity entity = LinkEntity::forPartialApplyForwarder(fwd);
    assert(!asyncFunctionPtr &&
           "already had an async function pointer to the forwarder?!");
    emitAsyncFunctionEntry(subIGF, *asyncLayout, entity, asyncContextIdx);
    asyncFunctionPtr =
        emitAsyncFunctionPointer(IGM, fwd, entity, asyncLayout->getSize());
    // TODO: if calleeAFP is definition:
#if 0
    subIGF.Builder.CreateIntrinsicCall(
        llvm::Intrinsic::coro_async_size_replace,
        {subIGF.Builder.CreateBitCast(asyncFunctionPtr, IGM.Int8PtrTy),
         subIGF.Builder.CreateBitCast(calleeAFP, IGM.Int8PtrTy)});
#endif
  }
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(subIGF, fwd);

  auto emission = getPartialApplicationForwarderEmission(
      IGM, subIGF, fwd, staticFnPtr, calleeHasContext, origSig, origType,
      substType, outType, subs, layout, conventions);
  emission->begin();
  emission->gatherArgumentsFromApply();

  struct AddressToDeallocate {
    SILType Type;
    const TypeInfo &TI;
    StackAddress Addr;
  };
  SmallVector<AddressToDeallocate, 4> addressesToDeallocate;

  bool dependsOnContextLifetime = false;
  bool consumesContext;
  bool needsAllocas = false;
  
  switch (outType->getCalleeConvention()) {
  case ParameterConvention::Direct_Owned:
    consumesContext = true;
    break;
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    consumesContext = false;
    break;
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    llvm_unreachable("indirect or pack callables not supported");
  }

  // Lower the captured arguments in the original function's generic context.
  GenericContextScope scope(IGM, origType->getInvocationGenericSignature());

  // This is where the context parameter appears.
  llvm::Value *rawData = nullptr;
  Address data;
  unsigned nextCapturedField = 0;
  if (!layout) {
    rawData = emission->getContext();
  } else if (!layout->isKnownEmpty()) {
    rawData = emission->getContext();
    data = layout->emitCastTo(subIGF, rawData);

    // Restore type metadata bindings, if we have them.
    if (layout->hasBindings()) {
      auto bindingLayout = layout->getElement(nextCapturedField++);
      // The bindings should be fixed-layout inside the object, so we can
      // pass None here. If they weren't, we'd have a chicken-egg problem.
      auto bindingsAddr =
          bindingLayout.project(subIGF, data, /*offsets*/ llvm::None);
      layout->getBindings().restore(subIGF, bindingsAddr,
                                    MetadataState::Complete);
    }

  // There's still a placeholder to claim if the target type is thick
  // or there's an error result.
  } else if (outType->getRepresentation()==SILFunctionTypeRepresentation::Thick
             || outType->hasErrorResult()) {
    llvm::Value *contextPtr = emission->getContext(); (void)contextPtr;
    assert(contextPtr->getType() == IGM.RefCountedPtrTy);
  }

  Explosion polyArgs;

  // Emit the polymorphic arguments.
  assert((subs.hasAnySubstitutableParams()
            == hasPolymorphicParameters(origType) ||
         (!subs.hasAnySubstitutableParams() && origType->getRepresentation() ==
             SILFunctionTypeRepresentation::WitnessMethod))
         && "should have substitutions iff original function is generic");
  WitnessMetadata witnessMetadata;

  // If we have a layout we might have to bind polymorphic arguments from the
  // captured arguments which we will do later. Otherwise, we have to
  // potentially bind polymorphic arguments from the context if it was a
  // partially applied argument.
  bool hasPolymorphicParams =
      hasPolymorphicParameters(origType) &&
      (!staticFnPtr || !staticFnPtr->shouldSuppressPolymorphicArguments());
  if (!layout && hasPolymorphicParams) {
    assert(conventions.size() == 1);
    // We could have either partially applied an argument from the function
    // signature or otherwise we could have a closure context to forward. We only
    // care for the former for the purpose of reconstructing polymorphic
    // parameters from regular arguments.
    if (!calleeHasContext) {
      unsigned paramI =
          findSinglePartiallyAppliedParameterIndexIgnoringEmptyTypes(
              subIGF, substType, outType);
      auto paramInfo = substType->getParameters()[paramI];
      auto &ti = IGM.getTypeInfoForLowered(paramInfo.getArgumentType(
          IGM.getSILModule(), substType, IGM.getMaximalTypeExpansionContext()));
      Explosion param;
      auto ref = rawData;
      // We can get a '{ swift.refcounted* }' type for AnyObject on linux.
      if (!ti.getStorageType()->isPointerTy() &&
          ti.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal))
        ref = subIGF.coerceValue(rawData, ti.getStorageType(),
                                 subIGF.IGM.DataLayout);
      else
        ref = subIGF.Builder.CreateBitCast(rawData, ti.getStorageType());
      param.add(ref);
      bindPolymorphicParameter(subIGF, origType, substType, param, paramI);
      (void)param.claimAll();
    }

    emitPolymorphicArguments(subIGF, origType, subs,
                             &witnessMetadata, polyArgs);
  }

  auto haveContextArgument =
      calleeHasContext || hasSelfContextParameter(origType);

  // Witness method calls expect self, followed by the self type followed by,
  // the witness table at the end of the parameter list. But polymorphic
  // arguments come before this.
  bool isWitnessMethodCallee = origType->getRepresentation() ==
      SILFunctionTypeRepresentation::WitnessMethod;
  bool isMethodCallee =
      origType->getRepresentation() == SILFunctionTypeRepresentation::Method;
  Explosion witnessMethodSelfValue;

  llvm::Value *lastCapturedFieldPtr = nullptr;

  // If there's a data pointer required, but it's a swift-retainable
  // value being passed as the context, just forward it down.
  if (!layout) {
    assert(conventions.size() == 1);

    // We need to retain the parameter if:
    //   - we received at +0 (either) and are passing as owned
    //   - we received as unowned and are passing as guaranteed
    auto argConvention = conventions[nextCapturedField++];
    switch (argConvention) {
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Direct_Owned:
      if (!consumesContext) subIGF.emitNativeStrongRetain(rawData, subIGF.getDefaultAtomicity());
      break;

    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Direct_Guaranteed:
      dependsOnContextLifetime = true;
      if (outType->getCalleeConvention() ==
            ParameterConvention::Direct_Unowned) {
        subIGF.emitNativeStrongRetain(rawData, subIGF.getDefaultAtomicity());
        consumesContext = true;
      }
      break;

    case ParameterConvention::Direct_Unowned:
      // Make sure we release later if we received at +1.
      if (consumesContext)
        dependsOnContextLifetime = true;
      break;

    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Inout:
      llvm_unreachable("should never happen!");
    }

    // FIXME: The naming and documentation here isn't ideal. This
    // parameter is always present which is evident since we always
    // grab a type to cast to, but sometimes after the polymorphic
    // arguments. This is just following the lead of existing (and not
    // terribly easy to follow) code.

    // If there is a context argument, it comes after the polymorphic
    // arguments.
    auto argIndex = emission->getCurrentArgumentIndex();
    if (haveContextArgument)
      argIndex += polyArgs.size();
    if (origType->isAsync())
      argIndex += 1;

    llvm::Type *expectedArgTy = origSig.getType()->getParamType(argIndex);

    llvm::Value *argValue;
    if (isIndirectFormalParameter(argConvention)) {
      // We can use rawData's type for the alloca because it is a swift
      // retainable value. Defensively, give it that type. We can't use the
      // expectedArgType because it might be a generic parameter and therefore
      // have opaque storage.
      auto RetainableValue = rawData;
      if (RetainableValue->getType() != subIGF.IGM.RefCountedPtrTy)
        RetainableValue = subIGF.Builder.CreateBitCast(
            RetainableValue, subIGF.IGM.RefCountedPtrTy);
      needsAllocas = true;
      auto temporary = subIGF.createAlloca(RetainableValue->getType(),
                                           subIGF.IGM.getPointerAlignment(),
                                           "partial-apply.context");
      subIGF.Builder.CreateStore(RetainableValue, temporary);
      argValue = temporary.getAddress();
      argValue = subIGF.Builder.CreateBitCast(argValue, expectedArgTy);
    } else {
      argValue = subIGF.Builder.CreateBitCast(rawData, expectedArgTy);
    }
    emission->addArgument(argValue);

  // If there's a data pointer required, grab it and load out the
  // extra, previously-curried parameters.
  } else {
    unsigned origParamI = outType->getParameters().size();
    unsigned extraFieldIndex = 0;
    assert(layout->getElements().size() == conventions.size()
           && "conventions don't match context layout");

    // Calculate non-fixed field offsets.
    HeapNonFixedOffsets offsets(subIGF, *layout);

    // Perform the loads.
    for (unsigned n = layout->getElements().size();
         nextCapturedField < n;
         ++nextCapturedField) {
      auto &fieldLayout = layout->getElement(nextCapturedField);
      auto &fieldTy = layout->getElementTypes()[nextCapturedField];
      auto fieldConvention = conventions[nextCapturedField];
      Address fieldAddr = fieldLayout.project(subIGF, data, offsets);
      auto &fieldTI = fieldLayout.getType();
      lastCapturedFieldPtr = fieldAddr.getAddress();
      
      Explosion param;
      switch (fieldConvention) {
      case ParameterConvention::Indirect_In: {

        auto initStackCopy = [&addressesToDeallocate, &needsAllocas, &param,
                              &subIGF](const TypeInfo &fieldTI, SILType fieldTy,
                                       Address fieldAddr) {
          // The +1 argument is passed indirectly, so we need to copy into a
          // temporary.
          needsAllocas = true;
          auto stackAddr = fieldTI.allocateStack(subIGF, fieldTy, "arg.temp");
          auto addressPointer = stackAddr.getAddress().getAddress();
          fieldTI.initializeWithCopy(subIGF, stackAddr.getAddress(), fieldAddr,
                                     fieldTy, false);
          param.add(addressPointer);

          // Remember to deallocate later.
          addressesToDeallocate.push_back(
              AddressToDeallocate{fieldTy, fieldTI, stackAddr});
        };

        if (outType->isNoEscape()) {
          // If the closure is [onstack] it only captured the address of the
          // value. Load that address from the context.
          Explosion addressExplosion;
          cast<LoadableTypeInfo>(fieldTI).loadAsCopy(subIGF, fieldAddr,
                                                     addressExplosion);
          assert(fieldTy.isAddress());
          auto newFieldTy = fieldTy.getObjectType();
          auto &newFieldTI =
              subIGF.getTypeInfoForLowered(newFieldTy.getASTType());
          fieldAddr =
              newFieldTI.getAddressForPointer(addressExplosion.claimNext());
          initStackCopy(newFieldTI, newFieldTy, fieldAddr);
        } else {
          initStackCopy(fieldTI, fieldTy, fieldAddr);
        }
        break;
      }
      case ParameterConvention::Indirect_In_Guaranteed:
        if (outType->isNoEscape()) {
          cast<LoadableTypeInfo>(fieldTI).loadAsCopy(subIGF, fieldAddr, param);
        } else {
          // The argument is +0, so we can use the address of the param in
          // the context directly.
          param.add(fieldAddr.getAddress());
          dependsOnContextLifetime = true;
        }
        break;
      case ParameterConvention::Pack_Guaranteed:
      case ParameterConvention::Pack_Owned:
      case ParameterConvention::Pack_Inout:
        llvm_unreachable("partial application of pack?");
        break;
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable:
        // Load the address of the inout parameter.
        cast<LoadableTypeInfo>(fieldTI).loadAsCopy(subIGF, fieldAddr, param);
        break;
      case ParameterConvention::Direct_Guaranteed:
      case ParameterConvention::Direct_Unowned:
        // If the type is nontrivial, keep the context alive since the field
        // depends on the context to not be deallocated.
        if (!fieldTI.isTriviallyDestroyable(ResilienceExpansion::Maximal))
          dependsOnContextLifetime = true;

        // Load these parameters directly. We can "take" since the parameter is
        // +0. This can happen since the context will keep the parameter alive.
        cast<LoadableTypeInfo>(fieldTI).loadAsTake(subIGF, fieldAddr, param);
        break;
      case ParameterConvention::Direct_Owned:
        // Copy the value out at +1.
        cast<LoadableTypeInfo>(fieldTI).loadAsCopy(subIGF, fieldAddr, param);
        break;
      }
      
      // Reemit the capture params as unsubstituted.

      // Skip empty parameters.
      while (origParamI < origType->getParameters().size()) {
        if (!isABIIgnoredParameterWithoutStorage(IGM, subIGF, substType,
                                                 origParamI))
          break;
        ++origParamI;
      }

      if (origParamI < origType->getParameters().size()) {
        Explosion origParam;
        auto origParamInfo = origType->getParameters()[origParamI];
        if (hasPolymorphicParams)
          bindPolymorphicParameter(subIGF, origType, substType, param,
                                   origParamI);
        emitApplyArgument(subIGF, origType, origParamInfo, substType,
                          emission->getParameterInfo(origParamI), param,
                          origParam);
        bool isWitnessMethodCalleeSelf = (isWitnessMethodCallee &&
            origParamI + 1 == origType->getParameters().size());
        Explosion arg;
        needsAllocas |= emission->transformArgumentToNative(
            origParamInfo, origParam,
            isWitnessMethodCalleeSelf ? witnessMethodSelfValue : arg);
        if (!isWitnessMethodCalleeSelf) {
          emission->addArgument(arg, origParamI);
        }
        ++origParamI;
      } else {
        switch (extraFieldIndex) {
        case 0:
          emission->addDynamicFunctionContext(param);
          break;
        case 1:
          emission->addDynamicFunctionPointer(param);
          break;
        default:
          llvm_unreachable("unexpected extra field in thick context");
        }
        ++extraFieldIndex;
      }
      
    }
    
    // If the parameters can live independent of the context, release it now
    // so we can tail call. The safety of this assumes that neither this release
    // nor any of the loads can throw.
    if (consumesContext && !dependsOnContextLifetime && rawData) {
      assert(!outType->isNoEscape() && "Trivial context must not be released");
      subIGF.emitNativeStrongRelease(rawData, subIGF.getDefaultAtomicity());
    }

    // Now that we have bound generic parameters from the captured arguments
    // emit the polymorphic arguments.
    if (hasPolymorphicParameters(origType)) {
      emitPolymorphicArguments(subIGF, origType, subs,
                               &witnessMetadata, polyArgs);
    }
  }

  // Derive the callee function pointer.
  auto fnTy = origSig.getType()->getPointerTo();
  FunctionPointer fnPtr = [&]() -> FunctionPointer {
    // If we found a function pointer statically, great.
    if (staticFnPtr) {
      if (staticFnPtr->getPointer(subIGF)->getType() != fnTy) {
        auto fnPtr = staticFnPtr->getPointer(subIGF);
        fnPtr = subIGF.Builder.CreateBitCast(fnPtr, fnTy);
        return FunctionPointer::createUnsigned(origType, fnPtr, origSig);
      }
      return *staticFnPtr;
    }

    // Otherwise, it was the last thing we added to the layout.

    assert(lastCapturedFieldPtr);
    auto authInfo = PointerAuthInfo::emit(
        subIGF,
        origType->isAsync()
            ? IGM.getOptions().PointerAuth.AsyncPartialApplyCapture
            : IGM.getOptions().PointerAuth.PartialApplyCapture,
        lastCapturedFieldPtr, PointerAuthEntity::Special::PartialApplyCapture);

    // The dynamic function pointer is packed "last" into the context,
    // and we pulled it out as an argument.  Just pop it off.
    auto fnPtr = emission->getDynamicFunctionPointer();

    // It comes out of the context as an i8*. Cast to the function type.
    fnPtr = subIGF.Builder.CreateBitCast(fnPtr, fnTy);

    return FunctionPointer::createSigned(
        origType->isAsync() ? FunctionPointer::Kind::AsyncFunctionPointer
                            : FunctionPointer::Kind::Function,
        fnPtr, authInfo, origSig);
  }();

  if (origType->isAsync())
    emission->mapAsyncParameters(fnPtr);

  // Derive the context argument if needed.  This is either:
  //   - the saved context argument, in which case it was the last
  //     thing we added to the layout other than a possible non-static
  //     function pointer (which we already popped off of 'args'); or
  //   - 'self', in which case it was the last formal argument.
  // In either case, it's the last thing in 'args'.
  llvm::Value *fnContext = nullptr;
  if (haveContextArgument)
    fnContext = emission->getDynamicFunctionContext();

  emission->addPolymorphicArguments(std::move(polyArgs));

  // If we have a witness method call, the inner context is the
  // witness table. Metadata for Self is derived inside the partial
  // application thunk and doesn't need to be stored in the outer
  // context.
  if (isWitnessMethodCallee) {
    assert(fnContext->getType() == IGM.Int8PtrTy);
    llvm::Value *wtable = subIGF.Builder.CreateBitCast(
        fnContext, IGM.WitnessTablePtrTy);
    assert(wtable->getType() == IGM.WitnessTablePtrTy);
    witnessMetadata.SelfWitnessTable = wtable;

  // Okay, this is where the callee context goes.
  } else if (fnContext) {
    Explosion explosion;
    explosion.add(fnContext);
    if (isMethodCallee) {
      emission->addSelf(explosion);
    } else {
      emission->addDynamicFunctionContext(explosion);
    }

  // Pass a placeholder for thin function calls.
  } else if (origType->hasErrorResult() && !origType->isAsync()) {
    emission->addArgument(llvm::UndefValue::get(IGM.RefCountedPtrTy));
  }

  // Add the witness methods self argument before the error parameter after the
  // polymorphic arguments.
  if (isWitnessMethodCallee)
    emission->addSelf(witnessMethodSelfValue);

  // Pass down the error result.
  if (origType->hasErrorResult()) {
    emission->forwardErrorResult();
  }

  assert(emission->originalParametersConsumed());

  if (isWitnessMethodCallee) {
    assert(witnessMetadata.SelfMetadata->getType() == IGM.TypeMetadataPtrTy);
    emission->addWitnessSelfMetadata(witnessMetadata.SelfMetadata);
    assert(witnessMetadata.SelfWitnessTable->getType() == IGM.WitnessTablePtrTy);
    emission->addWitnessSelfWitnessTable(witnessMetadata.SelfWitnessTable);
  }

  llvm::CallInst *call = emission->createCall(fnPtr);

  if (!origType->isAsync() && addressesToDeallocate.empty() && !needsAllocas &&
      (!consumesContext || !dependsOnContextLifetime))
    call->setTailCall();

  // Deallocate everything we allocated above.
  // FIXME: exceptions?
  for (auto &entry : addressesToDeallocate) {
    entry.TI.deallocateStack(subIGF, entry.Addr, entry.Type);
  }
  
  // If the parameters depended on the context, consume the context now.
  if (rawData && consumesContext && dependsOnContextLifetime) {
    assert(!outType->isNoEscape() && "Trivial context must not be released");
    subIGF.emitNativeStrongRelease(rawData, subIGF.getDefaultAtomicity());
  }

  emission->createReturn(call);
  emission->end();

  return asyncFunctionPtr ? asyncFunctionPtr : fwd;
}

/// Emit a partial application thunk for a function pointer applied to a partial
/// set of argument values.
llvm::Optional<StackAddress> irgen::emitFunctionPartialApplication(
    IRGenFunction &IGF, SILFunction &SILFn, const FunctionPointer &fn,
    llvm::Value *fnContext, Explosion &args, ArrayRef<SILParameterInfo> params,
    SubstitutionMap subs, CanSILFunctionType origType,
    CanSILFunctionType substType, CanSILFunctionType outType, Explosion &out,
    bool isOutlined) {
  // If we have a single Swift-refcounted context value, we can adopt it
  // directly as our closure context without creating a box and thunk.
  enum HasSingleSwiftRefcountedContext { Maybe, Yes, No, Thunkable }
    hasSingleSwiftRefcountedContext = Maybe;
  llvm::Optional<ParameterConvention> singleRefcountedConvention;
  llvm::Optional<llvm::Type *> singleRefCountedType;

  SmallVector<const TypeInfo *, 4> argTypeInfos;
  SmallVector<SILType, 4> argValTypes;
  SmallVector<ParameterConvention, 4> argConventions;

  // A context's HeapLayout stores all of the partially applied args.
  // A HeapLayout is "fixed" if all of its fields have a fixed layout.
  // Otherwise the HeapLayout is "non-fixed".
  // Only a non-fixed HeapLayout needs TypeMetadata of the non-fixed fields
  // during IRGen of the HeapLayout's destructor function.
  // We should not consider partially applied args as TypeMetadata sources,
  // because they are available only in the caller and the partial application
  // forwarder, but not in the destructor function.
  // It is safe to consider partially applied args as TypeMetadata sources for
  // "fixed" HeapLayout, because they are not accessed during the IRGen of the
  // destructor function.
  bool considerParameterSources = true;
  for (auto param : params) {
    SILType argType = IGF.IGM.silConv.getSILType(
        param, origType, IGF.IGM.getMaximalTypeExpansionContext());
    auto argLoweringTy = getArgumentLoweringType(argType.getASTType(), param,
                                                 outType->isNoEscape());
    auto &ti = IGF.getTypeInfoForLowered(argLoweringTy);

    if (!isa<FixedTypeInfo>(ti)) {
      considerParameterSources = false;
      break;
    }
  }

  // Reserve space for polymorphic bindings.
  auto bindings = NecessaryBindings::forPartialApplyForwarder(
      IGF.IGM, origType, subs, outType->isNoEscape(),
      considerParameterSources);

  if (!bindings.empty()) {
    hasSingleSwiftRefcountedContext = No;
    auto bindingsSize = bindings.getBufferSize(IGF.IGM);
    auto &bindingsTI = IGF.IGM.getOpaqueStorageTypeInfo(bindingsSize,
                                                 IGF.IGM.getPointerAlignment());
    argValTypes.push_back(SILType());
    argTypeInfos.push_back(&bindingsTI);
    argConventions.push_back(ParameterConvention::Direct_Unowned);
  }

  // Collect the type infos for the context parameters.
  for (auto param : params) {
    SILType argType = IGF.IGM.silConv.getSILType(
        param, origType, IGF.IGM.getMaximalTypeExpansionContext());

    auto argLoweringTy = getArgumentLoweringType(argType.getASTType(), param,
                                                 outType->isNoEscape());

    auto &ti = IGF.getTypeInfoForLowered(argLoweringTy);

    // Empty values don't matter.
    auto schema = ti.getSchema();
    if (schema.empty() && !param.isFormalIndirect())
      continue;

    argValTypes.push_back(argType);
    argConventions.push_back(param.getConvention());
    argTypeInfos.push_back(&ti);

    // Update the single-swift-refcounted check, unless we already ruled that
    // out.
    if (hasSingleSwiftRefcountedContext == No)
      continue;
    
    
    // Adding nonempty values when we already have a single refcounted pointer
    // means we don't have a single value anymore.
    if (hasSingleSwiftRefcountedContext != Maybe) {
      hasSingleSwiftRefcountedContext = No;
      continue;
    }
      
    if (ti.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
      hasSingleSwiftRefcountedContext = Yes;
      singleRefcountedConvention = param.getConvention();
      singleRefCountedType = ti.getStorageType();
    } else {
      hasSingleSwiftRefcountedContext = No;
    }
  }

  // We can't just bitcast if there's an error parameter to forward.
  // This is an unfortunate restriction arising from the fact that a
  // thin throwing function will have the signature:
  //   %result (%arg*, %context*, %error*)
  // but the output signature needs to be
  //   %result (%context*, %error*)
  //
  // 'swifterror' fixes this physically, but there's still a risk of
  // miscompiles because the LLVM optimizer may forward arguments
  // positionally without considering 'swifterror'.
  //
  // Note, however, that we will override this decision below if the
  // only thing we have to forward is already a context pointer.
  // That's fine.
  //
  // The proper long-term fix is that closure functions should be
  // emitted with a convention that takes the closure box as the
  // context parameter.  When we do that, all of this code will
  // disappear.
  if (hasSingleSwiftRefcountedContext == Yes &&
      origType->hasErrorResult()) {
    hasSingleSwiftRefcountedContext = Thunkable;
  }
  
  // If the function pointer is a witness method call, include the witness
  // table in the context.
  if (origType->getRepresentation() ==
        SILFunctionTypeRepresentation::WitnessMethod) {
    llvm::Value *wtable = fnContext;
    assert(wtable->getType() == IGF.IGM.WitnessTablePtrTy);

    // TheRawPointerType lowers as i8*, not i8**.
    args.add(IGF.Builder.CreateBitCast(wtable, IGF.IGM.Int8PtrTy));

    argValTypes.push_back(SILType::getRawPointerType(IGF.IGM.Context));
    argTypeInfos.push_back(
         &IGF.getTypeInfoForLowered(IGF.IGM.Context.TheRawPointerType));
    argConventions.push_back(ParameterConvention::Direct_Unowned);
    hasSingleSwiftRefcountedContext = No;

  // Otherwise, we might have a reference-counted context pointer.
  } else if (fnContext) {
    args.add(fnContext);
    argValTypes.push_back(SILType::getNativeObjectType(IGF.IGM.Context));
    argConventions.push_back(origType->getCalleeConvention());
    argTypeInfos.push_back(
         &IGF.getTypeInfoForLowered(IGF.IGM.Context.TheNativeObjectType));
    // If this is the only context argument we end up with, we can just share
    // it.
    if (args.size() == 1) {
      assert(bindings.empty());
      hasSingleSwiftRefcountedContext = Yes;
      singleRefcountedConvention = origType->getCalleeConvention();
      singleRefCountedType = IGF.IGM.getNativeObjectTypeInfo().getStorageType();
    }
  }

  auto outAuthInfo = PointerAuthInfo::forFunctionPointer(IGF.IGM, outType);
  
  // If we have a single refcounted pointer context (and no polymorphic args
  // to capture), and the dest ownership semantics match the parameter's,
  // skip building the box and thunk and just take the pointer as
  // context.
  // TODO: We can only do this and use swiftself if all our swiftcc emit the
  // last parameter that fits into a register as swiftself.
  // We should get this optimization back using the @convention(closure) whose
  // box argument should just be swift self.
  if (/* DISABLES CODE */ (false) &&
      !origType->isPolymorphic() &&
      hasSingleSwiftRefcountedContext == Yes &&
      outType->getCalleeConvention() == *singleRefcountedConvention) {
    assert(args.size() == 1);
    auto fnPtr = emitPointerAuthResign(IGF, fn, outAuthInfo).getPointer(IGF);
    fnPtr = IGF.Builder.CreateBitCast(fnPtr, IGF.IGM.Int8PtrTy);
    out.add(fnPtr);
    llvm::Value *ctx = args.claimNext();
    ctx = IGF.Builder.CreateBitCast(ctx, IGF.IGM.RefCountedPtrTy);
    out.add(ctx);
    return {};
  }

  llvm::Optional<FunctionPointer> staticFn;
  if (fn.isConstant()) staticFn = fn;

  // If the function pointer is dynamic, include it in the context.
  size_t nonStaticFnIndex = ~size_t(0);
  if (!staticFn) {
    nonStaticFnIndex = argTypeInfos.size();
    argValTypes.push_back(SILType::getRawPointerType(IGF.IGM.Context));
    argTypeInfos.push_back(
         &IGF.getTypeInfoForLowered(IGF.IGM.Context.TheRawPointerType));
    argConventions.push_back(ParameterConvention::Direct_Unowned);
    hasSingleSwiftRefcountedContext = No;
  }

  // If we only need to capture a single Swift-refcounted object, we
  // still need to build a thunk, but we don't need to allocate anything.
  if ((hasSingleSwiftRefcountedContext == Yes ||
       hasSingleSwiftRefcountedContext == Thunkable) &&
      *singleRefcountedConvention != ParameterConvention::Indirect_Inout &&
      *singleRefcountedConvention !=
        ParameterConvention::Indirect_InoutAliasable) {
    assert(bindings.empty());
    assert(args.size() == 1);

    auto origSig = IGF.IGM.getSignature(origType);

    llvm::Value *forwarder =
      emitPartialApplicationForwarder(IGF.IGM, staticFn, fnContext != nullptr,
                                      origSig, origType, substType,
                                      outType, subs, nullptr, argConventions);
    forwarder = emitPointerAuthSign(IGF, forwarder, outAuthInfo);
    forwarder = IGF.Builder.CreateBitCast(forwarder, IGF.IGM.Int8PtrTy);
    out.add(forwarder);

    llvm::Value *ctx = args.claimNext();
    if (isIndirectFormalParameter(*singleRefcountedConvention))
      ctx = IGF.Builder.CreateLoad(
          Address(ctx, *singleRefCountedType, IGF.IGM.getPointerAlignment()));

    auto expectedClosureTy =
        outType->isNoEscape() ? IGF.IGM.OpaquePtrTy : IGF.IGM.RefCountedPtrTy;

    // We might get a struct containing a pointer e.g type <{ %AClass* }>
    if (ctx->getType() != expectedClosureTy)
      ctx = IGF.coerceValue(ctx, expectedClosureTy, IGF.IGM.DataLayout);
    out.add(ctx);
    if (outType->isNoEscape())
      return StackAddress();
    return {};
  }

  // Store the context arguments on the heap/stack.
  assert(argValTypes.size() == argTypeInfos.size()
         && argTypeInfos.size() == argConventions.size()
         && "argument info lists out of sync");
  HeapLayout layout(IGF.IGM, LayoutStrategy::Optimal, argValTypes, argTypeInfos,
                    /*typeToFill*/ nullptr, std::move(bindings),
                    /*bindingsIndex*/ 0);

  llvm::Value *data;

  llvm::Optional<StackAddress> stackAddr;

  if (args.empty() && layout.isKnownEmpty()) {
    if (outType->isNoEscape())
      data = llvm::ConstantPointerNull::get(IGF.IGM.OpaquePtrTy);
    else
      data = IGF.IGM.RefCountedNull;
  } else {

    // Allocate a new object on the heap or stack.
    HeapNonFixedOffsets offsets(IGF, layout);
    if (outType->isNoEscape()) {
      stackAddr = IGF.emitDynamicAlloca(
          IGF.IGM.Int8Ty,
          layout.isFixedLayout() ? layout.emitSize(IGF.IGM) : offsets.getSize(),
          Alignment(16));
      stackAddr = stackAddr->withAddress(IGF.Builder.CreateElementBitCast(
          stackAddr->getAddress(), IGF.IGM.OpaqueTy));
      data = stackAddr->getAddress().getAddress();
    } else {
        auto descriptor = IGF.IGM.getAddrOfCaptureDescriptor(SILFn, origType,
                                                       substType, subs,
                                                       layout);

        data = IGF.emitUnmanagedAlloc(layout, "closure", descriptor, &offsets);
    }
    Address dataAddr = layout.emitCastTo(IGF, data);
    
    unsigned i = 0;

    // Store necessary bindings, if we have them.
    if (layout.hasBindings()) {
      auto &bindingsLayout = layout.getElement(i);
      Address bindingsAddr = bindingsLayout.project(IGF, dataAddr, offsets);
      layout.getBindings().save(IGF, bindingsAddr);
      ++i;
    }
    
    // Store the context arguments.
    for (unsigned end = layout.getElements().size(); i < end; ++i) {
      auto &fieldLayout = layout.getElement(i);
      auto &fieldTy = layout.getElementTypes()[i];
      Address fieldAddr = fieldLayout.project(IGF, dataAddr, offsets);

      // We don't add non-constant function pointers to the explosion above,
      // so we need to handle them specially now.
      if (i == nonStaticFnIndex) {
        llvm::Value *fnPtr = fn.getRawPointer();
        if (auto &schema =
                origType->isAsync()
                    ? IGF.getOptions().PointerAuth.AsyncPartialApplyCapture
                    : IGF.getOptions().PointerAuth.PartialApplyCapture) {
          auto schemaAuthInfo = PointerAuthInfo::emit(
              IGF, schema, fieldAddr.getAddress(),
              PointerAuthEntity::Special::PartialApplyCapture);
          fnPtr =
              emitPointerAuthResign(IGF, fn, schemaAuthInfo).getRawPointer();
        }

        fnPtr = IGF.Builder.CreateBitCast(fnPtr, IGF.IGM.Int8PtrTy);
        IGF.Builder.CreateStore(fnPtr, fieldAddr);
        continue;
      }

      switch (argConventions[i]) {
      // Take indirect value arguments out of memory.
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Guaranteed: {
        if (outType->isNoEscape()) {
          cast<LoadableTypeInfo>(fieldLayout.getType())
              .initialize(IGF, args, fieldAddr, isOutlined);
        } else {
          auto addr =
              fieldLayout.getType().getAddressForPointer(args.claimNext());
          fieldLayout.getType().initializeWithTake(IGF, fieldAddr, addr,
                                                   fieldTy, isOutlined);
        }
        break;
      }
      // Take direct value arguments and inout pointers by value.
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Guaranteed:
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable:
        cast<LoadableTypeInfo>(fieldLayout.getType())
            .initialize(IGF, args, fieldAddr, isOutlined);
        break;

      case ParameterConvention::Pack_Guaranteed:
      case ParameterConvention::Pack_Owned:
      case ParameterConvention::Pack_Inout:
        llvm_unreachable("partial application of pack?");
        break;
      }
    }
  }
  assert(args.empty() && "unused args in partial application?!");
  
  // Create the forwarding stub.
  auto origSig = IGF.IGM.getSignature(origType);

  llvm::Value *forwarder = emitPartialApplicationForwarder(
      IGF.IGM, staticFn, fnContext != nullptr, origSig, origType, substType,
      outType, subs, &layout, argConventions);
  forwarder = emitPointerAuthSign(IGF, forwarder, outAuthInfo);
  forwarder = IGF.Builder.CreateBitCast(forwarder, IGF.IGM.Int8PtrTy);
  out.add(forwarder);
  out.add(data);
  return stackAddr;
}

/// Emit the block copy helper for a block.
static llvm::Function *emitBlockCopyHelper(IRGenModule &IGM,
                                           CanSILBlockStorageType blockTy,
                                           const BlockStorageTypeInfo &blockTL){
  // See if we've produced a block copy helper for this type before.
  // TODO
  
  // Create the helper.
  llvm::Type *args[] = {
    blockTL.getStorageType()->getPointerTo(),
    blockTL.getStorageType()->getPointerTo(),
  };
  auto copyTy = llvm::FunctionType::get(IGM.VoidTy, args, /*vararg*/ false);
  // TODO: Give these predictable mangled names and shared linkage.
  auto func = llvm::Function::Create(copyTy, llvm::GlobalValue::InternalLinkage,
                                     "block_copy_helper",
                                     IGM.getModule());
  func->setAttributes(IGM.constructInitialAttributes());
  IRGenFunction IGF(IGM, func);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, func);
  
  // Copy the captures from the source to the destination.
  Explosion params = IGF.collectParameters();
  auto dest = Address(params.claimNext(), blockTL.getStorageType(),
                      blockTL.getFixedAlignment());
  auto src = Address(params.claimNext(), blockTL.getStorageType(),
                     blockTL.getFixedAlignment());

  auto destCapture = blockTL.projectCapture(IGF, dest);
  auto srcCapture = blockTL.projectCapture(IGF, src);
  auto &captureTL = IGM.getTypeInfoForLowered(blockTy->getCaptureType());
  captureTL.initializeWithCopy(IGF, destCapture, srcCapture,
                               blockTy->getCaptureAddressType(), false);

  IGF.Builder.CreateRetVoid();
  
  return func;
}

/// Emit the block copy helper for a block.
static llvm::Function *emitBlockDisposeHelper(IRGenModule &IGM,
                                           CanSILBlockStorageType blockTy,
                                           const BlockStorageTypeInfo &blockTL){
  // See if we've produced a block destroy helper for this type before.
  // TODO
  
  // Create the helper.
  auto destroyTy = llvm::FunctionType::get(IGM.VoidTy,
                                       blockTL.getStorageType()->getPointerTo(),
                                       /*vararg*/ false);
  // TODO: Give these predictable mangled names and shared linkage.
  auto func = llvm::Function::Create(destroyTy,
                                     llvm::GlobalValue::InternalLinkage,
                                     "block_destroy_helper",
                                     IGM.getModule());
  func->setAttributes(IGM.constructInitialAttributes());
  IRGenFunction IGF(IGM, func);
  assert(!func->hasFnAttribute(llvm::Attribute::SanitizeThread));
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, func);
  
  // Destroy the captures.
  Explosion params = IGF.collectParameters();
  auto storage = Address(params.claimNext(), blockTL.getStorageType(),
                         blockTL.getFixedAlignment());
  auto capture = blockTL.projectCapture(IGF, storage);
  auto &captureTL = IGM.getTypeInfoForLowered(blockTy->getCaptureType());
  captureTL.destroy(IGF, capture, blockTy->getCaptureAddressType(),
                    false /*block storage code path: never outlined*/);
  IGF.Builder.CreateRetVoid();
  
  return func;
}

/// Emit the block header into a block storage slot.
void irgen::emitBlockHeader(IRGenFunction &IGF,
                            Address storage,
                            CanSILBlockStorageType blockTy,
                            llvm::Constant *invokeFunction,
                            CanSILFunctionType invokeTy,
                            ForeignFunctionInfo foreignInfo) {
  auto &storageTL
    = IGF.getTypeInfoForLowered(blockTy).as<BlockStorageTypeInfo>();

  Address headerAddr = storageTL.projectBlockHeader(IGF, storage);
  
  //
  // Initialize the "isa" pointer, which is _NSConcreteStackBlock.
  auto NSConcreteStackBlock =
      IGF.IGM.getModule()->getOrInsertGlobal("_NSConcreteStackBlock",
                                             IGF.IGM.ObjCClassStructTy);
  ApplyIRLinkage(IRLinkage::ExternalImport)
      .to(cast<llvm::GlobalVariable>(NSConcreteStackBlock));

  //
  // Set the flags.
  // - HAS_COPY_DISPOSE unless the capture type is POD
  uint32_t flags = 0;
  auto &captureTL
    = IGF.getTypeInfoForLowered(blockTy->getCaptureType());
  bool isTriviallyDestroyable = captureTL.isTriviallyDestroyable(ResilienceExpansion::Maximal);
  if (!isTriviallyDestroyable)
    flags |= 1 << 25;
  
  // - HAS_STRET, if the invoke function is sret
  assert(foreignInfo.ClangInfo);
  if (foreignInfo.ClangInfo->getReturnInfo().isIndirect())
    flags |= 1 << 29;
  
  // - HAS_SIGNATURE
  flags |= 1 << 30;
  
  auto flagsVal = llvm::ConstantInt::get(IGF.IGM.Int32Ty, flags);
  
  // Collect the reserved and invoke pointer fields.
  auto reserved = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);
  llvm::Value *invokeVal = llvm::ConstantExpr::getBitCast(invokeFunction,
                                                      IGF.IGM.FunctionPtrTy);
  
  // Build the block descriptor.
  ConstantInitBuilder builder(IGF.IGM);
  auto descriptorFields = builder.beginStruct();

  const clang::ASTContext &ASTContext = IGF.IGM.getClangASTContext();
  llvm::IntegerType *UnsignedLongTy =
      llvm::IntegerType::get(IGF.IGM.getLLVMContext(),
                             ASTContext.getTypeSize(ASTContext.UnsignedLongTy));
  descriptorFields.addInt(UnsignedLongTy, 0);
  descriptorFields.addInt(UnsignedLongTy,
                          storageTL.getFixedSize().getValue());
  
  if (!isTriviallyDestroyable) {
    // Define the copy and dispose helpers.
    descriptorFields.addSignedPointer(
                       emitBlockCopyHelper(IGF.IGM, blockTy, storageTL),
                       IGF.getOptions().PointerAuth.BlockHelperFunctionPointers,
                       PointerAuthEntity::Special::BlockCopyHelper);
    descriptorFields.addSignedPointer(
                       emitBlockDisposeHelper(IGF.IGM, blockTy, storageTL),
                       IGF.getOptions().PointerAuth.BlockHelperFunctionPointers,
                       PointerAuthEntity::Special::BlockDisposeHelper);
  }
  
  // Build the descriptor signature.
  descriptorFields.add(getBlockTypeExtendedEncoding(IGF.IGM, invokeTy));
  
  // Create the descriptor.
  auto descriptor =
    descriptorFields.finishAndCreateGlobal("block_descriptor",
                                           IGF.IGM.getPointerAlignment(),
                                           /*constant*/ true);

  auto descriptorVal = llvm::ConstantExpr::getBitCast(descriptor,
                                                      IGF.IGM.Int8PtrTy);
  
  // Store the block header.
  auto layout = IGF.IGM.DataLayout.getStructLayout(IGF.IGM.ObjCBlockStructTy);
  IGF.Builder.CreateStore(NSConcreteStackBlock,
                          IGF.Builder.CreateStructGEP(headerAddr, 0, layout));
  IGF.Builder.CreateStore(flagsVal,
                          IGF.Builder.CreateStructGEP(headerAddr, 1, layout));
  IGF.Builder.CreateStore(reserved,
                          IGF.Builder.CreateStructGEP(headerAddr, 2, layout));

  auto invokeAddr = IGF.Builder.CreateStructGEP(headerAddr, 3, layout);
  if (auto &schema =
        IGF.getOptions().PointerAuth.BlockInvocationFunctionPointers) {
    auto invokeAuthInfo = PointerAuthInfo::emit(IGF, schema,
                                                invokeAddr.getAddress(),
                                                invokeTy);
    invokeVal = emitPointerAuthSign(IGF, invokeVal, invokeAuthInfo);
  }
  IGF.Builder.CreateStore(invokeVal, invokeAddr);

  IGF.Builder.CreateStore(descriptorVal,
                          IGF.Builder.CreateStructGEP(headerAddr, 4, layout));
}

llvm::Value *
IRGenFunction::emitAsyncResumeProjectContext(llvm::Value *calleeContext) {
  auto addr = Builder.CreateBitOrPointerCast(calleeContext, IGM.Int8PtrPtrTy);
  Address callerContextAddr(addr, IGM.Int8PtrTy, IGM.getPointerAlignment());
  llvm::Value *callerContext = Builder.CreateLoad(callerContextAddr);
  if (auto schema = IGM.getOptions().PointerAuth.AsyncContextParent) {
    auto authInfo =
        PointerAuthInfo::emit(*this, schema, addr, PointerAuthEntity());
    callerContext = emitPointerAuthAuth(*this, callerContext, authInfo);
  }
  // TODO: remove this once all platforms support lowering the intrinsic.
  // At the time of this writing only arm64 supports it.
  if (IGM.TargetInfo.canUseSwiftAsyncContextAddrIntrinsic()) {
    llvm::Value *storedCallerContext = callerContext;
    auto contextLocationInExtendedFrame =
        Address(Builder.CreateIntrinsicCall(
                    llvm::Intrinsic::swift_async_context_addr, {}),
                IGM.Int8PtrTy, IGM.getPointerAlignment());
    // On arm64e we need to sign this pointer address discriminated
    // with 0xc31a and process dependent key.
    if (auto schema =
            IGM.getOptions().PointerAuth.AsyncContextExtendedFrameEntry) {
      auto authInfo = PointerAuthInfo::emit(
          *this, schema, contextLocationInExtendedFrame.getAddress(),
          PointerAuthEntity());
      storedCallerContext =
          emitPointerAuthSign(*this, storedCallerContext, authInfo);
    }
    Builder.CreateStore(storedCallerContext, contextLocationInExtendedFrame);
  }
  return callerContext;
}

llvm::Function *IRGenFunction::getOrCreateResumePrjFn(bool forPrologue) {
  // The prologue version lacks artificial debug info as this would cause
  // verification errors when it gets inlined.
  auto name = forPrologue ? "__swift_async_resume_project_context_prologue"
                          : "__swift_async_resume_project_context";
  auto Fn = cast<llvm::Function>(IGM.getOrCreateHelperFunction(
      name, IGM.Int8PtrTy, {IGM.Int8PtrTy},
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        auto &Builder = IGF.Builder;
        auto addr = &(*it);
        auto callerContext = IGF.emitAsyncResumeProjectContext(addr);
        Builder.CreateRet(callerContext);
      },
      false /*isNoInline*/, forPrologue));
  Fn->addFnAttr(llvm::Attribute::AlwaysInline);
  return Fn;
}
llvm::Function *
IRGenFunction::createAsyncDispatchFn(const FunctionPointer &fnPtr,
                                     ArrayRef<llvm::Value *> args) {
  SmallVector<llvm::Type*, 8> argTys;
  for (auto arg : args) {
    auto *ty = arg->getType();
    argTys.push_back(ty);
  }
  return createAsyncDispatchFn(fnPtr, argTys);
}

llvm::Function *
IRGenFunction::createAsyncDispatchFn(const FunctionPointer &fnPtr,
                                     ArrayRef<llvm::Type *> argTypes) {
  SmallVector<llvm::Type*, 8> argTys;
  argTys.push_back(IGM.Int8PtrTy); // Function pointer to be called.
  auto originalAuthInfo = fnPtr.getAuthInfo();
  if (fnPtr.getAuthInfo()) {
    argTys.push_back(IGM.Int64Ty); // Discriminator for the function pointer.
  }
  for (auto ty : argTypes) {
    argTys.push_back(ty);
  }
  auto calleeFnPtrType = fnPtr.getRawPointer()->getType();
  auto *dispatchFnTy =
      llvm::FunctionType::get(IGM.VoidTy, argTys, false /*vaargs*/);
  llvm::SmallString<40> name;
  llvm::raw_svector_ostream(name) << CurFn->getName() << ".0";
  llvm::Function *dispatch =
      llvm::Function::Create(dispatchFnTy, llvm::Function::InternalLinkage,
                             llvm::StringRef(name), &IGM.Module);
  dispatch->setCallingConv(IGM.SwiftAsyncCC);
  dispatch->setDoesNotThrow();
  IRGenFunction dispatchIGF(IGM, dispatch);
  // Don't emit debug info if we are generating a function for the prologue.
  if (IGM.DebugInfo && Builder.getCurrentDebugLocation())
    IGM.DebugInfo->emitOutlinedFunction(dispatchIGF, dispatch, CurFn->getName());
  auto &Builder = dispatchIGF.Builder;
  auto it = dispatchIGF.CurFn->arg_begin(), end = dispatchIGF.CurFn->arg_end();
  llvm::Value *fnPtrArg = &*(it++);
  llvm::Value *discriminatorArg = ((bool)originalAuthInfo) ? &*(it++) : nullptr;
  SmallVector<llvm::Value *, 8> callArgs;
  for (; it != end; ++it) {
    callArgs.push_back(&*it);
  }
  fnPtrArg = Builder.CreateBitOrPointerCast(fnPtrArg, calleeFnPtrType);
  PointerAuthInfo newAuthInfo =
      ((bool)originalAuthInfo)
          ? PointerAuthInfo(fnPtr.getAuthInfo().getKey(), discriminatorArg)
          : originalAuthInfo;
  auto callee = FunctionPointer::createSigned(
      fnPtr.getKind(), fnPtrArg, newAuthInfo, fnPtr.getSignature());
  auto call = Builder.CreateCall(callee, callArgs);
  call->setTailCallKind(IGM.AsyncTailCallKind);
  Builder.CreateRetVoid();
  return dispatch;
}

void IRGenFunction::emitSuspensionPoint(Explosion &toExecutor,
                                        llvm::Value *asyncResume) {

  // Setup the suspend point.
  SmallVector<llvm::Value *, 8> arguments;
  unsigned swiftAsyncContextIndex = 0;
  arguments.push_back(IGM.getInt32(swiftAsyncContextIndex)); // context index
  arguments.push_back(asyncResume);
  auto resumeProjFn = getOrCreateResumeFromSuspensionFn();
  arguments.push_back(
      Builder.CreateBitOrPointerCast(resumeProjFn, IGM.Int8PtrTy));
  llvm::Function *suspendFn = createAsyncSuspendFn();
  arguments.push_back(
      Builder.CreateBitOrPointerCast(suspendFn, IGM.Int8PtrTy));

  // Extra arguments to pass to the suspension function.
  arguments.push_back(asyncResume);
  arguments.push_back(toExecutor.claimNext());
  arguments.push_back(toExecutor.claimNext());
  arguments.push_back(getAsyncContext());
  auto resultTy = llvm::StructType::get(IGM.getLLVMContext(), {IGM.Int8PtrTy},
                                        false /*packed*/);
  emitSuspendAsyncCall(swiftAsyncContextIndex, resultTy, arguments);
}

llvm::Function *IRGenFunction::getOrCreateResumeFromSuspensionFn() {
  auto name = "__swift_async_resume_get_context";
  return cast<llvm::Function>(IGM.getOrCreateHelperFunction(
      name, IGM.Int8PtrTy, {IGM.Int8PtrTy},
      [&](IRGenFunction &IGF) {
        auto &Builder = IGF.Builder;
        Builder.CreateRet(&*IGF.CurFn->arg_begin());
      },
      false /*isNoInline*/));
}

llvm::Function *IRGenFunction::createAsyncSuspendFn() {
  llvm::SmallString<40> nameBuffer;
  llvm::raw_svector_ostream(nameBuffer) << CurFn->getName() << ".1";
  StringRef name(nameBuffer);
  if (llvm::GlobalValue *F = IGM.Module.getNamedValue(name))
    return cast<llvm::Function>(F);

  // The parameters here match the extra arguments passed to
  // @llvm.coro.suspend.async by emitSuspensionPoint.
  SmallVector<llvm::Type*, 8> argTys;
  argTys.push_back(IGM.Int8PtrTy); // resume function
  argTys.push_back(IGM.ExecutorFirstTy); // target executor (first half)
  argTys.push_back(IGM.ExecutorSecondTy); // target executor (second half)
  argTys.push_back(getAsyncContext()->getType()); // current context
  auto *suspendFnTy =
      llvm::FunctionType::get(IGM.VoidTy, argTys, false /*vaargs*/);

  llvm::Function *suspendFn =
      llvm::Function::Create(suspendFnTy, llvm::Function::InternalLinkage,
                             name, &IGM.Module);
  suspendFn->setCallingConv(IGM.SwiftAsyncCC);
  suspendFn->setDoesNotThrow();
  IRGenFunction suspendIGF(IGM, suspendFn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitOutlinedFunction(suspendIGF, suspendFn,
                                        CurFn->getName());
  auto &Builder = suspendIGF.Builder;

  llvm::Value *resumeFunction = suspendFn->getArg(0);
  llvm::Value *targetExecutorFirst = suspendFn->getArg(1);
  llvm::Value *targetExecutorSecond = suspendFn->getArg(2);
  llvm::Value *context = suspendFn->getArg(3);
  context = Builder.CreateBitCast(context, IGM.SwiftContextPtrTy);

  // Sign the task resume function with the C function pointer schema.
  if (auto schema = IGM.getOptions().PointerAuth.FunctionPointers) {
    // Use the Clang type for TaskContinuationFunction*
    // to make this work with type diversity.
    if (schema.hasOtherDiscrimination())
      schema = IGM.getOptions().PointerAuth.ClangTypeTaskContinuationFunction;
    auto authInfo = PointerAuthInfo::emit(suspendIGF, schema, nullptr,
                                          PointerAuthEntity());
    resumeFunction = emitPointerAuthSign(suspendIGF, resumeFunction, authInfo);
  }

  auto *suspendCall = Builder.CreateCall(
      IGM.getTaskSwitchFuncFunctionPointer(),
      {context, resumeFunction, targetExecutorFirst, targetExecutorSecond});
  suspendCall->setCallingConv(IGM.SwiftAsyncCC);
  suspendCall->setDoesNotThrow();
  suspendCall->setTailCallKind(IGM.AsyncTailCallKind);

  llvm::AttributeList attrs = suspendCall->getAttributes();
  IGM.addSwiftAsyncContextAttributes(attrs, /*context arg index*/ 0);
  suspendCall->setAttributes(attrs);

  Builder.CreateRetVoid();
  return suspendFn;
}

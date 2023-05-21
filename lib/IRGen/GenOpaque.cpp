//===--- GenOpaque.cpp - Swift IR-generation for opaque values ------------===//
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
//  This file implements IR generation for opaque values and value
//  witness operations.
//
//  In the comments throughout this file, three type names are used:
//    'B' is the type of a fixed-size buffer
//    'T' is the type which implements a protocol
//    'W' is the type of a witness to the protocol
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DerivedTypes.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/IRGen/ValueWitness.h"
#include "swift/SIL/TypeLowering.h"

#include "Callee.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenPointerAuth.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "ProtocolInfo.h"

#include "GenOpaque.h"

using namespace swift;
using namespace irgen;

/// A fixed-size buffer is always three pointers in size and pointer-aligned.
/// If we align them more, we'll need to introduce padding to
/// make protocol types work.
Size irgen::getFixedBufferSize(IRGenModule &IGM) {
  return NumWords_ValueBuffer * IGM.getPointerSize();
}
Alignment irgen::getFixedBufferAlignment(IRGenModule &IGM) {
  return IGM.getPointerAlignment();
}

/// Lazily create the standard fixed-buffer type.
llvm::Type *IRGenModule::getFixedBufferTy() {
  if (FixedBufferTy) return FixedBufferTy;

  auto size = getFixedBufferSize(*this).getValue();
  FixedBufferTy = llvm::ArrayType::get(Int8Ty, size);
  return FixedBufferTy;
}

static llvm::Type *createWitnessType(IRGenModule &IGM, ValueWitness index) {
  switch (index) {
  // void (*destroy)(T *object, witness_t *self);
  case ValueWitness::Destroy: {
    llvm::Type *args[] = { IGM.OpaquePtrTy, IGM.TypeMetadataPtrTy };
    return llvm::FunctionType::get(IGM.VoidTy, args, /*isVarArg*/ false);
  }

  // T *(*initializeBufferWithCopyOfBuffer)(B *dest, B *src, M *self);
  case ValueWitness::InitializeBufferWithCopyOfBuffer: {
    llvm::Type *bufPtrTy = IGM.getFixedBufferTy()->getPointerTo(0);
    llvm::Type *args[] = { bufPtrTy, bufPtrTy, IGM.TypeMetadataPtrTy };
    return llvm::FunctionType::get(IGM.OpaquePtrTy, args, /*isVarArg*/ false);
  }

  // T *(*assignWithCopy)(T *dest, T *src, M *self);
  // T *(*assignWithTake)(T *dest, T *src, M *self);
  // T *(*initializeWithCopy)(T *dest, T *src, M *self);
  // T *(*initializeWithTake)(T *dest, T *src, M *self);
  case ValueWitness::AssignWithCopy:
  case ValueWitness::AssignWithTake:
  case ValueWitness::InitializeWithCopy:
  case ValueWitness::InitializeWithTake: {
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *args[] = { ptrTy, ptrTy, IGM.TypeMetadataPtrTy };
    return llvm::FunctionType::get(ptrTy, args, /*isVarArg*/ false);
  }
      
  /// unsigned (*getEnumTag)(T *obj, M *self);
  case ValueWitness::GetEnumTag: {
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *metaTy = IGM.TypeMetadataPtrTy;
    llvm::Type *indexTy = IGM.Int32Ty;

    llvm::Type *args[] = {ptrTy, metaTy};

    return llvm::FunctionType::get(indexTy, args, /*isVarArg*/ false);
  }

  /// void (*destructiveProjectEnumData)(T *obj, M *self);
  case ValueWitness::DestructiveProjectEnumData: {
    llvm::Type *voidTy = IGM.VoidTy;
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *metaTy = IGM.TypeMetadataPtrTy;

    llvm::Type *args[] = {ptrTy, metaTy};

    return llvm::FunctionType::get(voidTy, args, /*isVarArg*/ false);
  }

  /// void (*destructiveInjectEnumTag)(T *obj, unsigned tag, M *self);
  case ValueWitness::DestructiveInjectEnumTag: {
    llvm::Type *voidTy = IGM.VoidTy;
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *indexTy = IGM.Int32Ty;
    llvm::Type *metaTy = IGM.TypeMetadataPtrTy;

    llvm::Type *args[] = {ptrTy, indexTy, metaTy};

    return llvm::FunctionType::get(voidTy, args, /*isVarArg*/ false);
  }

  /// unsigned (*getEnumTagSinglePayload)(const T* enum, UINT_TYPE emptyCases,
  ///                                     M *self)
  case ValueWitness::GetEnumTagSinglePayload: {
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *indexTy = IGM.Int32Ty;
    llvm::Type *metaTy = IGM.TypeMetadataPtrTy;

    llvm::Type *args[] = { ptrTy, indexTy, metaTy };
    return llvm::FunctionType::get(indexTy, args, false);
  }

  /// void (*storeEnumTagSinglePayload)(T* enum, UINT_TYPE whichCase,
  ///                                   UINT_TYPE emptyCases,
  ///                                   M *self)
  case ValueWitness::StoreEnumTagSinglePayload: {
    llvm::Type *voidTy = IGM.VoidTy;
    llvm::Type *ptrTy = IGM.OpaquePtrTy;
    llvm::Type *indexTy = IGM.Int32Ty;
    llvm::Type *metaTy = IGM.TypeMetadataPtrTy;

    llvm::Type *args[] = { ptrTy, indexTy, indexTy, metaTy };
    return llvm::FunctionType::get(voidTy, args, false);
  }

  case ValueWitness::Size:
  case ValueWitness::Stride:
    return IGM.SizeTy;
  case ValueWitness::Flags:
  case ValueWitness::ExtraInhabitantCount:
    return IGM.Int32Ty;
  }

  llvm_unreachable("bad value witness!");
}

static llvm::AttributeList getValueWitnessAttrs(IRGenModule &IGM,
                                                ValueWitness index) {
  assert(isValueWitnessFunction(index));

  auto &ctx = IGM.getLLVMContext();

  // All value witnesses are nounwind.
  auto attrs =
      llvm::AttributeList().addFnAttribute(ctx, llvm::Attribute::NoUnwind);

  switch (index) {
  // These have two arguments, but they can alias.
  case ValueWitness::AssignWithCopy:
    return attrs;

  // These have one argument.
  case ValueWitness::Destroy:
  case ValueWitness::DestructiveInjectEnumTag:
  case ValueWitness::DestructiveProjectEnumData:
  case ValueWitness::GetEnumTag:
  case ValueWitness::StoreEnumTagSinglePayload:
    return attrs.addParamAttribute(ctx, 0, llvm::Attribute::NoAlias);

  case ValueWitness::GetEnumTagSinglePayload:
    return attrs.addFnAttribute(ctx, llvm::Attribute::ReadOnly)
        .addParamAttribute(ctx, 0, llvm::Attribute::NoAlias);

  // These have two arguments and they don't alias each other.
  case ValueWitness::AssignWithTake:
  case ValueWitness::InitializeBufferWithCopyOfBuffer:
  case ValueWitness::InitializeWithCopy:
  case ValueWitness::InitializeWithTake:
    return attrs.addParamAttribute(ctx, 0, llvm::Attribute::NoAlias)
        .addParamAttribute(ctx, 1, llvm::Attribute::NoAlias);

  case ValueWitness::Size:
  case ValueWitness::Flags:
  case ValueWitness::ExtraInhabitantCount:
  case ValueWitness::Stride:
    llvm_unreachable("not a function value witness");
  }
  llvm_unreachable("bad witness");
}

/// Return the cached pointer-to-function type for the given value
/// witness index.
llvm::Type *IRGenModule::getValueWitnessTy(ValueWitness index) {
  assert(unsigned(index) < MaxNumValueWitnesses);
  auto &ty = ValueWitnessTys[unsigned(index)];
  if (ty) return ty;

  ty = createWitnessType(*this, index);
  return ty;
}

Signature IRGenModule::getValueWitnessSignature(ValueWitness index) {
  assert(isValueWitnessFunction(index));
  auto fnTy = cast<llvm::FunctionType>(getValueWitnessTy(index));
  auto attrs = getValueWitnessAttrs(*this, index);
  return Signature(fnTy, attrs, DefaultCC);
}

static StringRef getValueWitnessLabel(ValueWitness index) {
  switch (index) {
  case ValueWitness::Destroy:
    return "destroy";
  case ValueWitness::InitializeBufferWithCopyOfBuffer:
    return "initializeBufferWithCopyOfBuffer";
  case ValueWitness::AssignWithCopy:
    return "assignWithCopy";
  case ValueWitness::AssignWithTake:
    return "assignWithTake";
  case ValueWitness::InitializeWithCopy:
    return "initializeWithCopy";
  case ValueWitness::InitializeWithTake:
    return "initializeWithTake";
  case ValueWitness::Size:
    return "size";
  case ValueWitness::Flags:
    return "flags";
  case ValueWitness::ExtraInhabitantCount:
    return "extraInhabitantCount";
  case ValueWitness::Stride:
    return "stride";
  case ValueWitness::GetEnumTag:
    return "getEnumTag";
  case ValueWitness::DestructiveProjectEnumData:
    return "destructiveProjectEnumData";
  case ValueWitness::DestructiveInjectEnumTag:
    return "destructiveInjectEnumTag";
  case ValueWitness::GetEnumTagSinglePayload:
    return "getEnumTagSinglePayload";
  case ValueWitness::StoreEnumTagSinglePayload:
    return "storeEnumTagSinglePayload";
  }
  llvm_unreachable("bad value witness index");
}

static llvm::StructType *
getOrCreateValueWitnessTableTy(IRGenModule &IGM, llvm::StructType *&cache,
                               StringRef name, bool includeEnumWitnesses) {
  if (cache) return cache;

  SmallVector<llvm::Type*, 16> types;

#define FUNC(lowerId, upperId, retTy, paramTys) \
  types.push_back(IGM.Int8PtrTy);
#define DATA(lowerId, upperId, ty) \
  types.push_back(IGM.getValueWitnessTy(ValueWitness::upperId));

  // Add the base value witnesses.
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define FUNCTION_VALUE_WITNESS FUNC
#define DATA_VALUE_WITNESS DATA
#include "swift/ABI/ValueWitness.def"

  // Add the enum value witnesses.
  if (includeEnumWitnesses) {
#define WANT_ONLY_ENUM_VALUE_WITNESSES
#define FUNCTION_VALUE_WITNESS FUNC
#define DATA_VALUE_WITNESS DATA
#include "swift/ABI/ValueWitness.def"
  }

#undef DATA
#undef FUNC

  auto structTy = llvm::StructType::create(types, name);
  cache = structTy;
  return structTy;
}

llvm::StructType *IRGenModule::getValueWitnessTableTy() {
  return getOrCreateValueWitnessTableTy(*this, ValueWitnessTableTy,
                                        "swift.vwtable", false);
}
llvm::PointerType *IRGenModule::getValueWitnessTablePtrTy() {
  return getValueWitnessTableTy()->getPointerTo();
}

llvm::StructType *IRGenModule::getEnumValueWitnessTableTy() {
  return getOrCreateValueWitnessTableTy(*this, EnumValueWitnessTableTy,
                                        "swift.enum_vwtable", true);
}
llvm::PointerType *IRGenModule::getEnumValueWitnessTablePtrTy() {
  return getEnumValueWitnessTableTy()->getPointerTo();
}

Address irgen::slotForLoadOfOpaqueWitness(IRGenFunction &IGF,
                                          llvm::Value *table,
                                          WitnessIndex index,
                                          bool areEntriesRelative) {
  assert(table->getType() == IGF.IGM.WitnessTablePtrTy);

  // Are we loading from a relative protocol witness table.
  if (areEntriesRelative) {
    llvm::Value *slot =
      IGF.Builder.CreateBitOrPointerCast(table, IGF.IGM.RelativeAddressPtrTy);
    if (index.getValue() != 0)
      slot = IGF.Builder.CreateConstInBoundsGEP1_32(IGF.IGM.RelativeAddressTy,
                                                    slot, index.getValue());
    return Address(slot, IGF.IGM.RelativeAddressTy, Alignment(4));
  }

  // GEP to the appropriate index, avoiding spurious IR in the trivial case.
  llvm::Value *slot = table;
  if (index.getValue() != 0)
    slot = IGF.Builder.CreateConstInBoundsGEP1_32(IGF.IGM.WitnessTableTy, table,
                                                  index.getValue());

  return Address(slot, IGF.IGM.WitnessTableTy, IGF.IGM.getPointerAlignment());
}

/// Load a specific witness from a known table.  The result is
/// always an i8*.
llvm::Value *irgen::emitInvariantLoadOfOpaqueWitness(IRGenFunction &IGF,
                                                     bool isProtocolWitness,
                                                     llvm::Value *table,
                                                     WitnessIndex index,
                                                     llvm::Value **slotPtr) {
  // Is this is a load of a relative protocol witness table entry.
  auto isRelativeTable = IGF.IGM.IRGen.Opts.UseRelativeProtocolWitnessTables &&
    isProtocolWitness;

  auto slot = slotForLoadOfOpaqueWitness(IGF, table, index, isRelativeTable);
  if (slotPtr) *slotPtr = slot.getAddress();

  if (isRelativeTable) {
    return IGF.emitLoadOfRelativePointer(slot, false, IGF.IGM.Int8Ty);
  }

  return IGF.emitInvariantLoad(slot);
}

/// Load a specific witness from a known table.  The result is
/// always an i8*.
llvm::Value *irgen::emitInvariantLoadOfOpaqueWitness(IRGenFunction &IGF,
                                                     bool isProtocolWitness,
                                                     llvm::Value *table,
                                                     llvm::Value *index,
                                                     llvm::Value **slotPtr) {
  assert(table->getType() == IGF.IGM.WitnessTablePtrTy);
  assert(!isProtocolWitness &&
         "This function does not yet support relative protocol witnesses");

  // GEP to the appropriate index.
  llvm::Value *slot =
      IGF.Builder.CreateInBoundsGEP(IGF.IGM.WitnessTableTy, table, index);

  if (slotPtr) *slotPtr = slot;

  auto witness = IGF.Builder.CreateLoad(
      Address(slot, IGF.IGM.WitnessTableTy, IGF.IGM.getPointerAlignment()));
  IGF.setInvariantLoad(witness);
  return witness;
}

static Address emitAddressOfValueWitnessTableValue(IRGenFunction &IGF,
                                                   llvm::Value *table,
                                                   ValueWitness witness) {
  assert(!isValueWitnessFunction(witness));
  assert(unsigned(witness) <= unsigned(ValueWitness::ExtraInhabitantCount) &&
         "extraInhabitantCount not the last non-function value witness");

  auto pointerSize = IGF.IGM.getPointerSize();

  // Most of the witnesses are at an offset that's just a multiple of the
  // pointer size, but the extra-inhabitant count is packed in after the
  // 32-bit flags.
  // This computation is correct for all pointer sizes, including 16.
  // It would be wrong if size_t is ever a different size from the pointer
  // size, though.
  Size offset =
    (witness == ValueWitness::ExtraInhabitantCount
       ? unsigned(ValueWitness::Flags) * pointerSize + Size(4)
       : unsigned(witness) * pointerSize);

  Address addr =
      Address(table, IGF.IGM.WitnessTableTy, IGF.IGM.getPointerAlignment());
  addr =
      IGF.Builder.CreateElementBitCast(addr, IGF.IGM.getValueWitnessTableTy());
  addr = IGF.Builder.CreateStructGEP(addr, unsigned(witness), offset);
  return addr;
}

/// Given a value witness table, load one of the value witnesses.
/// The result has the appropriate type for the witness.
static llvm::Value *emitLoadOfValueWitnessValue(IRGenFunction &IGF,
                                                llvm::Value *table,
                                                ValueWitness witness) {
  auto addr = emitAddressOfValueWitnessTableValue(IGF, table, witness);
  auto load = IGF.Builder.CreateLoad(addr, getValueWitnessLabel(witness));
  IGF.setInvariantLoad(load);
  return load;
}

/// Given a type metadata pointer, load one of the value witnesses from its
/// value witness table.
static llvm::Value *
emitLoadOfValueWitnessValueFromMetadata(IRGenFunction &IGF,
                                        llvm::Value *metadata,
                                        ValueWitness index) {
  llvm::Value *vwtable = IGF.emitValueWitnessTableRefForMetadata(metadata);
  return emitLoadOfValueWitnessValue(IGF, vwtable, index);
}

/// Given a value witness table, load one of the value witnesses.
/// The result has the appropriate type for the witness.
static FunctionPointer emitLoadOfValueWitnessFunction(IRGenFunction &IGF,
                                                      llvm::Value *table,
                                                      ValueWitness index) {
  assert(isValueWitnessFunction(index));
  WitnessIndex windex = [&] {
    unsigned i = unsigned(index);
    if (i > unsigned(ValueWitness::Flags)) {
      if (IGF.IGM.getPointerSize() == Size(8)) {
        --i; // one pointer width skips both flags and xiCount
      } else if (IGF.IGM.getPointerSize() == Size(4)) {
        // no adjustment required
      } else {
        assert(IGF.IGM.getPointerSize() == Size(2));
        i += 2; // flags and xiCount take up two pointers apiece
      }
    }
    return WitnessIndex(i, false);
  }();

  llvm::Value *slot;
  llvm::Value *witness =
    emitInvariantLoadOfOpaqueWitness(IGF, /*isProtocolWitness*/false, table,
                                     windex, &slot);
  auto label = getValueWitnessLabel(index);
  auto signature = IGF.IGM.getValueWitnessSignature(index);

  auto type = signature.getType()->getPointerTo();
  witness = IGF.Builder.CreateBitCast(witness, type, label);

  auto authInfo = PointerAuthInfo::emit(IGF,
                                    IGF.getOptions().PointerAuth.ValueWitnesses,
                                        slot, index);

  return FunctionPointer::createSigned(FunctionPointer::Kind::Function, witness,
                                       authInfo, signature);
}

/// Given a type metadata pointer, load one of the function
/// value witnesses from its value witness table.
static FunctionPointer
emitLoadOfValueWitnessFunctionFromMetadata(IRGenFunction &IGF,
                                           llvm::Value *metadata,
                                           ValueWitness index) {
  llvm::Value *vwtable = IGF.emitValueWitnessTableRefForMetadata(metadata);
  return emitLoadOfValueWitnessFunction(IGF, vwtable, index);
}

llvm::Value *IRGenFunction::emitValueWitnessValue(SILType type,
                                                  ValueWitness index) {
  assert(!isValueWitnessFunction(index));

  auto key = LocalTypeDataKind::forValueWitness(index);
  if (auto witness = tryGetLocalTypeDataForLayout(type, key)) {
    return witness;
  }
  
  auto vwtable = emitValueWitnessTableRef(type);
  auto witness = emitLoadOfValueWitnessValue(*this, vwtable, index);
  setScopedLocalTypeDataForLayout(type, key, witness);
  return witness;
}

FunctionPointer
IRGenFunction::emitValueWitnessFunctionRef(SILType type,
                                           llvm::Value *&metadataSlot,
                                           ValueWitness index) {
  assert(isValueWitnessFunction(index));

  auto key = LocalTypeDataKind::forValueWitness(index);
  if (auto witness = tryGetLocalTypeDataForLayout(type, key)) {
    metadataSlot = emitTypeMetadataRefForLayout(type);
    auto signature = IGM.getValueWitnessSignature(index);
    PointerAuthInfo authInfo;
    if (auto &schema = getOptions().PointerAuth.ValueWitnesses) {
      auto discriminator =
        tryGetLocalTypeDataForLayout(type,
                        LocalTypeDataKind::forValueWitnessDiscriminator(index));
      assert(discriminator && "no saved discriminator for value witness fn!");
      authInfo = PointerAuthInfo(schema.getKey(), discriminator);
    }
    return FunctionPointer::createSigned(FunctionPointer::Kind::Function,
                                         witness, authInfo, signature);
  }
  
  auto vwtable = emitValueWitnessTableRef(type, &metadataSlot);
  auto witness = emitLoadOfValueWitnessFunction(*this, vwtable, index);
  setScopedLocalTypeDataForLayout(type, key, witness.getRawPointer());
  if (auto &authInfo = witness.getAuthInfo()) {
    setScopedLocalTypeDataForLayout(type,
                        LocalTypeDataKind::forValueWitnessDiscriminator(index),
                                    authInfo.getDiscriminator());
  }

  return witness;
}

static llvm::Value *emitCastToOpaquePtr(IRGenFunction &IGF,
                                        Address object) {
  return IGF.Builder.CreateBitCast(object.getAddress(), IGF.IGM.OpaquePtrTy);
}

llvm::Value *irgen::emitInitializeBufferWithCopyOfBufferCall(IRGenFunction &IGF,
                                                     SILType T,
                                                     Address destBuffer,
                                                     Address srcBuffer) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  return emitInitializeBufferWithCopyOfBufferCall(IGF, metadata,
                                                  destBuffer, srcBuffer);
}

/// Emit a call to do an 'initializeBufferWithCopyOfBuffer' operation.
llvm::Value *
irgen::emitInitializeBufferWithCopyOfBufferCall(IRGenFunction &IGF,
                                                llvm::Value *metadata,
                                                Address destBuffer,
                                                Address srcBuffer) {
  auto copyFn = emitLoadOfValueWitnessFunctionFromMetadata(IGF, metadata,
                             ValueWitness::InitializeBufferWithCopyOfBuffer);
  llvm::CallInst *call =
    IGF.Builder.CreateCall(copyFn,
      {destBuffer.getAddress(), srcBuffer.getAddress(), metadata});

  return call;
}

/// Emit a dynamic alloca call to allocate enough memory to hold an object of
/// type 'T' and an optional llvm.stackrestore point if 'isInEntryBlock' is
/// false.
StackAddress IRGenFunction::emitDynamicAlloca(SILType T,
                                              const llvm::Twine &name) {
  llvm::Value *size = emitLoadOfSize(*this, T);
  return emitDynamicAlloca(IGM.Int8Ty, size, Alignment(16), true, name);
}

StackAddress IRGenFunction::emitDynamicAlloca(llvm::Type *eltTy,
                                              llvm::Value *arraySize,
                                              Alignment align,
                                              bool allowTaskAlloc,
                                              const llvm::Twine &name) {
  // Async functions call task alloc.
  if (allowTaskAlloc && isAsync()) {
    llvm::Value *byteCount;
    auto eltSize = IGM.DataLayout.getTypeAllocSize(eltTy);
    if (eltSize == 1) {
      byteCount = arraySize;
    } else {
      byteCount = Builder.CreateMul(arraySize, IGM.getSize(Size(eltSize)));
    }
    // The task allocator wants size increments in the multiple of
    // MaximumAlignment.
    byteCount = alignUpToMaximumAlignment(IGM.SizeTy, byteCount);
    auto address = emitTaskAlloc(byteCount, align);
    auto stackAddress = StackAddress{address, address.getAddress()};
    stackAddress = stackAddress.withAddress(
        Builder.CreateElementBitCast(stackAddress.getAddress(), eltTy));
    return stackAddress;
    // In coroutines, call llvm.coro.alloca.alloc.
  } else if (isCoroutine()) {
    // NOTE: llvm does not support dynamic allocas in coroutines.

    // Compute the number of bytes to allocate.
    llvm::Value *byteCount;
    auto eltSize = IGM.DataLayout.getTypeAllocSize(eltTy);
    if (eltSize == 1) {
      byteCount = arraySize;
    } else {
      byteCount = Builder.CreateMul(arraySize, IGM.getSize(Size(eltSize)));
    }

    auto alignment = llvm::ConstantInt::get(IGM.Int32Ty, align.getValue());

    // Allocate memory.  This produces an abstract token.
    auto allocToken =
        Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_alloca_alloc,
                                    {IGM.SizeTy}, {byteCount, alignment});

    // Get the allocation result.
    auto ptr = Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_alloca_get,
                                           {allocToken});

    auto stackAddress =
        StackAddress{Address(ptr, IGM.Int8Ty, align), allocToken};
    stackAddress = stackAddress.withAddress(
        Builder.CreateElementBitCast(stackAddress.getAddress(), eltTy));
    return stackAddress;
  }

  // Otherwise, use a dynamic alloca.
  llvm::Value *stackRestorePoint = nullptr;

  // Save the stack pointer if we are not in the entry block (we could be
  // executed more than once).
  bool isInEntryBlock = (Builder.GetInsertBlock() == &*CurFn->begin());
  if (!isInEntryBlock) {
    stackRestorePoint =
        Builder.CreateIntrinsicCall(llvm::Intrinsic::stacksave, {}, "spsave");
  }

  // Emit the dynamic alloca.
  auto *alloca = Builder.IRBuilderBase::CreateAlloca(eltTy, arraySize, name);
  alloca->setAlignment(llvm::MaybeAlign(align.getValue()).valueOrOne());

  assert(!isInEntryBlock ||
         getActiveDominancePoint().isUniversal() &&
             "Must be in entry block if we insert dynamic alloca's without "
             "stackrestores");
  return {Address(alloca, eltTy, align), stackRestorePoint};
}

/// Deallocate dynamic alloca's memory if requested by restoring the stack
/// location before the dynamic alloca's call.
void IRGenFunction::emitDeallocateDynamicAlloca(StackAddress address,
                                                bool allowTaskDealloc) {
  // Async function use taskDealloc.
  if (allowTaskDealloc && isAsync() && address.getAddress().isValid()) {
    emitTaskDealloc(
        Address(address.getExtraInfo(), IGM.Int8Ty, address.getAlignment()));
    return;
  }
  // In coroutines, unconditionally call llvm.coro.alloca.free.
  // Except if the address is invalid, this happens when this is a StackAddress
  // for a partial_apply [stack] that did not need a context object on the
  // stack.
  else if (isCoroutine() && address.getAddress().isValid()) {
    // NOTE: llvm does not support dynamic allocas in coroutines.

    auto allocToken = address.getExtraInfo();
    assert(allocToken && "dynamic alloca in coroutine without alloc token?");
    Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_alloca_free, allocToken);
    return;
  }
  // Otherwise, call llvm.stackrestore if an address was saved.
  auto savedSP = address.getExtraInfo();
  if (savedSP == nullptr)
    return;
  Builder.CreateIntrinsicCall(llvm::Intrinsic::stackrestore, savedSP);
}

/// Emit a call to do an 'initializeArrayWithCopy' operation.
void irgen::emitInitializeArrayWithCopyCall(IRGenFunction &IGF,
                                            SILType T,
                                            Address destObject,
                                            Address srcObject,
                                            llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(IGF.IGM.getArrayInitWithCopyFunctionPointer(),
                         {dest, src, count, metadata});
}

/// Emit a call to do an 'initializeArrayWithTakeNoAlias' operation.
void irgen::emitInitializeArrayWithTakeNoAliasCall(IRGenFunction &IGF,
                                                   SILType T,
                                                   Address destObject,
                                                   Address srcObject,
                                                   llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(IGF.IGM.getArrayInitWithTakeNoAliasFunctionPointer(),
                         {dest, src, count, metadata});
}

/// Emit a call to do an 'initializeArrayWithTakeFrontToBack' operation.
void irgen::emitInitializeArrayWithTakeFrontToBackCall(IRGenFunction &IGF,
                                            SILType T,
                                            Address destObject,
                                            Address srcObject,
                                            llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(
      IGF.IGM.getArrayInitWithTakeFrontToBackFunctionPointer(),
      {dest, src, count, metadata});
}

/// Emit a call to do an 'initializeArrayWithTakeBackToFront' operation.
void irgen::emitInitializeArrayWithTakeBackToFrontCall(IRGenFunction &IGF,
                                            SILType T,
                                            Address destObject,
                                            Address srcObject,
                                            llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(
      IGF.IGM.getArrayInitWithTakeBackToFrontFunctionPointer(),
      {dest, src, count, metadata});
}

/// Emit a call to do an 'assignWithCopy' operation.
void irgen::emitAssignWithCopyCall(IRGenFunction &IGF,
                                   SILType T,
                                   Address destObject,
                                   Address srcObject) {
  llvm::Value *metadata;
  auto copyFn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                                ValueWitness::AssignWithCopy);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(copyFn, {dest, src, metadata});
}

/// Emit a call to do an 'assignWithCopy' operation.
void irgen::emitAssignWithCopyCall(IRGenFunction &IGF,
                                   llvm::Value *metadata,
                                   Address destObject,
                                   Address srcObject) {
  auto copyFn = emitLoadOfValueWitnessFunctionFromMetadata(IGF, metadata,
                                         ValueWitness::AssignWithCopy);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(copyFn, {dest, src, metadata});
}

/// Emit a call to do an 'arrayAssignWithCopyNoAlias' operation.
void irgen::emitAssignArrayWithCopyNoAliasCall(IRGenFunction &IGF, SILType T,
                                          Address destObject, Address srcObject,
                                          llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(IGF.IGM.getArrayAssignWithCopyNoAliasFunctionPointer(),
                         {dest, src, count, metadata});
}

/// Emit a call to do an 'arrayAssignWithCopyFrontToBack' operation.
void irgen::emitAssignArrayWithCopyFrontToBackCall(IRGenFunction &IGF,
                                                   SILType T,
                                                   Address destObject,
                                                   Address srcObject,
                                                   llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(
      IGF.IGM.getArrayAssignWithCopyFrontToBackFunctionPointer(),
      {dest, src, count, metadata});
}

/// Emit a call to do an 'arrayAssignWithCopyBackToFront' operation.
void irgen::emitAssignArrayWithCopyBackToFrontCall(IRGenFunction &IGF,
                                                   SILType T,
                                                   Address destObject,
                                                   Address srcObject,
                                                   llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(
      IGF.IGM.getArrayAssignWithCopyBackToFrontFunctionPointer(),
      {dest, src, count, metadata});
}

/// Emit a call to do an 'assignWithTake' operation.
void irgen::emitAssignWithTakeCall(IRGenFunction &IGF,
                                   SILType T,
                                   Address destObject,
                                   Address srcObject) {
  llvm::Value *metadata;
  auto copyFn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                                ValueWitness::AssignWithTake);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(copyFn, {dest, src, metadata});
}

/// Emit a call to do an 'arrayAssignWithTake' operation.
void irgen::emitAssignArrayWithTakeCall(IRGenFunction &IGF, SILType T,
                                        Address destObject, Address srcObject,
                                        llvm::Value *count) {
  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(IGF.IGM.getArrayAssignWithTakeFunctionPointer(),
                         {dest, src, count, metadata});
}

/// Emit a call to do a 'destroyArray' operation.
void irgen::emitDestroyArrayCall(IRGenFunction &IGF,
                                 SILType T,
                                 Address object,
                                 llvm::Value *count) {
  // If T is a trivial/POD type, nothing needs to be done.
  if (IGF.IGM.getTypeLowering(T).isTrivial())
    return;

  auto metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto obj = emitCastToOpaquePtr(IGF, object);
  IGF.Builder.CreateCall(IGF.IGM.getArrayDestroyFunctionPointer(),
                         {obj, count, metadata});
}

/// Emit a trampoline to call the getEnumTagSinglePayload witness. API:
/// UINT_TYPE (const T* enum, UINT_TYPE emptyCases, M *self)
static llvm::Constant *
getGetEnumTagSinglePayloadTrampolineFn(IRGenModule &IGM) {

  llvm::Type *argTys[] = {IGM.OpaquePtrTy, IGM.Int32Ty, IGM.TypeMetadataPtrTy};

  llvm::SmallString<40> fnName("__swift_getEnumTagSinglePayload");

  auto func = IGM.getOrCreateHelperFunction(
      fnName, IGM.Int32Ty, argTys,
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        auto *enumAddr = &*(it++);
        auto *numEmptyCases = &*(it++);
        auto *metadata = &*(it++);
        auto &Builder = IGF.Builder;
        auto witnessFunc = emitLoadOfValueWitnessFunctionFromMetadata(
            IGF, metadata, ValueWitness::GetEnumTagSinglePayload);
        auto *result = Builder.CreateCall(witnessFunc,
                                          {enumAddr, numEmptyCases, metadata});
        Builder.CreateRet(result);
      },
      true /*noinline*/);

  // This function is readonly.
  cast<llvm::Function>(func)->addFnAttr(llvm::Attribute::ReadOnly);
  return func;
}

/// Emit a trampoline to call the storeEnumTagSinglePayload witness. API:
/// VOID_TYPE (const T* enum, UINT_TYPE whichCase, UINT_TYPE emptyCases,
///            M *self)
static llvm::Constant *
getStoreEnumTagSinglePayloadTrampolineFn(IRGenModule &IGM) {

  llvm::Type *argTys[] = {IGM.OpaquePtrTy, IGM.Int32Ty, IGM.Int32Ty,
                          IGM.TypeMetadataPtrTy};

  llvm::SmallString<40> fnName("__swift_storeEnumTagSinglePayload");

  return IGM.getOrCreateHelperFunction(
      fnName, IGM.VoidTy, argTys,
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        auto *enumAddr = &*(it++);
        auto *whichCase = &*(it++);
        auto *numEmptyCases = &*(it++);
        auto *metadata = &*(it++);
        auto &Builder = IGF.Builder;
        auto witnessFunc = emitLoadOfValueWitnessFunctionFromMetadata(
            IGF, metadata, ValueWitness::StoreEnumTagSinglePayload);
        Builder.CreateCall(witnessFunc,
                           {enumAddr, whichCase, numEmptyCases, metadata});
        Builder.CreateRetVoid();
      },
      true /*noinline*/);
}

llvm::Value *irgen::emitGetEnumTagSinglePayloadCall(IRGenFunction &IGF,
                                                    SILType T,
                                                    llvm::Value *numEmptyCases,
                                                    Address destObject) {
  if (!IGF.optimizeForSize()) {
    llvm::Value *metadata;
    auto fn = IGF.emitValueWitnessFunctionRef(
        T, metadata, ValueWitness::GetEnumTagSinglePayload);
    auto dest = emitCastToOpaquePtr(IGF, destObject);
    llvm::CallInst *call = IGF.Builder.CreateCall(
        fn, {dest, numEmptyCases, metadata});
    return call;
  }
  auto *metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto *func = getGetEnumTagSinglePayloadTrampolineFn(IGF.IGM);
  auto *fnType = cast<llvm::Function>(func)->getFunctionType();
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  auto *result =
      IGF.Builder.CreateCall(fnType, func, {dest, numEmptyCases, metadata});
  return result;
}

void irgen::emitStoreEnumTagSinglePayloadCall(
    IRGenFunction &IGF, SILType T, llvm::Value *whichCase,
    llvm::Value *numEmptyCases, Address destObject) {
  if (!IGF.optimizeForSize()) {
    llvm::Value *metadata;
    auto fn = IGF.emitValueWitnessFunctionRef(
        T, metadata, ValueWitness::StoreEnumTagSinglePayload);
    auto dest = emitCastToOpaquePtr(IGF, destObject);
    IGF.Builder.CreateCall(fn, {dest, whichCase, numEmptyCases, metadata});
    return;
  }

  auto *metadata = IGF.emitTypeMetadataRefForLayout(T);
  auto *func = getStoreEnumTagSinglePayloadTrampolineFn(IGF.IGM);
  auto *fnType = cast<llvm::Function>(func)->getFunctionType();
  auto dest = emitCastToOpaquePtr(IGF, destObject);
  IGF.Builder.CreateCall(fnType, func,
                         {dest, whichCase, numEmptyCases, metadata});
}

/// Emit a call to the 'getEnumTag' operation.
llvm::Value *irgen::emitGetEnumTagCall(IRGenFunction &IGF,
                                       SILType T,
                                       Address srcObject) {
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                            ValueWitness::GetEnumTag);

  auto src = emitCastToOpaquePtr(IGF, srcObject);
  llvm::CallInst *call =
    IGF.Builder.CreateCall(fn, {src, metadata});
  return call;
}

/// Emit a call to the 'destructiveProjectEnumData' operation.
/// The type must be dynamically known to have enum witnesses.
void irgen::emitDestructiveProjectEnumDataCall(IRGenFunction &IGF,
                                               SILType T,
                                               Address srcObject) {
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                    ValueWitness::DestructiveProjectEnumData);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(fn, {src, metadata});
}

/// Emit a call to the 'destructiveInjectEnumTag' operation.
/// The type must be dynamically known to have enum witnesses.
void irgen::emitDestructiveInjectEnumTagCall(IRGenFunction &IGF,
                                             SILType T,
                                             llvm::Value *tagValue,
                                             Address srcObject) {
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                      ValueWitness::DestructiveInjectEnumTag);
  auto src = emitCastToOpaquePtr(IGF, srcObject);
  IGF.Builder.CreateCall(fn, {src, tagValue, metadata});
}

/// Load the 'size' value witness from the given table as a size_t.
llvm::Value *irgen::emitLoadOfSize(IRGenFunction &IGF, SILType T) {
  return IGF.emitValueWitnessValue(T, ValueWitness::Size);
}

/// Load the 'alignmentMask' value witness from the given table as a size_t.
llvm::Value *irgen::emitLoadOfAlignmentMask(IRGenFunction &IGF, SILType T) {
  auto flags = IGF.emitValueWitnessValue(T, ValueWitness::Flags);
  return emitAlignMaskFromFlags(IGF, flags);
}

/// Load the 'isTriviallyDestroyable' valueWitness from the given table as an i1.
llvm::Value *irgen::emitLoadOfIsTriviallyDestroyable(IRGenFunction &IGF, SILType T) {
  auto flags = IGF.emitValueWitnessValue(T, ValueWitness::Flags);
  auto mask = IGF.IGM.getInt32(ValueWitnessFlags::IsNonPOD);
  auto masked = IGF.Builder.CreateAnd(flags, mask);
  return IGF.Builder.CreateICmpEQ(masked, IGF.IGM.getInt32(0),
                                  flags->getName() + ".isTriviallyDestroyable");
}

/// Load the 'isBitwiseTakable' valueWitness from the given table as an i1.
llvm::Value *irgen::emitLoadOfIsBitwiseTakable(IRGenFunction &IGF, SILType T) {
  auto flags = IGF.emitValueWitnessValue(T, ValueWitness::Flags);
  auto mask = IGF.IGM.getInt32(ValueWitnessFlags::IsNonBitwiseTakable);
  auto masked = IGF.Builder.CreateAnd(flags, mask);
  return IGF.Builder.CreateICmpEQ(masked, IGF.IGM.getInt32(0),
                                  flags->getName() + ".isBitwiseTakable");
}

/// Load the 'isInline' valueWitness from the given table as an i1.
llvm::Value *irgen::emitLoadOfIsInline(IRGenFunction &IGF, SILType T) {
  auto flags = IGF.emitValueWitnessValue(T, ValueWitness::Flags);
  auto mask = IGF.IGM.getInt32(ValueWitnessFlags::IsNonInline);
  auto masked = IGF.Builder.CreateAnd(flags, mask);
  return IGF.Builder.CreateICmpEQ(masked, IGF.IGM.getInt32(0),
                                  flags->getName() + ".isInline");
}

/// Load the 'stride' value witness from the given table as a size_t.
llvm::Value *irgen::emitLoadOfStride(IRGenFunction &IGF, SILType T) {
  return IGF.emitValueWitnessValue(T, ValueWitness::Stride);
}

llvm::Value *irgen::emitLoadOfExtraInhabitantCount(IRGenFunction &IGF,
                                                   SILType T) {
  return IGF.emitValueWitnessValue(T, ValueWitness::ExtraInhabitantCount);
}

void irgen::emitStoreOfExtraInhabitantCount(IRGenFunction &IGF,
                                            llvm::Value *value,
                                            llvm::Value *metadata) {
  auto vwTable = IGF.emitValueWitnessTableRefForMetadata(metadata);
  auto addr = emitAddressOfValueWitnessTableValue(
      IGF, vwTable, ValueWitness::ExtraInhabitantCount);
  IGF.Builder.CreateStore(value, addr);
}

std::pair<llvm::Value *, llvm::Value *>
irgen::emitLoadOfIsInline(IRGenFunction &IGF, llvm::Value *metadata) {
  auto *flags = emitLoadOfValueWitnessValueFromMetadata(IGF, metadata,
                                                        ValueWitness::Flags);
  auto mask = IGF.IGM.getInt32(ValueWitnessFlags::IsNonInline);
  auto masked = IGF.Builder.CreateAnd(flags, mask);
  return std::make_pair(
      IGF.Builder.CreateICmpEQ(masked, IGF.IGM.getInt32(0),
                               flags->getName() + ".isInline"),
      flags);
}

llvm::Value *irgen::emitLoadOfSize(IRGenFunction &IGF, llvm::Value *metadata) {
  auto *size = emitLoadOfValueWitnessValueFromMetadata(IGF, metadata,
                                                       ValueWitness::Size);
  return size;
}

llvm::Value *irgen::emitAlignMaskFromFlags(IRGenFunction &IGF,
                                           llvm::Value *flags) {
  auto flagsAsSize = IGF.Builder.CreateZExtOrTrunc(flags, IGF.IGM.SizeTy);
  auto *alignMask = IGF.IGM.getSize(Size(ValueWitnessFlags::AlignmentMask));
  return IGF.Builder.CreateAnd(flagsAsSize, alignMask,
                               flags->getName() + ".alignmentMask");
}

/// Emit a call to do an 'initializeWithCopy' operation.
void irgen::emitInitializeWithCopyCall(IRGenFunction &IGF,
                                       SILType T,
                                       Address dest,
                                       Address src) {
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                            ValueWitness::InitializeWithCopy);
  auto destPtr = emitCastToOpaquePtr(IGF, dest);
  auto srcPtr = emitCastToOpaquePtr(IGF, src);
  IGF.Builder.CreateCall(fn, {destPtr, srcPtr, metadata});
}

llvm::Value *irgen::emitInitializeWithCopyCall(IRGenFunction &IGF,
                                               llvm::Value *metadata,
                                               Address dest, Address src) {
  auto copyFn = emitLoadOfValueWitnessFunctionFromMetadata(
      IGF, metadata, ValueWitness::InitializeWithCopy);
  auto destPtr = emitCastToOpaquePtr(IGF, dest);
  auto srcPtr = emitCastToOpaquePtr(IGF, src);
  llvm::CallInst *call = IGF.Builder.CreateCall(
      copyFn, {destPtr, srcPtr, metadata});

  return call;
}

/// Emit a call to do an 'initializeWithTake' operation.
void irgen::emitInitializeWithTakeCall(IRGenFunction &IGF,
                                       SILType T,
                                       Address dest,
                                       Address src) {
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                            ValueWitness::InitializeWithTake);
  auto destPtr = emitCastToOpaquePtr(IGF, dest);
  auto srcPtr = emitCastToOpaquePtr(IGF, src);
  IGF.Builder.CreateCall(fn, {destPtr, srcPtr, metadata});
}

llvm::Value *irgen::emitInitializeWithTakeCall(IRGenFunction &IGF,
                                               llvm::Value *metadata,
                                               Address dest, Address src) {
  auto copyFn = emitLoadOfValueWitnessFunctionFromMetadata(
      IGF, metadata, ValueWitness::InitializeWithTake);
  auto destPtr = emitCastToOpaquePtr(IGF, dest);
  auto srcPtr = emitCastToOpaquePtr(IGF, src);
  llvm::CallInst *call =
    IGF.Builder.CreateCall(copyFn, {destPtr, srcPtr, metadata});

  return call;
}

/// Emit a call to do a 'destroy' operation.
void irgen::emitDestroyCall(IRGenFunction &IGF,
                            SILType T,
                            Address object) {
  // If T is a trivial/POD type, nothing needs to be done.
  if (IGF.IGM.getTypeLowering(T).isTrivial())
    return;
  llvm::Value *metadata;
  auto fn = IGF.emitValueWitnessFunctionRef(T, metadata,
                                            ValueWitness::Destroy);
  auto objectPtr = emitCastToOpaquePtr(IGF, object);
  IGF.Builder.CreateCall(fn, {objectPtr, metadata});
}

void irgen::emitDestroyCall(IRGenFunction &IGF, llvm::Value *metadata,
                            Address object) {
  auto fn = emitLoadOfValueWitnessFunctionFromMetadata(IGF, metadata,
                                                       ValueWitness::Destroy);
  auto objectPtr = emitCastToOpaquePtr(IGF, object);
  IGF.Builder.CreateCall(fn, {objectPtr, metadata});
}

static llvm::Constant *getAllocateValueBufferFunction(IRGenModule &IGM) {

  llvm::Type *argTys[] = {IGM.TypeMetadataPtrTy, IGM.OpaquePtrTy};

  llvm::SmallString<40> fnName("__swift_allocate_value_buffer");

  return IGM.getOrCreateHelperFunction(
      fnName, IGM.OpaquePtrTy, argTys,
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        auto *metadata = &*(it++);
        auto buffer = Address(&*(it++), IGM.OpaqueTy, Alignment(1));

        // Dynamically check whether this type is inline or needs an allocation.
        llvm::Value *isInline, *flags;
        std::tie(isInline, flags) = emitLoadOfIsInline(IGF, metadata);

        auto *outlineBB = IGF.createBasicBlock("outline.allocateValueInBuffer");
        auto *doneBB = IGF.createBasicBlock("done");
        llvm::Value *addressInline, *addressOutline;
        addressInline = buffer.getAddress();
        auto *origBB = IGF.Builder.GetInsertBlock();
        IGF.Builder.CreateCondBr(isInline, doneBB, outlineBB);

        IGF.Builder.emitBlock(outlineBB);
        {
          auto *size = emitLoadOfSize(IGF, metadata);
          auto *alignMask = emitAlignMaskFromFlags(IGF, flags);
          auto valueAddr =
              IGF.emitAllocRawCall(size, alignMask, "outline.ValueBuffer");
          IGF.Builder.CreateStore(
              valueAddr, Address(IGF.Builder.CreateBitCast(
                                     buffer.getAddress(),
                                     valueAddr->getType()->getPointerTo()),
                                 IGM.Int8PtrTy, Alignment(1)));
          addressOutline =
              IGF.Builder.CreateBitCast(valueAddr, IGM.OpaquePtrTy);
          IGF.Builder.CreateBr(doneBB);
        }

        IGF.Builder.emitBlock(doneBB);
        auto *addressOfValue = IGF.Builder.CreatePHI(IGM.OpaquePtrTy, 2);
        addressOfValue->addIncoming(addressInline, origBB);
        addressOfValue->addIncoming(addressOutline, outlineBB);
        IGF.Builder.CreateRet(addressOfValue);
      },
      true /*noinline*/);
}

Address irgen::emitAllocateValueInBuffer(IRGenFunction &IGF, SILType type,
                                         Address buffer) {
  // Handle FixedSize types.
  auto &IGM = IGF.IGM;
  auto storagePtrTy = IGM.getStoragePointerType(type);
  auto storageTy = IGM.getStorageType(type);
  auto &Builder = IGF.Builder;
  if (auto *fixedTI = dyn_cast<FixedTypeInfo>(&IGF.getTypeInfo(type))) {
    auto packing = fixedTI->getFixedPacking(IGM);

    // Inline representation.
    if (packing == FixedPacking::OffsetZero) {
      return Address(Builder.CreateBitCast(buffer.getAddress(), storagePtrTy),
                     storageTy, buffer.getAlignment());
    }

    // Outline representation.
    assert(packing == FixedPacking::Allocate && "Expect non dynamic packing");
    auto size = fixedTI->getStaticSize(IGM);
    auto alignMask = fixedTI->getStaticAlignmentMask(IGM);
    auto valueAddr =
        IGF.emitAllocRawCall(size, alignMask, "outline.ValueBuffer");
    Builder.CreateStore(valueAddr,
                        Address(Builder.CreateBitCast(buffer.getAddress(),
                                                      IGF.IGM.Int8PtrPtrTy),
                                IGM.Int8PtrTy, buffer.getAlignment()));
    return Address(Builder.CreateBitCast(valueAddr, storagePtrTy), storageTy,
                   buffer.getAlignment());
  }

  // Dynamic packing.

  /// Call a function to handle the non-fixed case.
  auto *allocateFun = getAllocateValueBufferFunction(IGF.IGM);
  auto *fnType = cast<llvm::Function>(allocateFun)->getFunctionType();
  auto *metadata = IGF.emitTypeMetadataRefForLayout(type);
  auto *call = Builder.CreateCall(
      fnType, allocateFun,
      {metadata, Builder.CreateBitCast(buffer.getAddress(), IGM.OpaquePtrTy)});
  call->setCallingConv(IGF.IGM.DefaultCC);
  call->setDoesNotThrow();

  auto addressOfValue = Builder.CreateBitCast(call, storagePtrTy);
  return Address(addressOfValue, storageTy, Alignment(1));
}

static llvm::Constant *getProjectValueInBufferFunction(IRGenModule &IGM) {

  llvm::Type *argTys[] = {IGM.TypeMetadataPtrTy, IGM.OpaquePtrTy};

  llvm::SmallString<40> fnName("__swift_project_value_buffer");

  return IGM.getOrCreateHelperFunction(
      fnName, IGM.OpaquePtrTy, argTys,
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        auto *metadata = &*(it++);
        auto buffer = Address(&*(it++), IGM.OpaqueTy, Alignment(1));
        auto &Builder = IGF.Builder;

        // Dynamically check whether this type is inline or needs an allocation.
        llvm::Value *isInline, *flags;
        std::tie(isInline, flags) = emitLoadOfIsInline(IGF, metadata);

        auto *outlineBB = IGF.createBasicBlock("outline.projectValueInBuffer");
        auto *doneBB = IGF.createBasicBlock("done");
        llvm::Value *addressInline, *addressOutline;
        auto *origBB = Builder.GetInsertBlock();
        addressInline = buffer.getAddress();

        Builder.CreateCondBr(isInline, doneBB, outlineBB);

        Builder.emitBlock(outlineBB);
        {
          addressOutline = Builder.CreateLoad(
              Address(Builder.CreateBitCast(buffer.getAddress(),
                                            IGM.OpaquePtrTy->getPointerTo()),
                      IGM.OpaquePtrTy, Alignment(1)));
          Builder.CreateBr(doneBB);
        }

        Builder.emitBlock(doneBB);
        auto *addressOfValue = Builder.CreatePHI(IGM.OpaquePtrTy, 2);
        addressOfValue->addIncoming(addressInline, origBB);
        addressOfValue->addIncoming(addressOutline, outlineBB);

        Builder.CreateRet(addressOfValue);
      },
      true /*noinline*/);
}

Address irgen::emitProjectValueInBuffer(IRGenFunction &IGF, SILType type,
                                        Address buffer) {
  // Handle FixedSize types.
  auto &IGM = IGF.IGM;
  auto storagePtrTy = IGM.getStoragePointerType(type);
  auto storageTy = IGM.getStorageType(type);
  auto &Builder = IGF.Builder;
  if (auto *fixedTI = dyn_cast<FixedTypeInfo>(&IGF.getTypeInfo(type))) {
    auto packing = fixedTI->getFixedPacking(IGM);

    // Inline representation.
    if (packing == FixedPacking::OffsetZero) {
      return Address(Builder.CreateBitCast(buffer.getAddress(), storagePtrTy),
                     storageTy, buffer.getAlignment());
    }

    // Outline representation.
    assert(packing == FixedPacking::Allocate && "Expect non dynamic packing");
    auto valueAddr = Builder.CreateLoad(
        Address(Builder.CreateBitCast(buffer.getAddress(),
                                      storagePtrTy->getPointerTo()),
                storagePtrTy, buffer.getAlignment()));
    return Address(Builder.CreateBitCast(valueAddr, storagePtrTy), storageTy,
                   buffer.getAlignment());
  }

  // Dynamic packing.
  auto *projectFun = getProjectValueInBufferFunction(IGF.IGM);
  auto *fnType = cast<llvm::Function>(projectFun)->getFunctionType();
  auto *metadata = IGF.emitTypeMetadataRefForLayout(type);
  auto *call = Builder.CreateCall(
      fnType, projectFun,
      {metadata, Builder.CreateBitCast(buffer.getAddress(), IGM.OpaquePtrTy)});
  call->setCallingConv(IGF.IGM.DefaultCC);
  call->setDoesNotThrow();

  auto addressOfValue = Builder.CreateBitCast(call, storagePtrTy);
  return Address(addressOfValue, storageTy, Alignment(1));
}

llvm::Value *
irgen::emitGetEnumTagSinglePayloadGenericCall(IRGenFunction &IGF,
                                              SILType payloadType,
                                              const TypeInfo &payloadTI,
                                              llvm::Value *numExtraCases,
                                              Address address,
                                         GetExtraInhabitantTagEmitter emitter) {
  auto getExtraInhabitantTagFn =
    getOrCreateGetExtraInhabitantTagFunction(IGF.IGM, payloadType,
                                             payloadTI, emitter);
  // Sign the getExtraInhabitantTag function with the C function pointer schema.
  if (auto schema = IGF.IGM.getOptions().PointerAuth.FunctionPointers) {
    if (schema.hasOtherDiscrimination())
      schema = IGF.IGM.getOptions().PointerAuth.GetExtraInhabitantTagFunction;
    getExtraInhabitantTagFn = IGF.IGM.getConstantSignedPointer(
        getExtraInhabitantTagFn, schema, PointerAuthEntity(), nullptr);
  }

  // We assume this is never a reabstracted type.
  auto type = payloadType.getASTType();
  assert(type->isLegalFormalType());
  auto metadata = IGF.emitTypeMetadataRef(type);

  auto ptr = IGF.Builder.CreateBitCast(address.getAddress(),
                                       IGF.IGM.OpaquePtrTy);

  auto getEnumTagGenericFn =
      IGF.IGM.getGetEnumTagSinglePayloadGenericFunctionPointer();
  auto call = IGF.Builder.CreateCall(getEnumTagGenericFn,
                                     {ptr,
                                      numExtraCases,
                                      metadata,
                                      getExtraInhabitantTagFn});
  call->setCallingConv(IGF.IGM.SwiftCC);
  return call;
}

llvm::Constant *
irgen::getOrCreateGetExtraInhabitantTagFunction(IRGenModule &IGM,
                                                SILType objectType,
                                                const TypeInfo &objectTI,
                                         GetExtraInhabitantTagEmitter emitter) {
  // We assume this is never a reabstracted type.
  CanType type = objectType.getASTType();
  assert(type->isLegalFormalType());

  auto fnTy = llvm::FunctionType::get(IGM.Int32Ty,
                                      {IGM.OpaquePtrTy,
                                       IGM.Int32Ty,
                                       IGM.TypeMetadataPtrTy},
                                      false);

  // TODO: use a meaningful mangled name and internal/shared linkage.
  auto fn = llvm::Function::Create(fnTy, llvm::Function::PrivateLinkage,
                                   "__swift_get_extra_inhabitant_index",
                                   &IGM.Module);
  fn->setAttributes(IGM.constructInitialAttributes());
  fn->setCallingConv(IGM.SwiftCC);
  IRGenFunction IGF(IGM, fn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, fn);
  auto parameters = IGF.collectParameters();
  auto ptr = parameters.claimNext();
  auto xiCount = parameters.claimNext();
  auto metadata = parameters.claimNext();

  // Bind the metadata to make any archetypes available.
  IGF.bindLocalTypeDataFromTypeMetadata(type, IsExact, metadata,
                                        MetadataState::Complete);

  // Form a well-typed address from the opaque pointer.
  ptr = IGF.Builder.CreateBitCast(ptr,
                                  objectTI.getStorageType()->getPointerTo());
  Address addr = objectTI.getAddressForPointer(ptr);

  auto tag = emitter(IGF, addr, xiCount);
  IGF.Builder.CreateRet(tag);

  return fn;
}

void
irgen::emitStoreEnumTagSinglePayloadGenericCall(IRGenFunction &IGF,
                                                SILType payloadType,
                                                const TypeInfo &payloadTI,
                                                llvm::Value *whichCase,
                                                llvm::Value *numExtraCases,
                                                Address address,
                                       StoreExtraInhabitantTagEmitter emitter) {
  auto storeExtraInhabitantTagFn =
    getOrCreateStoreExtraInhabitantTagFunction(IGF.IGM, payloadType,
                                               payloadTI, emitter);

  // Sign the getExtraInhabitantTag function with the C function pointer schema.
  if (auto schema = IGF.IGM.getOptions().PointerAuth.FunctionPointers) {
    if (schema.hasOtherDiscrimination())
      schema = IGF.IGM.getOptions().PointerAuth.StoreExtraInhabitantTagFunction;
    storeExtraInhabitantTagFn = IGF.IGM.getConstantSignedPointer(
        storeExtraInhabitantTagFn, schema, PointerAuthEntity(), nullptr);
  }

  // We assume this is never a reabstracted type.
  auto type = payloadType.getASTType();
  assert(type->isLegalFormalType());
  auto metadata = IGF.emitTypeMetadataRef(type);

  auto ptr = IGF.Builder.CreateBitCast(address.getAddress(),
                                       IGF.IGM.OpaquePtrTy);

  auto storeEnumTagGenericFn =
      IGF.IGM.getStoreEnumTagSinglePayloadGenericFunctionPointer();
  auto call = IGF.Builder.CreateCall(storeEnumTagGenericFn,
                                     {ptr,
                                      whichCase,
                                      numExtraCases,
                                      metadata,
                                      storeExtraInhabitantTagFn});
  call->setCallingConv(IGF.IGM.SwiftCC);
}

llvm::Constant *
irgen::getOrCreateStoreExtraInhabitantTagFunction(IRGenModule &IGM,
                                                  SILType objectType,
                                                  const TypeInfo &objectTI,
                                       StoreExtraInhabitantTagEmitter emitter) {
  // We assume this is never a reabstracted type.
  CanType type = objectType.getASTType();
  assert(type->isLegalFormalType());

  auto fnTy = llvm::FunctionType::get(IGM.VoidTy,
                                      {IGM.OpaquePtrTy,
                                       IGM.Int32Ty,
                                       IGM.Int32Ty,
                                       IGM.TypeMetadataPtrTy},
                                      false);

  // TODO: use a meaningful mangled name and internal/shared linkage.
  auto fn = llvm::Function::Create(fnTy, llvm::Function::PrivateLinkage,
                                   "__swift_store_extra_inhabitant_index",
                                   &IGM.Module);
  fn->setAttributes(IGM.constructInitialAttributes());
  fn->setCallingConv(IGM.SwiftCC);
  IRGenFunction IGF(IGM, fn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, fn);
  auto parameters = IGF.collectParameters();
  auto ptr = parameters.claimNext();
  auto tag = parameters.claimNext();
  auto xiCount = parameters.claimNext();
  auto metadata = parameters.claimNext();

  // Bind the metadata to make any archetypes available.
  IGF.bindLocalTypeDataFromTypeMetadata(type, IsExact, metadata,
                                        MetadataState::Complete);

  // Form a well-typed address from the opaque pointer.
  ptr = IGF.Builder.CreateBitCast(ptr,
                                  objectTI.getStorageType()->getPointerTo());
  Address addr = objectTI.getAddressForPointer(ptr);

  emitter(IGF, addr, tag, xiCount);
  IGF.Builder.CreateRetVoid();

  return fn;
}

llvm::Value *TypeInfo::getExtraInhabitantTagDynamic(IRGenFunction &IGF,
                                                    Address address,
                                                    SILType T,
                                                    llvm::Value *knownXICount,
                                                    bool isOutlined) const {
  if (auto fixedTI = dyn_cast<FixedTypeInfo>(this)) {
    auto index = fixedTI->getExtraInhabitantIndex(IGF, address, T, isOutlined);
    // The runtime APIs expect that 0 means the payload case and 1+
    // means the extra cases, but in IRGen, getExtraInhabitantIndex
    // returns -1 for the payload case and 0+ for extra inhabitants.
    // This is an easy adjustment to make.
    auto one = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1);
    auto tag = IGF.Builder.CreateAdd(index, one);
    return tag;
  } else {
    if (!knownXICount)
      knownXICount = emitLoadOfExtraInhabitantCount(IGF, T);

    auto tag = getEnumTagSinglePayload(IGF, /*num extra cases*/ knownXICount,
                                       address, T, isOutlined);
    return tag;
  }
}

void TypeInfo::storeExtraInhabitantTagDynamic(IRGenFunction &IGF,
                                              llvm::Value *tag,
                                              Address address,
                                              SILType T,
                                              bool isOutlined) const {
  if (auto fixedTI = dyn_cast<FixedTypeInfo>(this)) {
    // The runtime APIs expect that 0 means the payload case and 1+
    // means the extra cases, but in IRGen, storeExtraInhabitant
    // expects extra inhabitants to be indexed as 0+.
    // This is an easy adjustment to make.
    auto one = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1);
    auto index = IGF.Builder.CreateSub(tag, one);
    fixedTI->storeExtraInhabitant(IGF, index, address, T, isOutlined);
  } else {
    storeEnumTagSinglePayload(IGF, tag, tag, address, T, isOutlined);
  }
}

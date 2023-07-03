//===--- GenValueWitness.cpp - IR generation for value witnesses ----------===//
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
//  This file implements IR generation for value witnesses in Swift.
//
//  Value witnesses are (predominantly) functions that implement the basic
//  operations for copying and destroying values.
//
//  In the comments throughout this file, three type names are used:
//    'B' is the type of a fixed-size buffer
//    'T' is the type which implements a protocol
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Attr.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"

#include "ConstantBuilder.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenEnum.h"
#include "GenMeta.h"
#include "GenOpaque.h"
#include "GenPointerAuth.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "MetadataLayout.h"
#include "StructLayout.h"
#include "TypeInfo.h"

#include "GenValueWitness.h"

using namespace swift;
using namespace irgen;

const char *irgen::getValueWitnessName(ValueWitness witness) {
  switch (witness) {
#define CASE(NAME) case ValueWitness::NAME: return #NAME;
  CASE(AssignWithCopy)
  CASE(AssignWithTake)
  CASE(Destroy)
  CASE(InitializeBufferWithCopyOfBuffer)
  CASE(InitializeWithCopy)
  CASE(InitializeWithTake)
  CASE(GetEnumTag)
  CASE(DestructiveProjectEnumData)
  CASE(DestructiveInjectEnumTag)
  CASE(Size)
  CASE(Flags)
  CASE(ExtraInhabitantCount)
  CASE(Stride)
  CASE(GetEnumTagSinglePayload)
  CASE(StoreEnumTagSinglePayload)
#undef CASE
  }
  llvm_unreachable("bad value witness kind");
}

namespace {
  /// An operation to be performed for various kinds of packing.
  struct DynamicPackingOperation {
    virtual ~DynamicPackingOperation() = default;

    /// Emit the operation at a concrete packing kind.
    ///
    /// Immediately after this call, there will be an unconditional
    /// branch to the continuation block.
    virtual void emitForPacking(IRGenFunction &IGF,
                                SILType T,
                                const TypeInfo &type,
                                FixedPacking packing) = 0;

    /// Given that we are currently at the beginning of the
    /// continuation block, complete the operation.
    virtual void complete(IRGenFunction &IGF) = 0;
  };

  /// A class for merging a particular kind of value across control flow.
  template <class T> class DynamicPackingPHIMapping;

  /// An implementation of DynamicPackingPHIMapping for a single LLVM value.
  template <> class DynamicPackingPHIMapping<llvm::Value*> {
    llvm::PHINode *PHI = nullptr;
  public:
    void collect(IRGenFunction &IGF, llvm::Value *value) {
      // Add the result to the phi, creating it (unparented) if necessary.
      if (!PHI) PHI = llvm::PHINode::Create(value->getType(), 2,
                                            "dynamic-packing.result");
      PHI->addIncoming(value, IGF.Builder.GetInsertBlock());
    }
    void complete(IRGenFunction &IGF) {
      assert(PHI);
      IGF.Builder.Insert(PHI);
    }
    llvm::Value *get(IRGenFunction &IGF, SILType T, const TypeInfo &type) {
      assert(PHI);
      return PHI;
    }
  };

  /// An implementation of DynamicPackingPHIMapping for Addresses.
  template <> class DynamicPackingPHIMapping<Address>
      : private DynamicPackingPHIMapping<llvm::Value*> {
    using super = DynamicPackingPHIMapping<llvm::Value *>;

  public:
    void collect(IRGenFunction &IGF, Address value) {
      super::collect(IGF, value.getAddress());
    }
    void complete(IRGenFunction &IGF) {
      super::complete(IGF);
    }
    Address get(IRGenFunction &IGF, SILType T, const TypeInfo &type) {
      return type.getAddressForPointer(super::get(IGF, T, type));
    }
  };

  /// An implementation of packing operations based around a lambda.
  template <class ResultTy, class FnTy>
  class LambdaDynamicPackingOperation : public DynamicPackingOperation {
    FnTy Fn;
    DynamicPackingPHIMapping<ResultTy> Mapping;
  public:
    explicit LambdaDynamicPackingOperation(FnTy &&fn) : Fn(fn) {}
    void emitForPacking(IRGenFunction &IGF, SILType T, const TypeInfo &type,
                        FixedPacking packing) override {
      Mapping.collect(IGF, Fn(IGF, T, type, packing));
    }

    void complete(IRGenFunction &IGF) override {
      Mapping.complete(IGF);
    }

    ResultTy get(IRGenFunction &IGF, SILType T, const TypeInfo &type) {
      return Mapping.get(IGF, T, type);
    }
  };

  /// A partial specialization for lambda-based packing operations
  /// that return 'void'.
  template <class FnTy>
  class LambdaDynamicPackingOperation<void, FnTy>
      : public DynamicPackingOperation {
    FnTy Fn;
  public:
    explicit LambdaDynamicPackingOperation(FnTy &&fn) : Fn(fn) {}
    void emitForPacking(IRGenFunction &IGF, SILType T, const TypeInfo &type,
                        FixedPacking packing) override {
      Fn(IGF, T, type, packing);
    }
    void complete(IRGenFunction &IGF) override {}
    void get(IRGenFunction &IGF, SILType T, const TypeInfo &type) {}
  };
} // end anonymous namespace

/// Dynamic check for the enabling conditions of different kinds of
/// packing into a fixed-size buffer, and perform an operation at each
/// of them.
static void emitDynamicPackingOperation(IRGenFunction &IGF,
                                        SILType T,
                                        const TypeInfo &type,
                                        DynamicPackingOperation &operation) {
  auto indirectBB = IGF.createBasicBlock("dynamic-packing.indirect");
  auto directBB = IGF.createBasicBlock("dynamic-packing.direct");
  auto contBB = IGF.createBasicBlock("dynamic-packing.cont");

  // Branch.
  auto isInline = type.isDynamicallyPackedInline(IGF, T);
  IGF.Builder.CreateCondBr(isInline, directBB, indirectBB);

  // Emit the indirect path.
  IGF.Builder.emitBlock(indirectBB); {
    ConditionalDominanceScope condition(IGF);
    operation.emitForPacking(IGF, T, type, FixedPacking::Allocate);
    IGF.Builder.CreateBr(contBB);
  }

  // Emit the direct path.
  IGF.Builder.emitBlock(directBB); {
    ConditionalDominanceScope condition(IGF);
    operation.emitForPacking(IGF, T, type, FixedPacking::OffsetZero);
    IGF.Builder.CreateBr(contBB);
  }

  // Enter the continuation block and add the PHI if required.
  IGF.Builder.emitBlock(contBB);
  operation.complete(IGF);
}

/// A helper function for creating a lambda-based DynamicPackingOperation.
template <class ResultTy, class FnTy>
LambdaDynamicPackingOperation<ResultTy, FnTy>
makeLambdaDynamicPackingOperation(FnTy &&fn) {
  return LambdaDynamicPackingOperation<ResultTy, FnTy>(std::move(fn));
}

/// Perform an operation on a type that requires dynamic packing.
template <class ResultTy, class... ArgTys, class... ParamTys>
static ResultTy emitForDynamicPacking(IRGenFunction &IGF,
                                      ResultTy (*fn)(ParamTys...),
                                      SILType T,
                                      const TypeInfo &type,
                                      ArgTys... args) {
  auto operation = makeLambdaDynamicPackingOperation<ResultTy>(
    [&](IRGenFunction &IGF, SILType T, const TypeInfo &type,
        FixedPacking packing) {
      return fn(IGF, args..., T, type, packing);
    });
  emitDynamicPackingOperation(IGF, T, type, operation);
  return operation.get(IGF, T, type);
}

/// Emit a 'projectBuffer' operation.  Always returns a T*.
static Address emitDefaultProjectBuffer(IRGenFunction &IGF, Address buffer,
                                        SILType T, const TypeInfo &type,
                                        FixedPacking packing) {
  llvm::PointerType *resultTy = type.getStorageType()->getPointerTo();
  switch (packing) {
  case FixedPacking::Allocate: {

    // Use copy-on-write existentials?
    auto &IGM = IGF.IGM;
    auto &Builder = IGF.Builder;
    Address boxAddress(
        Builder.CreateBitCast(buffer.getAddress(),
                              IGM.RefCountedPtrTy->getPointerTo()),
        IGM.RefCountedPtrTy, buffer.getAlignment());
    auto *boxStart = IGF.Builder.CreateLoad(boxAddress);
    auto *alignmentMask = type.getAlignmentMask(IGF, T);
    auto *heapHeaderSize = llvm::ConstantInt::get(
        IGM.SizeTy, IGM.RefCountedStructSize.getValue());
    auto *startOffset =
        Builder.CreateAnd(Builder.CreateAdd(heapHeaderSize, alignmentMask),
                          Builder.CreateNot(alignmentMask));
    auto *addressInBox =
        IGF.emitByteOffsetGEP(boxStart, startOffset, IGM.OpaqueTy);

    addressInBox = Builder.CreateBitCast(addressInBox, resultTy);
    return type.getAddressForPointer(addressInBox);
  }

  case FixedPacking::OffsetZero: {
    return IGF.Builder.CreateElementBitCast(buffer, type.getStorageType(),
                                            "object");
  }

  case FixedPacking::Dynamic:
    return emitForDynamicPacking(IGF, &emitDefaultProjectBuffer,
                                 T, type, buffer);

  }
  llvm_unreachable("bad packing!");

}

/// Emit an 'allocateBuffer' operation.  Always returns a T*.
static Address emitDefaultAllocateBuffer(IRGenFunction &IGF, Address buffer,
                                         SILType T, const TypeInfo &type,
                                         FixedPacking packing) {
  switch (packing) {
  case FixedPacking::Allocate: {
    llvm::Value *box, *address;
    auto *metadata = IGF.emitTypeMetadataRefForLayout(T);
    IGF.emitAllocBoxCall(metadata, box, address);
    IGF.Builder.CreateStore(
        box, Address(IGF.Builder.CreateBitCast(buffer.getAddress(),
                                               box->getType()->getPointerTo()),
                     IGF.IGM.RefCountedPtrTy, buffer.getAlignment()));

    llvm::PointerType *resultTy = type.getStorageType()->getPointerTo();
    address = IGF.Builder.CreateBitCast(address, resultTy);
    return type.getAddressForPointer(address);
  }

  case FixedPacking::OffsetZero:
    return emitDefaultProjectBuffer(IGF, buffer, T, type, packing);

  case FixedPacking::Dynamic:
    return emitForDynamicPacking(IGF, &emitDefaultAllocateBuffer,
                                 T, type, buffer);
  }
  llvm_unreachable("bad packing!");
}

/// Emit an 'initializeBufferWithCopyOfBuffer' operation.
/// Returns the address of the destination object.
static Address emitDefaultInitializeBufferWithCopyOfBuffer(
    IRGenFunction &IGF, Address destBuffer, Address srcBuffer, SILType T,
    const TypeInfo &type, FixedPacking packing) {
  // Special-case dynamic packing in order to thread the jumps.
  if (packing == FixedPacking::Dynamic)
    return emitForDynamicPacking(IGF,
                                 &emitDefaultInitializeBufferWithCopyOfBuffer,
                                 T, type, destBuffer, srcBuffer);

  if (packing == FixedPacking::OffsetZero) {
    Address destObject =
        emitDefaultAllocateBuffer(IGF, destBuffer, T, type, packing);
    Address srcObject =
        emitDefaultProjectBuffer(IGF, srcBuffer, T, type, packing);
    type.initializeWithCopy(IGF, destObject, srcObject, T, true);
    return destObject;
  } else {
    assert(packing == FixedPacking::Allocate);
    auto *destReferenceAddr = IGF.Builder.CreateBitCast(
        destBuffer.getAddress(), IGF.IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReferenceAddr = IGF.Builder.CreateBitCast(
        srcBuffer.getAddress(), IGF.IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReference = IGF.Builder.CreateLoad(Address(
        srcReferenceAddr, IGF.IGM.RefCountedPtrTy, srcBuffer.getAlignment()));
    IGF.emitNativeStrongRetain(srcReference, IGF.getDefaultAtomicity());
    IGF.Builder.CreateStore(srcReference,
                            Address(destReferenceAddr, IGF.IGM.RefCountedPtrTy,
                                    destBuffer.getAlignment()));
    return emitDefaultProjectBuffer(IGF, destBuffer, T, type, packing);
  }
}

// Metaprogram some of the common boilerplate here:
//   - the default implementation in TypeInfo
//   - the value-witness emitter which tries to avoid some dynamic
//     dispatch and the recomputation of the fixed packing

#define DEFINE_BINARY_BUFFER_OP(LOWER, TITLE)                             \
Address TypeInfo::LOWER(IRGenFunction &IGF, Address dest, Address src,    \
                        SILType T) const {                                \
  return emitDefault##TITLE(IGF, dest, src, T, *this,                     \
                            getFixedPacking(IGF.IGM));                    \
}                                                                         \
static Address emit##TITLE(IRGenFunction &IGF, Address dest, Address src, \
                           SILType T, const TypeInfo &type,               \
                           FixedPacking packing) {                        \
  if (packing == FixedPacking::Dynamic)                                   \
    return type.LOWER(IGF, dest, src, T);                                 \
  return emitDefault##TITLE(IGF, dest, src, T, type, packing);            \
}
DEFINE_BINARY_BUFFER_OP(initializeBufferWithCopyOfBuffer,
                        InitializeBufferWithCopyOfBuffer)
#undef DEFINE_BINARY_BUFFER_OP


static llvm::Value *getArg(llvm::Function::arg_iterator &it,
                           StringRef name) {
  llvm::Value *arg = &*(it++);
  arg->setName(name);
  return arg;
}

/// Get the next argument as a pointer to the given storage type.
static Address getArgAs(IRGenFunction &IGF,
                        llvm::Function::arg_iterator &it,
                        const TypeInfo &type,
                        StringRef name) {
  llvm::Value *arg = getArg(it, name);
  llvm::Value *result =
    IGF.Builder.CreateBitCast(arg, type.getStorageType()->getPointerTo());
  return type.getAddressForPointer(result);
}

/// Get the next argument as a pointer to the given storage type.
static Address getArgAsBuffer(IRGenFunction &IGF,
                              llvm::Function::arg_iterator &it,
                              StringRef name) {
  llvm::Value *arg = getArg(it, name);
  return Address(arg, IGF.IGM.getFixedBufferTy(),
                 getFixedBufferAlignment(IGF.IGM));
}

/// Don't add new callers of this, it doesn't make any sense.
static CanType getFormalTypeInPrimaryContext(CanType abstractType) {
  auto *nominal = abstractType.getAnyNominal();
  if (abstractType->isEqual(nominal->getDeclaredType())) {
    return nominal->mapTypeIntoContext(nominal->getDeclaredInterfaceType())
      ->getCanonicalType();
  }

  assert(!abstractType->hasUnboundGenericType());
  return abstractType;
}

SILType irgen::getLoweredTypeInPrimaryContext(IRGenModule &IGM,
                                              NominalTypeDecl *type) {
  CanType concreteFormalType = type->mapTypeIntoContext(
      type->getDeclaredInterfaceType())->getCanonicalType();
  return IGM.getLoweredType(concreteFormalType);
}

void irgen::getArgAsLocalSelfTypeMetadata(IRGenFunction &IGF, llvm::Value *arg,
                                          CanType abstractType) {
  assert(arg->getType() == IGF.IGM.TypeMetadataPtrTy &&
         "Self argument is not a type?!");

  auto formalType = getFormalTypeInPrimaryContext(abstractType);
  IGF.bindLocalTypeDataFromTypeMetadata(formalType, IsExact, arg,
                                        MetadataState::Complete);
}

/// Get the next argument and use it as the 'self' type metadata.
static void getArgAsLocalSelfTypeMetadata(IRGenFunction &IGF,
                                          llvm::Function::arg_iterator &it,
                                          CanType abstractType) {
  llvm::Value *arg = &*it++;
  getArgAsLocalSelfTypeMetadata(IGF, arg, abstractType);
}

static const TypeLayoutEntry *
conditionallyGetTypeLayoutEntry(IRGenModule &IGM, SILType concreteType) {
  if (!IGM.getOptions().UseTypeLayoutValueHandling)
    return nullptr;

  auto &typeLayoutEntry = IGM.getTypeLayoutEntry(
    concreteType, IGM.getOptions().ForceStructTypeLayouts);

  // Don't use type layout based generation for layouts that contain a resilient
  // field but no archetype. We don't expect a speedup by using type layout
  // based ir generation.
  if ((typeLayoutEntry.containsResilientField() &&
       !typeLayoutEntry.containsArchetypeField()) ||
      typeLayoutEntry.containsDependentResilientField())
    return nullptr;

  return &typeLayoutEntry;
}

static const EnumTypeLayoutEntry *
conditionallyGetEnumTypeLayoutEntry(IRGenModule &IGM, SILType concreteType) {
  auto *entry = conditionallyGetTypeLayoutEntry(IGM, concreteType);
  if (!entry)
    return nullptr;

  return entry->getAsEnum();
}

/// Build a specific value-witness function.
static void buildValueWitnessFunction(IRGenModule &IGM,
                                      llvm::Function *fn,
                                      ValueWitness index,
                                      FixedPacking packing,
                                      CanType abstractType,
                                      SILType concreteType,
                                      const TypeInfo &type) {
  assert(isValueWitnessFunction(index));

  IRGenFunction IGF(IGM, fn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, fn);

  auto argv = fn->arg_begin();
  switch (index) {
  case ValueWitness::AssignWithCopy: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->assignWithCopy(IGF, dest, src);
    } else {
      type.assignWithCopy(IGF, dest, src, concreteType, true);
    }
    dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.OpaqueTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::AssignWithTake: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->assignWithTake(IGF, dest, src);
    } else {
      type.assignWithTake(IGF, dest, src, concreteType, true);
    }
    dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.OpaqueTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::Destroy: {
    Address object = getArgAs(IGF, argv, type, "object");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->destroy(IGF, object);
    } else {
      type.destroy(IGF, object, concreteType, true);
    }
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::InitializeBufferWithCopyOfBuffer: {
    Address dest = getArgAsBuffer(IGF, argv, "dest");
    Address src = getArgAsBuffer(IGF, argv, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    llvm::Value *objectPtr = nullptr;
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      objectPtr = typeLayoutEntry->initBufferWithCopyOfBuffer(IGF, dest, src);
    } else {
      Address result = emitInitializeBufferWithCopyOfBuffer(
          IGF, dest, src, concreteType, type, packing);
      result = IGF.Builder.CreateElementBitCast(result, IGF.IGM.OpaqueTy);
      objectPtr = result.getAddress();
    }
    IGF.Builder.CreateRet(objectPtr);
    return;
  }

  case ValueWitness::InitializeWithCopy: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->initWithCopy(IGF, dest, src);
    } else {
      type.initializeWithCopy(IGF, dest, src, concreteType, true);
    }
    dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.OpaqueTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::InitializeWithTake: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->initWithTake(IGF, dest, src);
    } else {
      type.initializeWithTake(IGF, dest, src, concreteType, true);
    }
    dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.OpaqueTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::GetEnumTag: {
    auto &strategy = getEnumImplStrategy(IGM, concreteType);

    llvm::Value *value = getArg(argv, "value");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    auto enumTy = type.getStorageType()->getPointerTo();
    value = IGF.Builder.CreateBitCast(value, enumTy);
    auto enumAddr = type.getAddressForPointer(value);

    llvm::Value *result;
    if (auto *enumTypeLayoutEntry =
            conditionallyGetEnumTypeLayoutEntry(IGM, concreteType)) {
      result = enumTypeLayoutEntry->getEnumTag(IGF, enumAddr);
    } else {
      result = strategy.emitGetEnumTag(IGF, concreteType, enumAddr);
    }
    IGF.Builder.CreateRet(result);
    return;
  }

  case ValueWitness::DestructiveProjectEnumData: {
    auto &strategy = getEnumImplStrategy(IGM, concreteType);

    llvm::Value *value = getArg(argv, "value");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    if (!strategy.getElementsWithPayload().empty()) {
      if (auto *enumTypeLayoutEntry =
              conditionallyGetEnumTypeLayoutEntry(IGM, concreteType)) {
        enumTypeLayoutEntry->destructiveProjectEnumData(
            IGF, Address(value, IGM.OpaqueTy, type.getBestKnownAlignment()));
      } else {
        strategy.destructiveProjectDataForLoad(
            IGF, concreteType,
            Address(value, IGM.OpaqueTy, type.getBestKnownAlignment()));
      }
    }

    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::DestructiveInjectEnumTag: {
    auto &strategy = getEnumImplStrategy(IGM, concreteType);

    llvm::Value *value = getArg(argv, "value");

    auto enumTy = type.getStorageType()->getPointerTo();
    value = IGF.Builder.CreateBitCast(value, enumTy);

    llvm::Value *tag = getArg(argv, "tag");

    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *enumTypeLayoutEntry =
            conditionallyGetEnumTypeLayoutEntry(IGM, concreteType)) {
      enumTypeLayoutEntry->destructiveInjectEnumTag(
          IGF, tag,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()));
    } else {
      strategy.emitStoreTag(
          IGF, concreteType,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()),
          tag);
    }

    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::GetEnumTagSinglePayload: {
    llvm::Value *value = getArg(argv, "value");
    auto enumTy = type.getStorageType()->getPointerTo();
    value = IGF.Builder.CreateBitCast(value, enumTy);

    llvm::Value *numEmptyCases = getArg(argv, "numEmptyCases");

    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      auto *idx = typeLayoutEntry->getEnumTagSinglePayload(
          IGF, numEmptyCases,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()));
      IGF.Builder.CreateRet(idx);
    } else {
      llvm::Value *idx = type.getEnumTagSinglePayload(
          IGF, numEmptyCases,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()),
          concreteType, true);
      IGF.Builder.CreateRet(idx);
    }

    return;
  }

  case ValueWitness::StoreEnumTagSinglePayload: {
    llvm::Value *value = getArg(argv, "value");
    auto enumTy = type.getStorageType()->getPointerTo();
    value = IGF.Builder.CreateBitCast(value, enumTy);

    llvm::Value *whichCase = getArg(argv, "whichCase");
    llvm::Value *numEmptyCases = getArg(argv, "numEmptyCases");

    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    if (auto *typeLayoutEntry =
            conditionallyGetTypeLayoutEntry(IGM, concreteType)) {
      typeLayoutEntry->storeEnumTagSinglePayload(
          IGF, whichCase, numEmptyCases,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()));
    } else {
      type.storeEnumTagSinglePayload(
          IGF, whichCase, numEmptyCases,
          Address(value, type.getStorageType(), type.getBestKnownAlignment()),
          concreteType, true);
    }
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::Size:
  case ValueWitness::Flags:
  case ValueWitness::ExtraInhabitantCount:
  case ValueWitness::Stride:
    llvm_unreachable("these value witnesses aren't functions");
  }
  llvm_unreachable("bad value witness kind!");
}

/// Return a function which takes two pointer arguments and returns
/// void immediately.
static llvm::Constant *getNoOpVoidFunction(IRGenModule &IGM) {
  llvm::Type *argTys[] = { IGM.Int8PtrTy, IGM.TypeMetadataPtrTy };
  return IGM.getOrCreateHelperFunction("__swift_noop_void_return",
                                       IGM.VoidTy, argTys,
                                       [&](IRGenFunction &IGF) {
    IGF.Builder.CreateRetVoid();
  });
}

/// Return a function that traps because of an attempt to copy a noncopyable
/// type.
static llvm::Constant *getNoncopyableTrapFunction(IRGenModule &IGM) {
  return IGM.getOrCreateHelperFunction("__swift_cannot_copy_noncopyable_type",
                                       IGM.VoidTy, {},
                                       [&](IRGenFunction &IGF) {
    IGF.Builder.CreateNonMergeableTrap(IGM, "attempt to copy a value of a type that cannot be copied");
    IGF.Builder.CreateUnreachable();
  });
}

/// Return a function which takes three pointer arguments and does a
/// retaining assignWithCopy on the first two: it loads a pointer from
/// the second, retains it, loads a pointer from the first, stores the
/// new pointer in the first, and releases the old pointer.
static llvm::Constant *getAssignWithCopyStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  return IGM.getOrCreateHelperFunction("__swift_assignWithCopy_strong",
                                       ptrPtrTy, argTys,
                                       [&](IRGenFunction &IGF) {
    auto it = IGF.CurFn->arg_begin();
    Address dest(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());
    Address src(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    IGF.emitNativeStrongRetain(newValue, IGF.getDefaultAtomicity());
    llvm::Value *oldValue = IGF.Builder.CreateLoad(dest, "old");
    IGF.Builder.CreateStore(newValue, dest);
    IGF.emitNativeStrongRelease(oldValue, IGF.getDefaultAtomicity());

    IGF.Builder.CreateRet(dest.getAddress());
  });
}

/// Return a function which takes three pointer arguments and does a
/// retaining assignWithTake on the first two: it loads a pointer from
/// the second, retains it, loads a pointer from the first, stores the
/// new pointer in the first, and releases the old pointer.
static llvm::Constant *getAssignWithTakeStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  return IGM.getOrCreateHelperFunction("__swift_assignWithTake_strong",
                                       ptrPtrTy, argTys,
                                       [&](IRGenFunction &IGF) {
    auto it = IGF.CurFn->arg_begin();
    Address dest(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());
    Address src(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    llvm::Value *oldValue = IGF.Builder.CreateLoad(dest, "old");
    IGF.Builder.CreateStore(newValue, dest);
    IGF.emitNativeStrongRelease(oldValue, IGF.getDefaultAtomicity());

    IGF.Builder.CreateRet(dest.getAddress());
  });
}

/// Return a function which takes three pointer arguments and does a
/// retaining initWithCopy on the first two: it loads a pointer from
/// the second, retains it, and stores that in the first.
static llvm::Constant *getInitWithCopyStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  return IGM.getOrCreateHelperFunction("__swift_initWithCopy_strong",
                                       ptrPtrTy, argTys,
                                       [&](IRGenFunction &IGF) {
    auto it = IGF.CurFn->arg_begin();
    Address dest(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());
    Address src(&*(it++), IGM.RefCountedPtrTy, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    IGF.emitNativeStrongRetain(newValue, IGF.getDefaultAtomicity());
    IGF.Builder.CreateStore(newValue, dest);

    IGF.Builder.CreateRet(dest.getAddress());
  });
}

/// Return a function which takes two pointer arguments, loads a
/// pointer from the first, and calls swift_release on it immediately.
static llvm::Constant *getDestroyStrongFunction(IRGenModule &IGM) {
  llvm::Type *argTys[] = { IGM.Int8PtrPtrTy, IGM.WitnessTablePtrTy };
  return IGM.getOrCreateHelperFunction(
      "__swift_destroy_strong", IGM.VoidTy, argTys, [&](IRGenFunction &IGF) {
        Address arg(&*IGF.CurFn->arg_begin(), IGM.Int8PtrTy,
                    IGM.getPointerAlignment());
        IGF.emitNativeStrongRelease(IGF.Builder.CreateLoad(arg),
                                    IGF.getDefaultAtomicity());
        IGF.Builder.CreateRetVoid();
      });
}

/// Return a function which takes two pointer arguments, memcpys
/// from the second to the first, and returns the first argument.
static llvm::Constant *getMemCpyFunction(IRGenModule &IGM,
                                         const TypeInfo &objectTI) {
  // If we don't have a fixed type, use the standard copy-opaque-POD
  // routine.  It's not quite clear how in practice we'll be able to
  // conclude that something is known-POD without knowing its size,
  // but it's (1) conceivable and (2) needed as a general export anyway.
  auto *fixedTI = dyn_cast<FixedTypeInfo>(&objectTI);
  if (!fixedTI) return IGM.getCopyPODFn();

  // We need to unique by both size and alignment.  Note that we're
  // assuming that it's safe to call a function that returns a pointer
  // at a site that assumes the function returns void.
  llvm::SmallString<40> name;
  {
    llvm::raw_svector_ostream nameStream(name);
    nameStream << "__swift_memcpy";
    nameStream << fixedTI->getFixedSize().getValue();
    nameStream << '_';
    nameStream << fixedTI->getFixedAlignment().getValue();
  }

  llvm::Type *argTys[] = { IGM.Int8PtrTy, IGM.Int8PtrTy, IGM.TypeMetadataPtrTy };
  return IGM.getOrCreateHelperFunction(name, IGM.Int8PtrTy, argTys,
                                       [&](IRGenFunction &IGF) {
    auto it = IGF.CurFn->arg_begin();
    Address dest(&*it++, IGM.Int8Ty, fixedTI->getFixedAlignment());
    Address src(&*it++, IGM.Int8Ty, fixedTI->getFixedAlignment());
    IGF.emitMemCpy(dest, src, fixedTI->getFixedSize());
    IGF.Builder.CreateRet(dest.getAddress());
  });
}

namespace {
struct BoundGenericTypeCharacteristics {
  SILType concreteType;
  const TypeInfo *TI;
  FixedPacking packing;
};

ValueWitnessFlags getValueWitnessFlags(const TypeInfo *TI, SILType concreteType,
                                       FixedPacking packing) {
  ValueWitnessFlags flags;

  // If we locally know that the type has fixed layout, we can emit
  // meaningful flags for it.
  if (auto *fixedTI = dyn_cast<FixedTypeInfo>(TI)) {
    assert(packing == FixedPacking::OffsetZero ||
           packing == FixedPacking::Allocate);
    bool isInline = packing == FixedPacking::OffsetZero;
    bool isBitwiseTakable =
        fixedTI->isBitwiseTakable(ResilienceExpansion::Maximal);
    assert(isBitwiseTakable || !isInline);
    flags = flags.withAlignment(fixedTI->getFixedAlignment().getValue())
                .withPOD(fixedTI->isTriviallyDestroyable(ResilienceExpansion::Maximal))
                .withCopyable(fixedTI->isCopyable(ResilienceExpansion::Maximal))
                .withInlineStorage(isInline)
                .withBitwiseTakable(isBitwiseTakable);
  } else {
    flags = flags.withIncomplete(true);
  }

  if (concreteType.getEnumOrBoundGenericEnum())
    flags = flags.withEnumWitnesses(true);

  return flags;
}

unsigned getExtraInhabitantCount(const TypeInfo *TI, IRGenModule &IGM) {
  unsigned value = 0;
  if (auto *fixedTI = dyn_cast<FixedTypeInfo>(TI)) {
    value = fixedTI->getFixedExtraInhabitantCount(IGM);
  }
  return value;
}

void addSize(ConstantStructBuilder &B, const TypeInfo *TI, IRGenModule &IGM) {
  if (auto staticSize = TI->getStaticSize(IGM))
    return B.add(staticSize);
  // Just fill in 0 here if the type can't be statically laid out.
  return B.addSize(Size(0));
}

void addStride(ConstantStructBuilder &B, const TypeInfo *TI, IRGenModule &IGM) {
  if (auto value = TI->getStaticStride(IGM))
    return B.add(value);

  // Just fill in null here if the type can't be statically laid out.
  return B.addSize(Size(0));
}
} // end anonymous namespace

static bool isRuntimeInstatiatedLayoutString(IRGenModule &IGM,
                                       const TypeLayoutEntry *typeLayoutEntry) {
  if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
      IGM.Context.LangOpts.hasFeature(
          Feature::LayoutStringValueWitnessesInstantiation) &&
      IGM.getOptions().EnableLayoutStringValueWitnessesInstantiation) {
    return (
        (typeLayoutEntry->isAlignedGroup() || typeLayoutEntry->getAsEnum()) &&
        !typeLayoutEntry->isFixedSize(IGM));
  }

  return false;
}

static llvm::Constant *getEnumTagFunction(IRGenModule &IGM,
                                     const EnumTypeLayoutEntry *typeLayoutEntry,
                                          GenericSignature genericSig) {
  if (!typeLayoutEntry->layoutString(IGM, genericSig) &&
      !isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
    return nullptr;
  } else if (typeLayoutEntry->isSingleton()) {
    return IGM.getSingletonEnumGetEnumTagFn();
  } else if (!typeLayoutEntry->isFixedSize(IGM)) {
    if (typeLayoutEntry->isMultiPayloadEnum()) {
      return IGM.getMultiPayloadEnumGenericGetEnumTagFn();
    } else {
      return IGM.getSinglePayloadEnumGenericGetEnumTagFn();
    }
  } else if (typeLayoutEntry->isMultiPayloadEnum()) {
    return IGM.getEnumFnGetEnumTagFn();
  } else {
    auto &payloadTI = **(typeLayoutEntry->cases[0]->getFixedTypeInfo());
    auto mask = payloadTI.getFixedExtraInhabitantMask(IGM);
    auto tzCount = mask.countTrailingZeros();
    auto shiftedMask = mask.lshr(tzCount);
    auto toCount = shiftedMask.countTrailingOnes();
    if (payloadTI.mayHaveExtraInhabitants(IGM) &&
        (mask.countPopulation() > 64 ||
         toCount != mask.countPopulation() ||
         (tzCount % toCount != 0))) {
      return IGM.getEnumFnGetEnumTagFn();
    } else {
      return IGM.getEnumSimpleGetEnumTagFn();
    }
  }
}

static llvm::Constant *
getDestructiveInjectEnumTagFunction(IRGenModule &IGM,
                                    const EnumTypeLayoutEntry *typeLayoutEntry,
                                    GenericSignature genericSig) {
  if ((!typeLayoutEntry->layoutString(IGM, genericSig) &&
       !isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry))) {
    return nullptr;
  } else if (typeLayoutEntry->isSingleton()) {
    return nullptr;
  } else if (!typeLayoutEntry->isFixedSize(IGM)) {
    if (typeLayoutEntry->isMultiPayloadEnum()) {
      return IGM.getMultiPayloadEnumGenericDestructiveInjectEnumTagFn();
    } else {
      return IGM.getSinglePayloadEnumGenericDestructiveInjectEnumTagFn();
    }
  } else if (typeLayoutEntry->isMultiPayloadEnum()) {
    return nullptr;
  } else {
    auto &payloadTI = **(typeLayoutEntry->cases[0]->getFixedTypeInfo());
    auto mask = payloadTI.getFixedExtraInhabitantMask(IGM);
    auto tzCount = mask.countTrailingZeros();
    auto shiftedMask = mask.lshr(tzCount);
    auto toCount = shiftedMask.countTrailingOnes();
    if (payloadTI.mayHaveExtraInhabitants(IGM) &&
        (mask.countPopulation() > 64 || toCount != mask.countPopulation() ||
         (tzCount % toCount != 0))) {
      return nullptr;
    } else {
      return nullptr;
    }
  }
}

static bool
valueWitnessRequiresCopyability(ValueWitness index) {
  switch (index) {
  case ValueWitness::InitializeBufferWithCopyOfBuffer:
  case ValueWitness::InitializeWithCopy:
  case ValueWitness::AssignWithCopy:
    return true;
  
  case ValueWitness::Destroy:
  case ValueWitness::InitializeWithTake:
  case ValueWitness::AssignWithTake:
  case ValueWitness::GetEnumTagSinglePayload:
  case ValueWitness::StoreEnumTagSinglePayload:
  case ValueWitness::Size:
  case ValueWitness::Stride:
  case ValueWitness::Flags:
  case ValueWitness::ExtraInhabitantCount:
  case ValueWitness::GetEnumTag:
  case ValueWitness::DestructiveProjectEnumData:
  case ValueWitness::DestructiveInjectEnumTag:
    return false;
  }
  llvm_unreachable("not all value witnesses covered");
}



/// Find a witness to the fact that a type is a value type.
/// Always adds an i8*.
static void
addValueWitness(IRGenModule &IGM, ConstantStructBuilder &B, ValueWitness index,
                FixedPacking packing, CanType abstractType,
                SILType concreteType, const TypeInfo &concreteTI,
                const llvm::Optional<BoundGenericTypeCharacteristics>
                    boundGenericCharacteristics = llvm::None) {
  auto addFunction = [&](llvm::Constant *fn) {
    fn = llvm::ConstantExpr::getBitCast(fn, IGM.Int8PtrTy);
    B.addSignedPointer(fn, IGM.getOptions().PointerAuth.ValueWitnesses, index);
  };

  // Copyable and noncopyable types share a value witness table layout. It
  // should normally be statically impossible to generate a call to a copy
  // value witness, but if somebody manages to dynamically get hold of metadata
  // for a noncopyable type, and tries to use it to copy values of that type,
  // we should trap to prevent the attempt.
  if (!concreteTI.isCopyable(ResilienceExpansion::Maximal)
      && valueWitnessRequiresCopyability(index)) {
    return addFunction(getNoncopyableTrapFunction(IGM));
  }

  // Try to use a standard function.
  switch (index) {
  case ValueWitness::Destroy:
    if (concreteTI.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
      return addFunction(getNoOpVoidFunction(IGM));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
      return addFunction(getDestroyStrongFunction(IGM));
    } else if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
               IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics ? boundGenericCharacteristics->concreteType : concreteType;
      auto &typeInfo = boundGenericCharacteristics ? *boundGenericCharacteristics->TI : concreteTI;
      if (auto *typeLayoutEntry =
            typeInfo.buildTypeLayoutEntry(IGM, ty, /*useStructLayouts*/true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(IGM.getGenericDestroyFn());
        }
      }
    }
    goto standard;

  case ValueWitness::InitializeBufferWithCopyOfBuffer:
    if (packing == FixedPacking::OffsetZero) {
      if (concreteTI.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
        return addFunction(getMemCpyFunction(IGM, concreteTI));
      } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
        return addFunction(getInitWithCopyStrongFunction(IGM));
      }
    }

    if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
        IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics
                    ? boundGenericCharacteristics->concreteType
                    : concreteType;
      auto &typeInfo = boundGenericCharacteristics
                           ? *boundGenericCharacteristics->TI
                           : concreteTI;
      if (auto *typeLayoutEntry = typeInfo.buildTypeLayoutEntry(
              IGM, ty, /*useStructLayouts*/ true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(
              IGM.getGenericInitializeBufferWithCopyOfBufferFn());
        }
      }
    }
    goto standard;

  case ValueWitness::InitializeWithTake:
    if (concreteTI.isBitwiseTakable(ResilienceExpansion::Maximal)) {
      return addFunction(getMemCpyFunction(IGM, concreteTI));
    } else if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
               IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics ? boundGenericCharacteristics->concreteType : concreteType;
      auto &typeInfo = boundGenericCharacteristics ? *boundGenericCharacteristics->TI : concreteTI;
      if (auto *typeLayoutEntry =
            typeInfo.buildTypeLayoutEntry(IGM, ty, /*useStructLayouts*/true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(IGM.getGenericInitWithTakeFn());
        }
      }
    }
    goto standard;

  case ValueWitness::AssignWithCopy:
    if (concreteTI.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
      return addFunction(getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
      return addFunction(getAssignWithCopyStrongFunction(IGM));
    } else if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
               IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics ? boundGenericCharacteristics->concreteType : concreteType;
      auto &typeInfo = boundGenericCharacteristics ? *boundGenericCharacteristics->TI : concreteTI;
      if (auto *typeLayoutEntry =
            typeInfo.buildTypeLayoutEntry(IGM, ty, /*useStructLayouts*/true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(IGM.getGenericAssignWithCopyFn());
        }
      }
    }
    goto standard;

  case ValueWitness::AssignWithTake:
    if (concreteTI.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
      return addFunction(getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
      return addFunction(getAssignWithTakeStrongFunction(IGM));
    } else if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
               IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics ? boundGenericCharacteristics->concreteType : concreteType;
      auto &typeInfo = boundGenericCharacteristics ? *boundGenericCharacteristics->TI : concreteTI;
      if (auto *typeLayoutEntry =
            typeInfo.buildTypeLayoutEntry(IGM, ty, /*useStructLayouts*/true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(IGM.getGenericAssignWithTakeFn());
        }
      }
    }
    goto standard;

  case ValueWitness::InitializeWithCopy:
    if (concreteTI.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
      return addFunction(getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceExpansion::Maximal)) {
      return addFunction(getInitWithCopyStrongFunction(IGM));
    } else if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
               IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics ? boundGenericCharacteristics->concreteType : concreteType;
      auto &typeInfo = boundGenericCharacteristics ? *boundGenericCharacteristics->TI : concreteTI;
      if (auto *typeLayoutEntry =
            typeInfo.buildTypeLayoutEntry(IGM, ty, /*useStructLayouts*/true)) {
        auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
        if (typeLayoutEntry->layoutString(IGM, genericSig) ||
            isRuntimeInstatiatedLayoutString(IGM, typeLayoutEntry)) {
          return addFunction(IGM.getGenericInitWithCopyFn());
        }
      }
    }
    goto standard;

  case ValueWitness::Size: {
    if (boundGenericCharacteristics)
      return addSize(B, boundGenericCharacteristics->TI, IGM);
    return addSize(B, &concreteTI, IGM);
  }

  case ValueWitness::Flags: {
    if (boundGenericCharacteristics)
      return B.addInt32(
          getValueWitnessFlags(boundGenericCharacteristics->TI,
                               boundGenericCharacteristics->concreteType,
                               boundGenericCharacteristics->packing)
              .getOpaqueValue());
    return B.addInt32(getValueWitnessFlags(&concreteTI, concreteType, packing)
                          .getOpaqueValue());
  }

  case ValueWitness::ExtraInhabitantCount: {
    if (boundGenericCharacteristics)
      return B.addInt32(
          getExtraInhabitantCount(boundGenericCharacteristics->TI, IGM));
    return B.addInt32(getExtraInhabitantCount(&concreteTI, IGM));
  }

  case ValueWitness::Stride: {
    if (boundGenericCharacteristics)
      return addStride(B, boundGenericCharacteristics->TI, IGM);
    return addStride(B, &concreteTI, IGM);
  }

  case ValueWitness::GetEnumTagSinglePayload: {
    if (boundGenericCharacteristics)
      if (auto *enumDecl = boundGenericCharacteristics->concreteType
                               .getEnumOrBoundGenericEnum())
        if (IGM.getMetadataLayout(enumDecl).hasPayloadSizeOffset())
          return B.add(llvm::ConstantExpr::getBitCast(
              IGM.getGetMultiPayloadEnumTagSinglePayloadFn(), IGM.Int8PtrTy));
    goto standard;
  }
  case ValueWitness::StoreEnumTagSinglePayload: {
    if (boundGenericCharacteristics)
      if (auto *enumDecl = boundGenericCharacteristics->concreteType
                               .getEnumOrBoundGenericEnum())
        if (IGM.getMetadataLayout(enumDecl).hasPayloadSizeOffset())
          return B.add(llvm::ConstantExpr::getBitCast(
              IGM.getStoreMultiPayloadEnumTagSinglePayloadFn(), IGM.Int8PtrTy));
    goto standard;
  }

  case ValueWitness::GetEnumTag: {
    assert(concreteType.getEnumOrBoundGenericEnum());

    if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
        IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics
                    ? boundGenericCharacteristics->concreteType
                    : concreteType;
      auto &typeInfo = boundGenericCharacteristics
                           ? *boundGenericCharacteristics->TI
                           : concreteTI;
      if (auto *typeLayoutEntry = typeInfo.buildTypeLayoutEntry(
              IGM, ty, /*useStructLayouts*/ true)) {
        if (auto *enumLayoutEntry = typeLayoutEntry->getAsEnum()) {
          auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                              ->getGenericSignature();
          if (auto *fn = getEnumTagFunction(IGM, enumLayoutEntry, genericSig)) {
            return addFunction(fn);
          }
        }
      }
    }
    goto standard;
  }
  case ValueWitness::DestructiveInjectEnumTag: {
    assert(concreteType.getEnumOrBoundGenericEnum());
    if (IGM.Context.LangOpts.hasFeature(Feature::LayoutStringValueWitnesses) &&
        IGM.getOptions().EnableLayoutStringValueWitnesses) {
      auto ty = boundGenericCharacteristics
                    ? boundGenericCharacteristics->concreteType
                    : concreteType;
      auto &typeInfo = boundGenericCharacteristics
                           ? *boundGenericCharacteristics->TI
                           : concreteTI;
      if (auto *typeLayoutEntry = typeInfo.buildTypeLayoutEntry(
              IGM, ty, /*useStructLayouts*/ true)) {
        if (auto *enumLayoutEntry = typeLayoutEntry->getAsEnum()) {
          auto genericSig = concreteType.getNominalOrBoundGenericNominal()
                                ->getGenericSignature();
          if (auto *fn = getDestructiveInjectEnumTagFunction(
                  IGM, enumLayoutEntry, genericSig)) {
            return addFunction(fn);
          }
        }
      }
    }
    goto standard;
  }
  case ValueWitness::DestructiveProjectEnumData:
    assert(concreteType.getEnumOrBoundGenericEnum());
    goto standard;
  }
  llvm_unreachable("bad value witness kind");

 standard:
  llvm::Function *fn =
    IGM.getAddrOfValueWitness(abstractType, index, ForDefinition);
  if (fn->empty())
    buildValueWitnessFunction(IGM, fn, index, packing, abstractType,
                              concreteType, concreteTI);
  addFunction(fn);
 }

static bool shouldAddEnumWitnesses(CanType abstractType) {
  // Needs to handle UnboundGenericType.
  return dyn_cast_or_null<EnumDecl>(abstractType.getAnyNominal()) != nullptr;
}

static llvm::StructType *getValueWitnessTableType(IRGenModule &IGM,
                                                  CanType abstractType) {
  return shouldAddEnumWitnesses(abstractType)
           ? IGM.getEnumValueWitnessTableTy()
           : IGM.getValueWitnessTableTy();
}

/// Collect the value witnesses for a particular type.
static void
addValueWitnesses(IRGenModule &IGM, ConstantStructBuilder &B,
                  FixedPacking packing, CanType abstractType,
                  SILType concreteType, const TypeInfo &concreteTI,
                  const llvm::Optional<BoundGenericTypeCharacteristics>
                      boundGenericCharacteristics = llvm::None) {
  for (unsigned i = 0; i != NumRequiredValueWitnesses; ++i) {
    addValueWitness(IGM, B, ValueWitness(i), packing, abstractType,
                    concreteType, concreteTI, boundGenericCharacteristics);
  }
  if (shouldAddEnumWitnesses(abstractType)) {
    for (auto i = unsigned(ValueWitness::First_EnumValueWitness);
         i <= unsigned(ValueWitness::Last_EnumValueWitness);
         ++i) {
      addValueWitness(IGM, B, ValueWitness(i), packing, abstractType,
                      concreteType, concreteTI, boundGenericCharacteristics);
    }
  }
}

/// True if a type has a generic-parameter-dependent value witness table.
/// Currently, this is true if the size and/or alignment of the type is
/// dependent on its generic parameters.
bool irgen::hasDependentValueWitnessTable(IRGenModule &IGM, NominalTypeDecl *decl) {
  auto ty = decl->mapTypeIntoContext(decl->getDeclaredInterfaceType());
  return !IGM.getTypeInfoForUnlowered(ty).isFixedSize();
}

static void addValueWitnessesForAbstractType(IRGenModule &IGM,
                                             ConstantStructBuilder &B,
                                             CanType abstractType,
                                             bool &canBeConstant) {
  llvm::Optional<BoundGenericTypeCharacteristics> boundGenericCharacteristics;
  if (auto boundGenericType = dyn_cast<BoundGenericType>(abstractType)) {
    CanType concreteFormalType = getFormalTypeInPrimaryContext(abstractType);

    auto concreteLoweredType = IGM.getLoweredType(concreteFormalType);
    const auto *boundConcreteTI = &IGM.getTypeInfo(concreteLoweredType);
    auto packing = boundConcreteTI->getFixedPacking(IGM);
    boundGenericCharacteristics = {concreteLoweredType, boundConcreteTI,
                                   packing};

    abstractType =
        boundGenericType->getDecl()->getDeclaredType()->getCanonicalType();
  }
  CanType concreteFormalType = getFormalTypeInPrimaryContext(abstractType);

  auto concreteLoweredType = IGM.getLoweredType(concreteFormalType);
  auto &concreteTI = IGM.getTypeInfo(concreteLoweredType);
  FixedPacking packing = concreteTI.getFixedPacking(IGM);

  // For now, assume that we never have any interest in dynamically
  // changing the value witnesses for something that's fixed-layout.
  canBeConstant = boundGenericCharacteristics
                      ? boundGenericCharacteristics->TI->isFixedSize()
                      : concreteTI.isFixedSize();

  addValueWitnesses(IGM, B, packing, abstractType, concreteLoweredType,
                    concreteTI, boundGenericCharacteristics);
}

static constexpr uint64_t sizeAndAlignment(Size size, Alignment alignment) {
  return ((uint64_t)size.getValue() << 32) | alignment.getValue();
}

/// Return a reference to a known value witness table from the runtime
/// that's suitable for the given type, if there is one, or return null
/// if we should emit a new one.
static ConstantReference
getAddrOfKnownValueWitnessTable(IRGenModule &IGM, CanType type,
                                bool relativeReference) {
  // Native PE binaries shouldn't reference data symbols across DLLs, so disable
  // this on Windows, unless we're forming a relative indirectable reference.
  if (IGM.useDllStorage() && !relativeReference)
    return {};
  
  if (auto nom = type->getAnyNominal()) {
    // TODO: Non-C enums have extra inhabitants and also need additional value
    // witnesses for their tag manipulation (except when they're empty, in
    // which case values never exist to witness).
    if (auto enumDecl = dyn_cast<EnumDecl>(nom))
      if (!enumDecl->isObjC() && !type->isUninhabited())
        return {};
  }
 
  auto &C = IGM.Context;

  type = getFormalTypeInPrimaryContext(type);
  
  auto &ti = IGM.getTypeInfoForUnlowered(AbstractionPattern::getOpaque(), type);

    // We only have known value witness tables for copyable types currently.
  if (!ti.isCopyable(ResilienceExpansion::Maximal)) {
    return {};
  }
  
  // We only have witnesses for fixed type info.
  auto *fixedTI = dyn_cast<FixedTypeInfo>(&ti);
  if (!fixedTI)
    return {};

  CanType witnessSurrogate;
  ReferenceCounting refCounting;

  // Empty types can use empty tuple witnesses.
  if (ti.isKnownEmpty(ResilienceExpansion::Maximal)) {
    witnessSurrogate = TupleType::getEmpty(C);
  // Handle common POD type layouts.
  } else if (fixedTI->isTriviallyDestroyable(ResilienceExpansion::Maximal)
      && fixedTI->getFixedExtraInhabitantCount(IGM) == 0) {
    // Reuse one of the integer witnesses if applicable.
    switch (sizeAndAlignment(fixedTI->getFixedSize(),
                             fixedTI->getFixedAlignment())) {
    case sizeAndAlignment(Size(0), Alignment::create<1>()):
      witnessSurrogate = TupleType::getEmpty(C);
      break;
    case sizeAndAlignment(Size(1), Alignment::create<1>()):
      witnessSurrogate = BuiltinIntegerType::get(8, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(2), Alignment::create<2>()):
      witnessSurrogate = BuiltinIntegerType::get(16, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(4), Alignment::create<4>()):
      witnessSurrogate = BuiltinIntegerType::get(32, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(8), Alignment::create<8>()):
      witnessSurrogate = BuiltinIntegerType::get(64, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(16), Alignment::create<16>()):
      witnessSurrogate = BuiltinIntegerType::get(128, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(32), Alignment::create<32>()):
      witnessSurrogate = BuiltinIntegerType::get(256, C)->getCanonicalType();
      break;
    case sizeAndAlignment(Size(64), Alignment::create<64>()):
      witnessSurrogate = BuiltinIntegerType::get(512, C)->getCanonicalType();
      break;
    }
  } else if (fixedTI->isSingleRetainablePointer(ResilienceExpansion::Maximal,
                                                &refCounting)) {
    switch (refCounting) {
    case ReferenceCounting::Native:
      witnessSurrogate = C.TheNativeObjectType;
      break;
    case ReferenceCounting::ObjC:
    case ReferenceCounting::Block:
    case ReferenceCounting::Unknown:
      witnessSurrogate = C.getAnyObjectType();
      break;
    case ReferenceCounting::Bridge:
      witnessSurrogate = C.TheBridgeObjectType;
      break;
    case ReferenceCounting::Error:
    case ReferenceCounting::None:
    case ReferenceCounting::Custom:
      break;
    }
  }

  if (witnessSurrogate) {
    if (relativeReference) {
      return IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                            LinkEntity::forValueWitnessTable(witnessSurrogate));
    } else {
      return {IGM.getAddrOfValueWitnessTable(witnessSurrogate),
              ConstantReference::Direct};
    }
  }
  return {};
}

llvm::Constant *
IRGenModule::getAddrOfEffectiveValueWitnessTable(CanType concreteType,
                                                 ConstantInit init) {
  if (auto known =
          getAddrOfKnownValueWitnessTable(*this, concreteType, false)) {
    return known.getValue();
  }
  return getAddrOfValueWitnessTable(concreteType);
}

/// Emit a value-witness table for the given type.
ConstantReference irgen::emitValueWitnessTable(IRGenModule &IGM,
                                             CanType abstractType,
                                             bool isPattern,
                                             bool relativeReference) {
  // See if we can use a prefab witness table from the runtime.
  if (!isPattern) {
    if (auto known = getAddrOfKnownValueWitnessTable(IGM, abstractType,
                                                     relativeReference)) {
      return known;
    }
  }
  
  // We should never be making a pattern if the layout isn't fixed.
  // The reverse can be true for types whose layout depends on
  // resilient types.
  assert((!isPattern || hasDependentValueWitnessTable(
              IGM, abstractType->getAnyNominal())) &&
         "emitting VWT pattern for fixed-layout type");

  ConstantInitBuilder builder(IGM);
  auto witnesses =
    builder.beginStruct(getValueWitnessTableType(IGM, abstractType));

  bool canBeConstant = false;
  addValueWitnessesForAbstractType(IGM, witnesses, abstractType, canBeConstant);

  // If this is just an instantiation pattern, we should never be modifying
  // it in-place.
  if (isPattern) canBeConstant = true;

  auto addr = IGM.getAddrOfValueWitnessTable(abstractType,
                                             witnesses.finishAndCreateFuture());
  auto global = cast<llvm::GlobalVariable>(addr);
  global->setConstant(canBeConstant);

  return {llvm::ConstantExpr::getBitCast(global, IGM.WitnessTablePtrTy),
          ConstantReference::Direct};
}

llvm::Constant *IRGenModule::emitFixedTypeLayout(CanType t,
                                                 const FixedTypeInfo &ti) {
  auto silTy = SILType::getPrimitiveAddressType(t);
  // Collect the interesting information that gets encoded in a type layout
  // record, to see if there's one we can reuse.
  unsigned size = ti.getFixedSize().getValue();
  unsigned align = ti.getFixedAlignment().getValue();

  bool pod = ti.isTriviallyDestroyable(ResilienceExpansion::Maximal);
  bool bt = ti.isBitwiseTakable(ResilienceExpansion::Maximal);
  unsigned numExtraInhabitants = ti.getFixedExtraInhabitantCount(*this);

  // Try to use common type layouts exported by the runtime.
  llvm::Constant *commonValueWitnessTable = nullptr;
  if (pod && bt && numExtraInhabitants == 0) {
    if (size == 0)
      commonValueWitnessTable =
        getAddrOfValueWitnessTable(Context.TheEmptyTupleType);
    if (   (size ==  1 && align ==  1)
        || (size ==  2 && align ==  2)
        || (size ==  4 && align ==  4)
        || (size ==  8 && align ==  8)
        || (size == 16 && align == 16)
        || (size == 32 && align == 32))
      commonValueWitnessTable =
        getAddrOfValueWitnessTable(BuiltinIntegerType::get(size * 8, Context)
                                     ->getCanonicalType());
  }

  if (commonValueWitnessTable) {
    auto index = llvm::ConstantInt::get(Int32Ty,
                               (unsigned)ValueWitness::First_TypeLayoutWitness);
    return llvm::ConstantExpr::getGetElementPtr(Int8PtrTy,
                                                commonValueWitnessTable,
                                                index);
  }

  // Otherwise, see if a layout has been emitted with these characteristics
  // already.
  FixedLayoutKey key{size, numExtraInhabitants, align, pod, bt};

  auto found = PrivateFixedLayouts.find(key);
  if (found != PrivateFixedLayouts.end())
    return found->second;

  // Emit the layout values.
  ConstantInitBuilder builder(*this);
  auto witnesses = builder.beginStruct();
  FixedPacking packing = ti.getFixedPacking(*this);
  for (auto witness = ValueWitness::First_TypeLayoutWitness;
       witness <= ValueWitness::Last_RequiredTypeLayoutWitness;
       witness = ValueWitness(unsigned(witness) + 1)) {
    addValueWitness(*this, witnesses, witness, packing, t, silTy, ti);
  }

  auto layoutVar
    = witnesses.finishAndCreateGlobal(
        "type_layout_" + llvm::Twine(size)
                       + "_" + llvm::Twine(align)
                       + "_" + llvm::Twine::utohexstr(numExtraInhabitants)
                       + (pod ? "_pod" :
                          bt  ? "_bt"  : ""),
                                      getPointerAlignment(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::PrivateLinkage);

  // Cast to the standard currency type for type layouts.
  auto layout = llvm::ConstantExpr::getBitCast(layoutVar, Int8PtrPtrTy);

  PrivateFixedLayouts.insert({key, layout});
  return layout;
}

FixedPacking TypeInfo::getFixedPacking(IRGenModule &IGM) const {
  auto fixedTI = dyn_cast<FixedTypeInfo>(this);

  // If the type isn't fixed, we have to do something dynamic.
  // FIXME: some types are provably too big (or aligned) to be
  // allocated inline.
  if (!fixedTI)
    return FixedPacking::Dynamic;

  // By convention we only store bitwise takable values inline.
  if (!fixedTI->isBitwiseTakable(ResilienceExpansion::Maximal))
    return FixedPacking::Allocate;

  Size bufferSize = getFixedBufferSize(IGM);
  Size requiredSize = fixedTI->getFixedSize();

  // Flat out, if we need more space than the buffer provides,
  // we always have to allocate.
  // FIXME: there might be some interesting cases where this
  // is suboptimal for enums.
  if (requiredSize > bufferSize)
    return FixedPacking::Allocate;

  Alignment bufferAlign = getFixedBufferAlignment(IGM);
  Alignment requiredAlign = fixedTI->getFixedAlignment();

  // If the buffer alignment is good enough for the type, great.
  if (bufferAlign >= requiredAlign)
    return FixedPacking::OffsetZero;

  // TODO: consider using a slower mode that dynamically checks
  // whether the buffer size is small enough.

  // Otherwise we're stuck and have to separately allocate.
  return FixedPacking::Allocate;
}

bool TypeInfo::canValueWitnessExtraInhabitantsUpTo(IRGenModule &IGM,
                                                   unsigned index) const {
  // If this type is POD, then its value witnesses are trivial, so can handle
  // any bit pattern.
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    return true;
  }
  
  // By default, assume that extra inhabitants must be branched out on.
  return false;
}

Address TypeInfo::indexArray(IRGenFunction &IGF, Address base,
                             llvm::Value *index, SILType T) const {
  // The stride of a Swift type may not match its LLVM size. If we know we have
  // a fixed stride different from our size, or we have a dynamic size,
  // do a byte-level GEP with the proper stride.
  const auto *fixedTI = dyn_cast<FixedTypeInfo>(this);

  llvm::Value *destValue = nullptr;
  Size stride(1);
  
  // TODO: Arrays currently lower-bound the stride to 1.
  if (!fixedTI || fixedTI->getFixedStride() != fixedTI->getFixedSize()) {
    llvm::Value *byteAddr = IGF.Builder.CreateBitCast(base.getAddress(),
                                                      IGF.IGM.Int8PtrTy);
    llvm::Value *size = getStride(IGF, T);
    if (size->getType() != index->getType())
      size = IGF.Builder.CreateZExtOrTrunc(size, index->getType());
    llvm::Value *distance = IGF.Builder.CreateNSWMul(index, size);
    destValue =
        IGF.Builder.CreateInBoundsGEP(IGF.IGM.Int8Ty, byteAddr, distance);
    destValue = IGF.Builder.CreateBitCast(destValue, base.getType());
  } else {
    // We don't expose a non-inbounds GEP operation.
    destValue = IGF.Builder.CreateInBoundsGEP(getStorageType(),
                                              base.getAddress(), index);
    stride = fixedTI->getFixedStride();
  }
  if (auto *IndexConst = dyn_cast<llvm::ConstantInt>(index)) {
    // If we know the indexing value, we can get a better guess on the
    // alignment.
    // This even works if the stride is not known (and assumed to be 1).
    stride *= IndexConst->getValue().getZExtValue();
  }
  Alignment Align = base.getAlignment().alignmentAtOffset(stride);
  return Address(destValue, getStorageType(), Align);
}

Address TypeInfo::roundUpToTypeAlignment(IRGenFunction &IGF, Address base,
                                         SILType T) const {
  Alignment Align = base.getAlignment();
  llvm::Value *TyAlignMask = getAlignmentMask(IGF, T);
  if (auto *TyAlignMaskConst = dyn_cast<llvm::ConstantInt>(TyAlignMask)) {
    Alignment TyAlign(TyAlignMaskConst->getZExtValue() + 1);

    // No need to align if the base is already aligned.
    if (TyAlign <= Align)
      return base;
  }
  llvm::Value *Addr = base.getAddress();
  Addr = IGF.Builder.CreatePtrToInt(Addr, IGF.IGM.IntPtrTy);
  Addr = IGF.Builder.CreateNUWAdd(Addr, TyAlignMask);
  llvm::Value *InvertedMask = IGF.Builder.CreateNot(TyAlignMask);
  Addr = IGF.Builder.CreateAnd(Addr, InvertedMask);
  Addr = IGF.Builder.CreateIntToPtr(Addr, getStorageType()->getPointerTo());
  return Address(Addr, getStorageType(), Align);
}

void TypeInfo::destroyArray(IRGenFunction &IGF, Address array,
                            llvm::Value *count, SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal))
    return;

 emitDestroyArrayCall(IGF, T, array, count);
}

void TypeInfo::initializeArrayWithCopy(IRGenFunction &IGF,
                                       Address dest, Address src,
                                       llvm::Value *count, SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemCpy(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitInitializeArrayWithCopyCall(IGF, T, dest, src, count);
}

void TypeInfo::initializeArrayWithTakeNoAlias(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const {
  if (isBitwiseTakable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemCpy(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitInitializeArrayWithTakeNoAliasCall(IGF, T, dest, src, count);
}

void TypeInfo::initializeArrayWithTakeFrontToBack(IRGenFunction &IGF,
                                                  Address dest, Address src,
                                                  llvm::Value *count, SILType T)
const {
  if (isBitwiseTakable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemMove(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitInitializeArrayWithTakeFrontToBackCall(IGF, T, dest, src, count);
}

void TypeInfo::initializeArrayWithTakeBackToFront(IRGenFunction &IGF,
                                                  Address dest, Address src,
                                                  llvm::Value *count, SILType T)
const {
  if (isBitwiseTakable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemMove(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitInitializeArrayWithTakeBackToFrontCall(IGF, T, dest, src, count);
}

void TypeInfo::assignArrayWithCopyNoAlias(IRGenFunction &IGF, Address dest,
                                          Address src, llvm::Value *count,
                                          SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemCpy(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitAssignArrayWithCopyNoAliasCall(IGF, T, dest, src, count);
}

void TypeInfo::assignArrayWithCopyFrontToBack(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemMove(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitAssignArrayWithCopyFrontToBackCall(IGF, T, dest, src, count);
}

void TypeInfo::assignArrayWithCopyBackToFront(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemMove(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitAssignArrayWithCopyBackToFrontCall(IGF, T, dest, src, count);
}

void TypeInfo::assignArrayWithTake(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const {
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    llvm::Value *stride = getStride(IGF, T);
    llvm::Value *byteCount = IGF.Builder.CreateNUWMul(stride, count);
    IGF.Builder.CreateMemCpy(
        dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
        src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
        byteCount);
    return;
  }

  emitAssignArrayWithTakeCall(IGF, T, dest, src, count);
}

void TypeInfo::collectMetadataForOutlining(OutliningMetadataCollector &c,
                                           SILType T) const {
  auto canType = T.getASTType();
  assert(!canType->is<ArchetypeType>() && "Did not expect an ArchetypeType");
  (void)canType;
}

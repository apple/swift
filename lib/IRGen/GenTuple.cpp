//===--- GenTuple.cpp - Swift IR Generation For Tuple Types ---------------===//
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
//  This file implements IR generation for tuple types in Swift.  This
//  includes creating the IR type as well as emitting the primitive access
//  operations.
//
//  It is assumed in several places in IR-generation that the
//  explosion schema of a tuple type is always equal to the appended
//  explosion schemas of the component types.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/IR/DerivedTypes.h"

#include "GenHeap.h"
#include "GenRecord.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Explosion.h"
#include "IndirectTypeInfo.h"
#include "NonFixedTypeInfo.h"
#include "ResilientTypeInfo.h"

#include "GenTuple.h"

#pragma clang diagnostic ignored "-Winconsistent-missing-override"

using namespace swift;
using namespace irgen;

namespace {
  /// A type implementation for tuple types with a dynamic number of
  /// elements, that is, that contain pack expansion types. For now,
  /// these are completely abstract.
  class DynamicTupleTypeInfo
      : public ResilientTypeInfo<DynamicTupleTypeInfo>
  {
  public:
    DynamicTupleTypeInfo(llvm::Type *T,
                         IsCopyable_t copyable)
      : ResilientTypeInfo(T, copyable, IsABIAccessible) {}

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
    }
  };
} // end anonymous namespace

const TypeInfo *
TypeConverter::convertDynamicTupleType(IsCopyable_t copyable) {
  llvm::Type *storageType = IGM.OpaqueTy;
  return new DynamicTupleTypeInfo(storageType, copyable);
}

namespace {
  class TupleFieldInfo : public RecordField<TupleFieldInfo> {
  public:
    TupleFieldInfo(unsigned index, StringRef name, const TypeInfo &type)
      : RecordField(type), Index(index), Name(name)
    {}

    /// The field index.
    const unsigned Index;
    const StringRef Name;

    StringRef getFieldName() const {
      return Name;
    }
    
    const TupleTypeElt &getField(SILType T) const {
      auto tup = T.castTo<TupleType>();
      
      return tup->getElement(Index);
    }
    
    SILType getType(IRGenModule&, SILType t) const {
      return t.getTupleElementType(Index);
    }
  };
  
  /// Project a tuple offset from a tuple metadata structure.
  static llvm::Value *loadTupleOffsetFromMetadata(IRGenFunction &IGF,
                                                  llvm::Value *metadata,
                                                  llvm::Value *index) {
    auto asTuple = IGF.Builder.CreateBitCast(metadata,
                                             IGF.IGM.TupleTypeMetadataPtrTy);

    llvm::Value *indices[] = {
        IGF.IGM.getSize(Size(0)),                   // (*tupleType)
        llvm::ConstantInt::get(IGF.IGM.Int32Ty, 3), //   .Elements
        index,                                      //     [index]
        llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1)  //       .Offset
    };
    auto slot = IGF.Builder.CreateInBoundsGEP(IGF.IGM.TupleTypeMetadataTy,
                                              asTuple, indices);

    std::string name;
    if (auto *constantIndex = dyn_cast<llvm::ConstantInt>(index))
      name = (metadata->getName() + "." +
              Twine(constantIndex->getValue().getLimitedValue()) + ".offset")
                .str();
    else
      name = (metadata->getName() + ".dynamic.offset").str();

    return IGF.Builder.CreateLoad(slot, IGF.IGM.Int32Ty,
                                  IGF.IGM.getPointerAlignment(), name);
  }

  static llvm::Value *loadTupleOffsetFromMetadata(IRGenFunction &IGF,
                                                  llvm::Value *metadata,
                                                  unsigned index) {
    return loadTupleOffsetFromMetadata(IGF, metadata,
                                       IGF.IGM.getSize(Size(index)));
  }

  /// Adapter for tuple types.
  template <class Impl, class Base>
  class TupleTypeInfoBase
      : public RecordTypeInfo<Impl, Base, TupleFieldInfo> {
    using super = RecordTypeInfo<Impl, Base, TupleFieldInfo>;

  protected:
    template <class... As>
    TupleTypeInfoBase(As &&...args) : super(std::forward<As>(args)...) {}

    using super::asImpl;

  public:
    /// Given a full tuple explosion, project out a single element.
    void projectElementFromExplosion(IRGenFunction &IGF,
                                     Explosion &tuple,
                                     unsigned fieldNo,
                                     Explosion &out) const {
      const TupleFieldInfo &field = asImpl().getFields()[fieldNo];

      // If the field requires no storage, there's nothing to do.
      if (field.isEmpty())
        return IGF.emitFakeExplosion(field.getTypeInfo(), out);
  
      // Otherwise, project from the base.
      auto fieldRange = field.getProjectionRange();
      ArrayRef<llvm::Value *> element = tuple.getRange(fieldRange.first,
                                                       fieldRange.second);
      out.add(element);
    }

    /// Given the address of a tuple, project out the address of a
    /// single element.
    Address projectFieldAddress(IRGenFunction &IGF,
                                Address addr,
                                SILType T,
                                const TupleFieldInfo &field) const {
      return asImpl().projectElementAddress(IGF, addr, T, field.Index);
    }

    /// Given the address of a tuple, project out the address of a
    /// single element.
    Address projectElementAddress(IRGenFunction &IGF,
                                  Address tuple,
                                  SILType T,
                                  unsigned fieldNo) const {
      const TupleFieldInfo &field = asImpl().getFields()[fieldNo];
      if (field.isEmpty())
        return field.getTypeInfo().getUndefAddress();

      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      return field.projectAddress(IGF, tuple, offsets);
    }

    /// Return the statically-known offset of the given element.
    llvm::Optional<Size> getFixedElementOffset(IRGenModule &IGM,
                                               unsigned fieldNo) const {
      const TupleFieldInfo &field = asImpl().getFields()[fieldNo];
      switch (field.getKind()) {
      case ElementLayout::Kind::Empty:
      case ElementLayout::Kind::EmptyTailAllocatedCType:
      case ElementLayout::Kind::Fixed:
        return field.getFixedByteOffset();
      case ElementLayout::Kind::InitialNonFixedSize:
        return Size(0);
      case ElementLayout::Kind::NonFixed:
        return llvm::None;
      }
      llvm_unreachable("bad element layout kind");
    }

    llvm::Optional<unsigned> getElementStructIndex(IRGenModule &IGM,
                                                   unsigned fieldNo) const {
      const TupleFieldInfo &field = asImpl().getFields()[fieldNo];
      if (field.isEmpty())
        return llvm::None;
      return field.getStructIndex();
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address src, SILType T,
                              bool isOutlined) const override {
      llvm_unreachable("unexploded tuple as argument?");
    }
    
    void verify(IRGenTypeVerifierFunction &IGF,
                llvm::Value *metadata,
                SILType tupleType) const override {
      auto fields = asImpl().getFields();
      for (unsigned i : indices(fields)) {
        const TupleFieldInfo &field = fields[i];
        switch (field.getKind()) {
        case ElementLayout::Kind::Fixed: {
          // Check that the fixed layout matches the layout in the tuple
          // metadata.
          auto fixedOffset = field.getFixedByteOffset();
          
          auto runtimeOffset = loadTupleOffsetFromMetadata(IGF, metadata, i);

          IGF.verifyValues(metadata, runtimeOffset,
                     IGF.IGM.getSize(fixedOffset),
                     llvm::Twine("offset of tuple element ") + llvm::Twine(i));
          break;
        }
        
        case ElementLayout::Kind::Empty:
        case ElementLayout::Kind::EmptyTailAllocatedCType:
        case ElementLayout::Kind::InitialNonFixedSize:
        case ElementLayout::Kind::NonFixed:
          continue;
        }
      }
    }
  };

  /// Type implementation for loadable tuples.
  class LoadableTupleTypeInfo final :
      public TupleTypeInfoBase<LoadableTupleTypeInfo, LoadableTypeInfo> {
  public:
    // FIXME: Spare bits between tuple elements.
    LoadableTupleTypeInfo(ArrayRef<TupleFieldInfo> fields,
                          unsigned explosionSize,
                          llvm::Type *ty,
                          Size size, SpareBitVector &&spareBits,
                          Alignment align,
                          IsTriviallyDestroyable_t isTriviallyDestroyable,
                          IsCopyable_t isCopyable,
                          IsFixedSize_t alwaysFixedSize)
      : TupleTypeInfoBase(fields, explosionSize,
                          ty, size, std::move(spareBits), align,
                          isTriviallyDestroyable,
                          isCopyable,
                          alwaysFixedSize)
      {}

    void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                          Size offset) const override {
      for (auto &field : getFields()) {
        auto fieldOffset = offset + field.getFixedByteOffset();
        cast<LoadableTypeInfo>(field.getTypeInfo())
          .addToAggLowering(IGM, lowering, fieldOffset);
      }
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      if (getFields().empty()) {
        return IGM.typeLayoutCache.getEmptyEntry();
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      // if (fields.size() == 1) {
      //   return fields[0];
      // }
      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
  };

  /// Type implementation for fixed-size but non-loadable tuples.
  class FixedTupleTypeInfo final :
      public TupleTypeInfoBase<FixedTupleTypeInfo,
                               IndirectTypeInfo<FixedTupleTypeInfo,
                                                FixedTypeInfo>>
  {
  public:
    // FIXME: Spare bits between tuple elements.
    FixedTupleTypeInfo(ArrayRef<TupleFieldInfo> fields, llvm::Type *ty,
                       Size size, SpareBitVector &&spareBits, Alignment align,
                       IsTriviallyDestroyable_t isTriviallyDestroyable,
                       IsBitwiseTakable_t isBT,
                       IsCopyable_t isCopyable,
                       IsFixedSize_t alwaysFixedSize)
      : TupleTypeInfoBase(fields, ty, size, std::move(spareBits), align,
                          isTriviallyDestroyable, isBT, isCopyable,
                          alwaysFixedSize)
    {}

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      if (getFields().empty()) {
        return IGM.typeLayoutCache.getEmptyEntry();
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      // if (fields.size() == 1) {
      //   return fields[0];
      // }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
  };

  /// An accessor for the non-fixed offsets for a tuple type.
  class TupleNonFixedOffsets : public NonFixedOffsetsImpl {
    // TODO: Should be a SILType.
    SILType TheType;
  public:
    TupleNonFixedOffsets(SILType type) : TheType(type) {
      assert(TheType.is<TupleType>());
    }

    llvm::Value *getOffsetForIndex(IRGenFunction &IGF, unsigned index) override {
      // Fetch the metadata as a tuple type.  We cache this because
      // we might repeatedly need the bitcast.
      auto metadata = IGF.emitTypeMetadataRefForLayout(TheType);
      return loadTupleOffsetFromMetadata(IGF, metadata, index);
    }
  };

  /// Type implementation for non-fixed-size tuples.
  class NonFixedTupleTypeInfo final :
      public TupleTypeInfoBase<NonFixedTupleTypeInfo,
                               WitnessSizedTypeInfo<NonFixedTupleTypeInfo>>
  {
  public:
    NonFixedTupleTypeInfo(ArrayRef<TupleFieldInfo> fields,
                          FieldsAreABIAccessible_t fieldsABIAccessible,
                          llvm::Type *T,
                          Alignment minAlign, IsTriviallyDestroyable_t isTriviallyDestroyable,
                          IsBitwiseTakable_t isBT,
                          IsCopyable_t isCopyable,
                          IsABIAccessible_t tupleAccessible)
      : TupleTypeInfoBase(fields, fieldsABIAccessible,
                          T, minAlign, isTriviallyDestroyable, isBT, isCopyable,
                          tupleAccessible) {
      }

    TupleNonFixedOffsets getNonFixedOffsets(IRGenFunction &IGF,
                                            SILType T) const {
      return TupleNonFixedOffsets(T);
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : asImpl().getFields()) {
        auto fieldTy = field.getType(IGM, T);
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }

      if (fields.empty()) {
        return IGM.typeLayoutCache.getEmptyEntry();
      }

      // if (fields.size() == 1) {
      //   return fields[0];
      // }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                         llvm::Value *numEmptyCases,
                                         Address structAddr,
                                         SILType structType,
                                         bool isOutlined) const override {
      // The runtime will overwrite this with a concrete implementation
      // in the value witness table.
      return emitGetEnumTagSinglePayloadCall(IGF, structType, numEmptyCases,
                                             structAddr);
    }

    void storeEnumTagSinglePayload(IRGenFunction &IGF,
                                   llvm::Value *index,
                                   llvm::Value *numEmptyCases,
                                   Address structAddr,
                                   SILType structType,
                                   bool isOutlined) const override {
      // The runtime will overwrite this with a concrete implementation
      // in the value witness table.
      emitStoreEnumTagSinglePayloadCall(IGF, structType, index,
                                        numEmptyCases, structAddr);
    }
  };

  class TupleTypeBuilder :
      public RecordTypeBuilder<TupleTypeBuilder, TupleFieldInfo,
                               TupleTypeElt> {
    SILType TheTuple;

  public:
    TupleTypeBuilder(IRGenModule &IGM, SILType theTuple)
      : RecordTypeBuilder(IGM), TheTuple(theTuple) {}

    FixedTupleTypeInfo *createFixed(ArrayRef<TupleFieldInfo> fields,
                                    StructLayout &&layout) {
      return FixedTupleTypeInfo::create(fields, layout.getType(),
                                        layout.getSize(),
                                        std::move(layout.getSpareBits()),
                                        layout.getAlignment(),
                                        layout.isTriviallyDestroyable(),
                                        layout.isBitwiseTakable(),
                                        layout.isCopyable(),
                                        layout.isAlwaysFixedSize());
    }

    LoadableTupleTypeInfo *createLoadable(ArrayRef<TupleFieldInfo> fields,
                                          StructLayout &&layout,
                                          unsigned explosionSize) {
      return LoadableTupleTypeInfo::create(fields, explosionSize,
                                           layout.getType(), layout.getSize(),
                                           std::move(layout.getSpareBits()),
                                           layout.getAlignment(),
                                           layout.isTriviallyDestroyable(),
                                           layout.isCopyable(),
                                           layout.isAlwaysFixedSize());
    }

    NonFixedTupleTypeInfo *createNonFixed(ArrayRef<TupleFieldInfo> fields,
                                     FieldsAreABIAccessible_t fieldsAccessible,
                                          StructLayout &&layout) {
      auto tupleAccessible = IsABIAccessible_t(
        IGM.isTypeABIAccessible(TheTuple));
      return NonFixedTupleTypeInfo::create(fields, fieldsAccessible,
                                           layout.getType(),
                                           layout.getAlignment(),
                                           layout.isTriviallyDestroyable(),
                                           layout.isBitwiseTakable(),
                                           layout.isCopyable(),
                                           tupleAccessible);
    }

    TupleFieldInfo getFieldInfo(unsigned index,
                                const TupleTypeElt &field,
                                const TypeInfo &fieldTI) {
      StringRef name = field.hasName() ? field.getName().str() : "elt";
      return TupleFieldInfo(index, name, fieldTI);
    }

    SILType getType(const TupleTypeElt &field) {
      // We know we're working with a lowered type here.
      return SILType::getPrimitiveObjectType(CanType(field.getType()));
    }

    StructLayout performLayout(ArrayRef<const TypeInfo *> fieldTypes) {
      return StructLayout(IGM, /*decl=*/nullptr, LayoutKind::NonHeapObject,
                          LayoutStrategy::Universal, fieldTypes);
    }
  };
} // end anonymous namespace

const TypeInfo *TypeConverter::convertTupleType(TupleType *tuple) {
  if (tuple->containsPackExpansionType()) {
    // FIXME: Figure out if its copyable at least
    return &getDynamicTupleTypeInfo(IsCopyable);
  }

  TupleTypeBuilder builder(IGM, SILType::getPrimitiveAddressType(CanType(tuple)));
  return builder.layout(tuple->getElements());
}

/// A convenient macro for delegating an operation to all of the
/// various tuple implementations.
#define FOR_TUPLE_IMPL(IGF, type, op, ...) do {                      \
  auto &tupleTI = IGF.getTypeInfo(type);                             \
  if (isa<LoadableTypeInfo>(tupleTI)) {                              \
    return tupleTI.as<LoadableTupleTypeInfo>().op(IGF, __VA_ARGS__); \
  } else if (isa<FixedTypeInfo>(tupleTI)) {                          \
    return tupleTI.as<FixedTupleTypeInfo>().op(IGF, __VA_ARGS__);    \
  } else {                                                           \
    return tupleTI.as<NonFixedTupleTypeInfo>().op(IGF, __VA_ARGS__); \
  }                                                                  \
} while (0)

void irgen::projectTupleElementFromExplosion(IRGenFunction &IGF,
                                             SILType tupleType,
                                             Explosion &tuple,
                                             unsigned fieldNo,
                                             Explosion &out) {
  FOR_TUPLE_IMPL(IGF, tupleType, projectElementFromExplosion,
                 tuple, fieldNo, out);
}

Address irgen::projectTupleElementAddress(IRGenFunction &IGF,
                                          Address tuple,
                                          SILType tupleType,
                                          unsigned fieldNo) {
  FOR_TUPLE_IMPL(IGF, tupleType, projectElementAddress, tuple,
                 tupleType, fieldNo);
}

Address irgen::projectTupleElementAddressByDynamicIndex(IRGenFunction &IGF,
                                                        Address tuple,
                                                        SILType tupleType,
                                                        llvm::Value *index,
                                                        SILType elementType) {
  auto *metadata = IGF.emitTypeMetadataRefForLayout(tupleType);


  llvm::BasicBlock *trueBB = nullptr, *falseBB = nullptr, *restBB = nullptr;
  llvm::BasicBlock *unwrappedBB = nullptr;
  llvm::Value *unwrappedOffset = nullptr;

  auto loweredTupleType = tupleType.castTo<TupleType>();
  if (loweredTupleType->getNumScalarElements() <= 1) {
    ConditionalDominanceScope scope(IGF);

    // Test if the runtime length of the pack type is exactly 1.
    CanPackType packType = loweredTupleType.getInducedPackType();
    auto *shapeExpression = IGF.emitPackShapeExpression(packType);
  
    auto *one = llvm::ConstantInt::get(IGF.IGM.SizeTy, 1);
    auto *isOne = IGF.Builder.CreateICmpEQ(shapeExpression, one);

    trueBB = IGF.createBasicBlock("vanishing-tuple");
    falseBB = IGF.createBasicBlock("actual-tuple");

    IGF.Builder.CreateCondBr(isOne, trueBB, falseBB);

    IGF.Builder.emitBlock(trueBB);

    // If the length is 1, the offset is just zero.
    unwrappedBB = IGF.Builder.GetInsertBlock();
    unwrappedOffset = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);

    restBB = IGF.createBasicBlock("tuple-rest");
    IGF.Builder.CreateBr(restBB);

    IGF.Builder.emitBlock(falseBB);
  }

  llvm::Value *tupleOffset = nullptr;
  llvm::BasicBlock *tupleBB = nullptr;

  {
    ConditionalDominanceScope scope(IGF);
    tupleOffset = loadTupleOffsetFromMetadata(IGF, metadata, index);

    tupleBB = IGF.Builder.GetInsertBlock();
  }

  // Control flow join with the one-element case.
  llvm::Value *result = nullptr;
  if (unwrappedOffset != nullptr) {
    IGF.Builder.CreateBr(restBB);
    IGF.Builder.emitBlock(restBB);

    auto *phi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, 2);
    phi->addIncoming(unwrappedOffset, unwrappedBB);
    phi->addIncoming(tupleOffset, tupleBB);

    result = phi;
  } else {
    result = tupleOffset;
  }

  auto *gep =
      IGF.emitByteOffsetGEP(tuple.getAddress(), result, IGF.IGM.OpaqueTy);
  auto elementAddress = Address(gep, IGF.IGM.OpaqueTy,
                                IGF.IGM.getPointerAlignment());
  return IGF.Builder.CreateElementBitCast(elementAddress,
                                          IGF.IGM.getStorageType(elementType));
}

llvm::Optional<Size> irgen::getFixedTupleElementOffset(IRGenModule &IGM,
                                                       SILType tupleType,
                                                       unsigned fieldNo) {
  // Macro happens to work with IGM, too.
  FOR_TUPLE_IMPL(IGM, tupleType, getFixedElementOffset, fieldNo);
}

llvm::Optional<unsigned>
irgen::getPhysicalTupleElementStructIndex(IRGenModule &IGM, SILType tupleType,
                                          unsigned fieldNo) {
  FOR_TUPLE_IMPL(IGM, tupleType, getElementStructIndex, fieldNo);
}
